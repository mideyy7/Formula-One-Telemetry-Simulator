#include "simulation/telemetry_generator.h"
#include <algorithm>
#include <numeric>
#include <chrono>

// ─── Constructor ─────────────────────────────────────────────────────────────

TelemetryGenerator::TelemetryGenerator(
    SpscQueue<TelemetryFrame, 2048>& queue,
    const TrackProfile& track)
    : queue_(queue), track_(track)
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

        s.latest_frame.driver_id = DRIVERS[i].id;
        s.latest_frame.lap       = 1;
        s.latest_frame.fuel_kg   = 100.0f;
        s.lap_start              = std::chrono::steady_clock::now();

        states_.push_back(std::move(s));
    }
}

// ─── Thread control ──────────────────────────────────────────────────────────

void TelemetryGenerator::start() {
    // The lambda takes a std::stop_token — jthread notices this and passes
    // one in automatically when the thread starts.
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void TelemetryGenerator::stop() {
    thread_.request_stop();     // flips the stop_token's flag
    if (thread_.joinable()) thread_.join(); // wait for run() to actually exit
}

std::vector<DriverState> TelemetryGenerator::standings() const {
    return states_; // returns a copy — safe to call from any thread
}

// ─── Main thread loop ────────────────────────────────────────────────────────

void TelemetryGenerator::run(std::stop_token st) {
    while (!st.stop_requested() && !race_finished_) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 50Hz
    }
}

// ─── Per-tick simulation ─────────────────────────────────────────────────────

void TelemetryGenerator::tick() {
    for (auto& state : states_) {
        auto& frame = state.latest_frame;

        // ── Pit stop in progress ──────────────────────────────────────────────
        if (state.in_pit) {
            handle_pit(state);
            queue_.push(frame);
            continue;
        }

        // ── Speed calculation ─────────────────────────────────────────────────
        // Factors that reduce top speed:
        //   fuel load:  heavy fuel = slower by up to 8%
        //   tire wear:  worn tires = slower by up to 25%
        //   random var: models driver variation lap-to-lap
        float fuel_factor = 1.0f - (frame.fuel_kg / 100.0f) * 0.08f;
        float tire_factor = 1.0f - frame.tire_wear * 0.25f;
        float variation   = dist_(rng_) * (1.0f - state.profile.consistency) * 8.0f;

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

            // Pit decision
            if (should_pit(state)) {
                state.in_pit           = true;
                state.pit_timer_ticks  = PIT_STOP_TICKS;
                state.has_completed_pit = true;
                frame.in_pit            = true;
            }

            // Update global race lap counter
            if (frame.lap > race_lap_) {
                race_lap_ = frame.lap;
            }
            if (frame.lap > TOTAL_LAPS && state.position == 1) {
                race_finished_ = true;
            }
        }

        // ── Push frame to consumer ────────────────────────────────────────────
        // If the queue is full the consumer is slow; drop the frame rather
        // than block the whole simulation on one lagging reader.
        queue_.push(frame);
    }

    // After all cars are updated, recalculate positions and DRS.
    update_positions_and_drs();

    if (on_tick_) on_tick_(states_);
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
    }
}

// ─── Pit strategy decision ────────────────────────────────────────────────────

bool TelemetryGenerator::should_pit(const DriverState& state) const {
    if (state.has_completed_pit) return false; // one stop only for now
    if (state.latest_frame.lap < 5) return false; // no early pits

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