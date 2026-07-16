#include "simulation/telemetry_generator.h"
#include <algorithm>
#include <numeric>
#include <chrono>
#include <cstring>

// ─── Constructor ─────────────────────────────────────────────────────────────

TelemetryGenerator::TelemetryGenerator(
    SpscQueue<TelemetryFrame, 2048>& queue,
    const TrackProfile& track)
    : queue_(queue), track_(track),
      tick_barrier_{5, [this] { on_tick_complete(); }}
{
    states_.reserve(DRIVERS.size());

    for (std::size_t i = 0; i < DRIVERS.size(); ++i) {
        DriverState s;
        s.profile  = DRIVERS[i];
        s.car      = car_for_driver(DRIVERS[i]);
        s.position = static_cast<int>(i + 1);

        // Stagger starting distances so cars aren't all on top of each other.
        // P1 starts at the front; each car is 0.010 km behind the previous.
        s.distance_in_lap = static_cast<float>(DRIVERS.size() - i - 1) * 0.010f;

        std::memcpy(s.latest_frame.driver_id, DRIVERS[i].id.data(), 3);
        s.latest_frame.lap       = 1;
        s.latest_frame.fuel_kg   = 100.0f;
        s.lap_start              = std::chrono::steady_clock::now();

        states_.push_back(std::move(s));
    }
}

// ─── Thread control ──────────────────────────────────────────────────────────

void TelemetryGenerator::start() {
    // jthread here is used purely for RAII auto-join — shutdown is
    // coordinated via shutdown_requested_ (see its declaration), not via
    // std::stop_token, so the lambdas take no stop_token parameter.
    thread_ = std::jthread([this] { run(); });

    // 4 workers x 5 drivers = 20. Not derived generically from states_.size()
    // on purpose — it keeps the barrier count (5 = chunk_workers_.size() + 1
    // pacer thread) obviously matched to chunk_workers_.size() at a glance.
    const std::size_t chunk = states_.size() / chunk_workers_.size();
    for (std::size_t i = 0; i < chunk_workers_.size(); ++i) {
        const std::size_t first = i * chunk;
        const std::size_t last  = (i + 1 == chunk_workers_.size())
                                 ? states_.size() : first + chunk;
        chunk_workers_[i] = std::jthread(
            [this, i, first, last] { chunk_worker(i, first, last); });
    }
}

void TelemetryGenerator::stop() {
    // Just signals intent. The actual "stop after this phase" agreement
    // between all 5 threads happens in on_tick_complete() via
    // phase_should_stop_ — see that field's declaration for why this
    // flag alone isn't safe to read directly from 5 independent threads.
    shutdown_requested_.store(true, std::memory_order_relaxed);

    if (thread_.joinable()) thread_.join();
    for (auto& w : chunk_workers_) if (w.joinable()) w.join();
}

void TelemetryGenerator::apply_pit_plan(const std::unordered_map<std::string, int>& plans) {
    for (auto& state : states_) {
        auto it = plans.find(state.profile.id);
        if (it != plans.end()) state.planned_pit_lap = it->second;
    }
}

std::vector<DriverState> TelemetryGenerator::standings() const {
    return states_; // returns a copy — safe to call from any thread
}

// ─── Pacer thread loop ───────────────────────────────────────────────────────

void TelemetryGenerator::run() {
    if (start_gate_) start_gate_->arrive_and_wait();

    while (true) {
        // Waits for all 4 chunk workers to finish this tick's physics, then
        // runs on_tick_complete() (via the barrier's completion function)
        // exactly once before releasing everyone into the next phase.
        tick_barrier_.arrive_and_wait();
        if (phase_should_stop_) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 50Hz
    }
}

// ─── Chunk worker loop ───────────────────────────────────────────────────────

void TelemetryGenerator::chunk_worker(std::size_t worker_idx, std::size_t first, std::size_t last) {
    while (true) {
        process_chunk(worker_idx, first, last);

        // Release-publishes this worker's writes to its slice of states_.
        // on_tick_complete() pairs this with an acquire-load before it
        // touches states_ at all — see workers_done_'s declaration.
        workers_done_.fetch_add(1, std::memory_order_release);

        tick_barrier_.arrive_and_wait();
        if (phase_should_stop_) break;
    }
}

// ─── Per-tick simulation (runs on 4 chunk-worker threads, disjoint ranges) ──

void TelemetryGenerator::process_chunk(std::size_t worker_idx, std::size_t first, std::size_t last) {
    for (std::size_t i = first; i < last; ++i) {
        auto& state = states_[i];
        auto& frame = state.latest_frame;

        // ── Pit lane gate ─────────────────────────────────────────────────────
        // A driver that crossed the pit threshold doesn't enter until a
        // PitLane slot is free — try_enter() is non-blocking so a busy pit
        // lane just means "stay out, retry next tick" instead of stalling
        // this thread's whole chunk.
        if (state.wants_to_pit && !state.in_pit) {
            if (pit_lane_.try_enter(std::chrono::milliseconds(0))) {
                state.in_pit           = true;
                state.wants_to_pit     = false;
                state.pit_timer_ticks  = PIT_STOP_TICKS;
                state.has_completed_pit = true;
                frame.in_pit            = true;
            }
        }

        // ── Pit stop in progress ──────────────────────────────────────────────
        if (state.in_pit) {
            handle_pit(state);
            continue;
        }

        // ── Speed calculation ─────────────────────────────────────────────────
        // Factors that reduce top speed:
        //   fuel load:  heavy fuel = slower by up to 8%
        //   tire wear:  worn tires = slower by up to 25%
        //   random var: models driver variation lap-to-lap
        float fuel_factor = 1.0f - (frame.fuel_kg / 100.0f) * 0.08f;
        float tire_factor = 1.0f - frame.tire_wear * 0.25f;
        float variation   = dists_[worker_idx](rngs_[worker_idx]) * (1.0f - state.profile.consistency) * 8.0f;

        frame.speed_kph = max_speed(state.car) * fuel_factor * tire_factor
                        + variation
                        + (frame.drs_active ? 12.0f : 0.0f);
        frame.speed_kph = std::clamp(frame.speed_kph, 80.0f, 370.0f);

        // ── Throttle / brake (simplified model) ───────────────────────────────
        frame.throttle = std::min(1.0f, frame.speed_kph / 310.0f);
        frame.brake    = (frame.speed_kph < 150.0f)
                       ? std::clamp((150.0f - frame.speed_kph) / 150.0f, 0.0f, 1.0f)
                       : 0.0f;

        // ── Distance and sector ───────────────────────────────────────────────
        float dist_km = frame.speed_kph / 3600.0f * SIM_S_PER_TICK;
        state.distance_in_lap += dist_km;

        float sector_len = track_.length_km / 3.0f;
        frame.sector = std::min(3,
            static_cast<int>(state.distance_in_lap / sector_len) + 1);

        // ── Tire wear ─────────────────────────────────────────────────────────
        float wear = TIRE_WEAR_BASE
                   * state.profile.aggression
                   * track_.tire_deg_factor
                   * (frame.speed_kph / 280.0f);
        frame.tire_wear = std::min(1.0f, frame.tire_wear + wear);

        // ── Fuel burn ─────────────────────────────────────────────────────────
        float fuel_burn = track_.fuel_consumption * dist_km;
        frame.fuel_kg   = std::max(0.0f, frame.fuel_kg - fuel_burn);

        // ── Lap completion ────────────────────────────────────────────────────
        if (state.distance_in_lap >= track_.length_km) {
            state.distance_in_lap -= track_.length_km;
            frame.lap++;

            // Record lap time
            auto now     = std::chrono::steady_clock::now();
            float lap_ms = std::chrono::duration<float, std::milli>(
                               now - state.lap_start).count();
            state.lap_start = now;

            if (state.best_lap_ms < 1.0f || lap_ms < state.best_lap_ms) {
                state.best_lap_ms = lap_ms;
            }

            // Pit decision — this only raises the flag; actual entry is
            // gated by the PitLane semaphore above.
            if (should_pit(state)) {
                state.wants_to_pit = true;
            }

            // race_lap_/race_finished_ are NOT updated here on purpose: two
            // different chunk workers could both complete a lap in the same
            // tick, and both writing the same shared ints with no
            // synchronization would be a data race (independent of the
            // barrier/workers_done_ discussion above — this one is a plain
            // concurrent read-modify-write within a single phase). They're
            // computed once, serialized, in on_tick_complete() instead.
        }
    }
}

// ─── Lap barrier completion (guaranteed to run exactly once per tick) ───────
//
// This is the ONLY place that calls queue_.push(). SpscQueue is single-
// producer: two concurrent push() calls would race on tail_ even if they
// touched unrelated payloads. std::barrier's completion function is
// guaranteed to run once per phase, never concurrently with itself, so
// serializing all pushes here preserves the "single producer" contract even
// though the physics above just ran across 4 different threads.

void TelemetryGenerator::on_tick_complete() {
    // Acquire-gate: the actual happens-before edge for states_ that this
    // whole function depends on. Pairs with each chunk worker's
    // workers_done_.fetch_add(release) — NOT with tick_barrier_'s own
    // completion-function guarantee, which a minimal repro showed
    // ThreadSanitizer cannot verify on this platform (see workers_done_'s
    // declaration). By the time tick_barrier_ has collected all 5 arrivals
    // to even invoke this function, all 4 workers have already done their
    // fetch_add, so in practice this loop reads 4 on the first try — but
    // it's the acquire load itself, not the loop, that does the work.
    while (workers_done_.load(std::memory_order_acquire)
           != static_cast<int>(chunk_workers_.size())) {
        // spin — see comment above for why this is expected not to.
    }

    update_positions_and_drs();

    // race_lap_/race_finished_ computed once, here, serialized, instead of
    // inside process_chunk() — see the comment there for why letting
    // multiple chunk workers write these shared scalars directly would
    // itself be a data race, independent of the barrier discussion above.
    for (const auto& state : states_) {
        if (state.latest_frame.lap > race_lap_) race_lap_ = state.latest_frame.lap;
    }
    for (const auto& state : states_) {
        if (state.position == 1 && state.latest_frame.lap > TOTAL_LAPS) {
            race_finished_ = true;
        }
    }

    for (auto& state : states_) {
        // If the queue is full the consumer is slow; drop the frame rather
        // than block the whole simulation on one lagging reader.
        queue_.push(state.latest_frame);
    }

    if (on_tick_) on_tick_(states_);

    if (race_finished_) {
        shutdown_requested_.store(true, std::memory_order_relaxed);
    }

    // The single, race-free "stop after this phase" decision — see
    // phase_should_stop_'s declaration for why this indirection through the
    // completion function (rather than each of the 5 threads independently
    // reading shutdown_requested_) is required for correctness.
    phase_should_stop_ = shutdown_requested_.load(std::memory_order_relaxed);

    // Reset for the next phase. Safe without extra synchronization: every
    // chunk worker is still blocked inside tick_barrier_.arrive_and_wait()
    // until this function returns (the phase-counting guarantee, which is
    // not in question here), so none of them can call fetch_add() again
    // until after this store has happened. workers_done_ being atomic also
    // means the store/fetch_add sequence on the counter itself is always
    // coherently ordered regardless of memory_order — that guarantee is
    // unconditional for a single atomic object.
    workers_done_.store(0, std::memory_order_relaxed);
}

// ─── Pit stop handling ────────────────────────────────────────────────────────

void TelemetryGenerator::handle_pit(DriverState& state) {
    auto& frame = state.latest_frame;
    state.pit_timer_ticks--;
    frame.in_pit    = true;
    frame.speed_kph = 60.0f; // pit lane speed limit
    frame.throttle  = 0.2f;
    frame.brake     = 0.1f;

    if (state.pit_timer_ticks <= 0) {
        state.in_pit    = false;
        frame.in_pit    = false;
        frame.tire_wear = 0.02f;  // fresh tires (slight installation wear)
        frame.fuel_kg   = 100.0f; // refueled to max
        state.pit_stops++;
        pit_lane_.exit(); // free the slot for the next queued driver
    }
}

// ─── Pit strategy decision ────────────────────────────────────────────────────

bool TelemetryGenerator::should_pit(const DriverState& state) const {
    if (state.has_completed_pit) return false; // one stop only for now
    if (state.latest_frame.lap < 5) return false; // no early pits

    // A StrategyAnalyzer plan (if applied) overrides the flat wear
    // threshold — it already accounts for tire degradation and fuel burn
    // across the whole race, not just the current tick.
    if (state.planned_pit_lap > 0) {
        return state.latest_frame.lap >= state.planned_pit_lap;
    }

    // Aggressive drivers pit later (higher wear threshold).
    float threshold = 0.60f + state.profile.tire_mgmt * 0.25f;
    return state.latest_frame.tire_wear >= threshold;
}

// ─── Position and DRS update ──────────────────────────────────────────────────

void TelemetryGenerator::update_positions_and_drs() {
    // Sort by total race distance (lap * track_length + distance_in_lap).
    std::vector<int> idx(states_.size());
    std::iota(idx.begin(), idx.end(), 0);

    std::stable_sort(idx.begin(), idx.end(), [this](int a, int b) {
        float da = states_[a].latest_frame.lap * track_.length_km
                 + states_[a].distance_in_lap;
        float db = states_[b].latest_frame.lap * track_.length_km
                 + states_[b].distance_in_lap;
        return da > db; // descending: further ahead = lower position number
    });

    // Assign positions and gaps.
    for (int p = 0; p < static_cast<int>(idx.size()); ++p) {
        auto& s = states_[idx[p]];
        s.position = p + 1;

        if (p == 0) {
            s.latest_frame.gap_to_leader = 0.0f;
        } else {
            // Gap in seconds: distance difference / average speed x 3600.
            float d_leader = states_[idx[0]].latest_frame.lap * track_.length_km
                           + states_[idx[0]].distance_in_lap;
            float d_this   = s.latest_frame.lap * track_.length_km
                           + s.distance_in_lap;
            float gap_km   = d_leader - d_this;
            float avg_spd  = (states_[idx[0]].latest_frame.speed_kph +
                              s.latest_frame.speed_kph) / 2.0f;
            s.latest_frame.gap_to_leader =
                gap_km / std::max(avg_spd, 1.0f) * 3600.0f;
        }
    }

    // DRS: enabled if within 1 second of the car directly ahead.
    states_[idx[0]].latest_frame.drs_active = false; // leader has no DRS
    for (int p = 1; p < static_cast<int>(idx.size()); ++p) {
        float gap = states_[idx[p]].latest_frame.gap_to_leader
                  - states_[idx[p-1]].latest_frame.gap_to_leader;
        states_[idx[p]].latest_frame.drs_active = (gap < 1.0f);
    }
}

// ─── Utility ──────────────────────────────────────────────────────────────────

float TelemetryGenerator::max_speed(const CarProfile& car) {
    // Top F1 cars hit ~355-370 kph; back-markers around 290-310 kph.
    return 280.0f + car.engine_power * 90.0f;
}