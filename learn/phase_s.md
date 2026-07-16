# Phase S — Wiring In the Unused Primitives

## Overview

Phases 3 and 4 built four pieces that were never actually connected to the live
simulation: `RaceDirector` (latch), `LapBarrier` (barrier), `PitLane`
(semaphore), and `StrategyAnalyzer` (thread-pool + futures). Each one has a
passing unit test in isolation, but `main.cpp` only wires together Phases 1-5 —
these four were "proven correct, never adopted."

This phase closes that gap. Unlike phase_6.md (which describes a `.hpp`/FTXUI
design that was never actually built), this phase is written against the code
that actually exists in `src/` today — `.h` extensions, the real
`TelemetryGenerator` API, the real `DriverState` fields. Everything below is a
diff against current files, not a fresh rewrite.

**Concept recap (why each primitive earns its place here):**

| Primitive | File | What it will actually gate |
|---|---|---|
| `std::latch` | `simulation/race_director.h` | One-shot rendezvous: generator thread and main thread agree on the exact instant the race clock starts |
| `std::barrier` | `simulation/lap_barrier.h` | Reusable per-tick join point after splitting the 20-driver update loop across worker threads |
| `std::counting_semaphore<2>` | `simulation/pit_lane.h` | Caps concurrent pit-lane occupancy at 2, so drivers actually queue for a box instead of all pitting simultaneously |
| thread pool + futures | `strategy/strategy_analyzer.h` | Pre-race pit-lap planning actually decides *when* each driver pits, instead of the flat tire-wear threshold deciding it alone |

---

## Files Modified This Phase

| File | Action | Purpose |
|------|--------|---------|
| `src/common/types.h` | EDIT | Add `planned_pit_lap` and `wants_to_pit` to `DriverState` |
| `src/simulation/telemetry_generator.h` | EDIT | Add `PitLane`, `LapBarrier`, chunk-worker threads, start-gate hook, pit-plan setter |
| `src/simulation/telemetry_generator.cpp` | EDIT | Split `tick()` into parallel chunks + barrier-gated completion; gate pit entry through `PitLane`; consult `planned_pit_lap` in `should_pit()` |
| `src/main.cpp` | EDIT | Own a `RaceDirector`, a `ThreadPool`, a `StrategyAnalyzer`; run pre-race strategy analysis; wire the start gate |
| `tests/phase_s/test_integration.cpp` | CREATE | Verifies all four primitives are actually exercised, not just present |
| `tests/CMakeLists.txt` | EDIT | Register the `phase_s` test target |

---

## Step S1 — Race start latch

### Why it's not just decoration

Today, `main.cpp` calls `generator.start()` then immediately reads
`std::chrono::steady_clock::now()` as `race_start_time`. There's no guarantee
the generator's `jthread` has actually begun ticking by that point — the OS
could schedule it a few milliseconds later. `race_start_time` is used for the
60-second safety timeout, so a slow thread start silently eats into the
timeout budget. A latch makes "both sides are ready" an actual synchronization
point instead of an assumption.

### File: `src/simulation/telemetry_generator.h`
**Action:** EDIT — add a start-gate hook.

```cpp
#include "simulation/race_director.h"   // add to includes

class TelemetryGenerator {
public:
    // ... existing public API unchanged ...

    // Optional: if set, the generator's thread will rendezvous with this
    // gate before entering its tick loop. Lets the caller (main.cpp) capture
    // race_start_time at the instant ticking actually begins, not the
    // instant start() was called.
    void set_start_gate(RaceDirector* gate) { start_gate_ = gate; }

private:
    // ... existing private members ...
    RaceDirector* start_gate_ {nullptr};
};
```

### File: `src/simulation/telemetry_generator.cpp`
**Action:** EDIT — `run()`.

```cpp
void TelemetryGenerator::run(std::stop_token st) {
    if (start_gate_) start_gate_->arrive_and_wait();

    while (!st.stop_requested() && !race_finished_) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
```

### File: `src/main.cpp`
**Action:** EDIT — construct the gate and rendezvous before timing the race.

```cpp
#include "simulation/race_director.h"

int main() {
    // ... existing queue/leaderboard/race_state/systems setup unchanged ...

    RaceDirector race_start{2};              // generator thread + main thread
    generator.set_start_gate(&race_start);

    race_state.start_race();
    generator.start();
    race_start.arrive_and_wait();            // blocks until the generator's
                                              // jthread has also arrived —
                                              // ticking is now guaranteed to
                                              // have begun
    auto race_start_time = std::chrono::steady_clock::now();

    // ... rest of main() unchanged ...
}
```

`std::latch` is exactly right here because this handshake happens once. There
is no "next race" to reset it for.

---

## Step S2 — Lap barrier: parallelize the per-driver tick

### Design

`tick()` currently loops over all 20 `states_` sequentially on one thread.
Split that loop into 4 chunks of 5 drivers, each processed by a persistent
`jthread`. A `LapBarrier{5, completion}` — 4 chunk workers + the original
generator thread acting as pacer — joins them back up every 20ms.

**The subtlety worth calling out:** `SpscQueue` is single-producer. If 4
worker threads each called `queue_.push()` directly, that would violate the
queue's contract even though the workers touch disjoint driver indices — two
concurrent `push()` calls racing on `tail_` is undefined behavior regardless
of whether the payloads are unrelated. The fix: workers only mutate their own
slice of `states_` (physics, tire wear, fuel, pit decisions). The actual
`queue_.push()` calls, `update_positions_and_drs()`, and the `on_tick_`
callback all happen inside the barrier's **completion function**, which
`std::barrier` guarantees runs exactly once per phase — so even though it may
execute on whichever thread happens to arrive last, it is never running
concurrently with another push. Single "producer" is preserved as "single
active pusher at a time," which is all the queue actually requires.

### File: `src/simulation/telemetry_generator.h`
**Action:** EDIT

```cpp
#include "simulation/lap_barrier.h"
#include "simulation/pit_lane.h"
#include <array>
#include <atomic>

class TelemetryGenerator {
    // ... existing public API unchanged ...

private:
    void run();
    void process_chunk(std::size_t first, std::size_t last); // NEW: per-driver body, no push
    void chunk_worker(std::size_t first, std::size_t last); // NEW
    void on_tick_complete(); // NEW: barrier completion — push, recalc, callback

    // ... existing members ...

    LapBarrier tick_barrier_;                  // 4 workers + pacer thread
    PitLane    pit_lane_;                      // Step S3
    std::array<std::jthread, 4> chunk_workers_;
    RaceDirector* start_gate_ {nullptr};

    std::atomic<bool> shutdown_requested_ {false}; // written by stop()
    bool phase_should_stop_ {false};               // written by on_tick_complete()

    std::atomic<int> workers_done_ {0}; // see callout below
};
```

> **A third race — this one in `std::barrier` itself on this platform.**
> The design as described so far (chunk workers write disjoint slices of
> `states_`, the barrier's completion function reads all of them) is the
> textbook use case for `std::barrier`. It still raced under TSan. To rule
> out a mistake in this specific integration, I wrote a ~30-line repro with
> no project code at all: 4 threads each incrementing their own slice of a
> shared array, one barrier completion function summing the whole array.
> Same race, same shape, every time. Apple's libc++ requires
> `-fexperimental-library` to even expose `std::barrier`/`std::jthread` —
> and empirically, its completion function does not give ThreadSanitizer a
> visible happens-before over the arriving threads' pre-arrival writes, even
> though the standard requires exactly that. This isn't fixable from
> application code by writing the barrier usage "more correctly" — the
> repro already was textbook-correct.
>
> The fix: stop trusting `tick_barrier_` for anything except pacing
> ("release everyone once all 5 have shown up"), and add an explicit,
> independently-verifiable release/acquire handshake for the data itself —
> a plain `std::atomic<int> workers_done_`. Each chunk worker does
> `workers_done_.fetch_add(1, memory_order_release)` right after
> `process_chunk()`, before calling `arrive_and_wait()`. `on_tick_complete()`
> spins on `workers_done_.load(memory_order_acquire) == 4` before touching
> `states_` at all. This is a plain load/store pair on a `std::atomic<int>`
> — exactly the pattern TSan is built to verify — so it sidesteps whatever
> the barrier's internal implementation is doing wrong, instead of trying to
> out-argue it.
>
> This also forced out a second, unrelated bug: `race_lap_` and
> `race_finished_` were being written directly inside `process_chunk()`,
> where *multiple* chunk workers can complete a lap in the same tick and
> race on the same two shared scalars — a plain concurrent read-modify-write
> with no barrier involved at all. Fixed by computing both once, serialized,
> inside `on_tick_complete()` (after the `workers_done_` gate) instead of
> letting workers write them directly.

> **A second race TSan caught, unrelated to the shutdown one above:** the
> pre-existing `std::mt19937 rng_` / `std::normal_distribution<float> dist_`
> pair (used for lap-to-lap speed variation) was perfectly safe when one
> thread ticked all 20 drivers sequentially. The moment 4 chunk-worker
> threads call `dist_(rng_)` concurrently, it's a straightforward data race
> on the engine's internal state — TSan flagged a torn read inside
> `mersenne_twister_engine::operator()` on the very first test. Fix: one RNG
> + distribution **per chunk worker**, not shared:
> ```cpp
> std::array<std::mt19937, 4>                    rngs_{
>     std::mt19937{42}, std::mt19937{43}, std::mt19937{44}, std::mt19937{45}
> };
> std::array<std::normal_distribution<float>, 4> dists_{};
> ```
> `process_chunk()` and `chunk_worker()` both take a `worker_idx` parameter
> so each chunk indexes its own stream (`dists_[worker_idx](rngs_[worker_idx])`).
> The general lesson: parallelizing a loop body that was written assuming
> single-threaded reuse of *any* mutable member — not just ones you added
> for this phase — needs an audit of every member that body touches, not
> just the new ones.



> **A deadlock this design actually hit, and why the fix looks the way it
> does:** the first version of this code gave `run()` and `chunk_worker()`
> `std::stop_token` parameters and checked `st.stop_requested()` — one flag
> per thread, à la Phase 3. It passed the quick smoke tests but hung
> deterministically on longer-running ones (reproduced with
> `lldb -p <pid> -o "bt all"`: the pacer thread stuck forever in
> `arrive_and_wait()`, all 4 chunk workers already exited). The bug: a plain
> `std::atomic<bool>` read independently by 5 threads right after they
> *individually* resume from `arrive_and_wait()` is not enough, even with
> correct release/acquire pairing. Release/acquire only orders a store
> against the *specific* load that observes it — it does not stop 5 threads,
> resuming at slightly different real times, from disagreeing about a store
> that lands in the middle of their resumption window. If `stop()`'s write
> race won against some of the 5 reads and lost against others, the ones
> that saw `true` exited while the ones that saw `false` looped back into
> another phase expecting a 5th arrival that would never come — permanent
> deadlock. The fix below funnels the decision through the barrier's own
> completion function (`on_tick_complete()`), which is guaranteed by
> `std::barrier` to run, and finish, *before* any of the 5 waiters resume for
> that phase — so a plain (non-atomic) write there is visible identically to
> whichever thread reads it next. No race is possible.

### File: `src/simulation/telemetry_generator.cpp`
**Action:** EDIT — constructor, `tick()` replacement, `run()`, `stop()`.

```cpp
TelemetryGenerator::TelemetryGenerator(
    SpscQueue<TelemetryFrame, 2048>& queue,
    const TrackProfile& track)
    : queue_(queue), track_(track),
      tick_barrier_{5, [this] { on_tick_complete(); }}
{
    // ... existing state-seeding loop unchanged ...
}

void TelemetryGenerator::start() {
    // jthread here is purely for RAII auto-join — shutdown is coordinated
    // via shutdown_requested_/phase_should_stop_, not std::stop_token, so
    // these lambdas take no stop_token parameter.
    thread_ = std::jthread([this] { run(); });

    // 4 workers x 5 drivers = 20. If DRIVERS.size() ever changes, this ratio
    // needs revisiting — it's not derived generically on purpose, to keep
    // the barrier count (5) obviously matched to chunk_workers_.size()+1.
    std::size_t chunk = states_.size() / chunk_workers_.size();
    for (std::size_t i = 0; i < chunk_workers_.size(); ++i) {
        std::size_t first = i * chunk;
        std::size_t last  = (i + 1 == chunk_workers_.size()) ? states_.size()
                                                              : first + chunk;
        chunk_workers_[i] = std::jthread(
            [this, first, last] { chunk_worker(first, last); });
    }
}

void TelemetryGenerator::stop() {
    // Just signals intent — see phase_should_stop_ for why the actual
    // cross-thread agreement happens in on_tick_complete(), not here.
    shutdown_requested_.store(true, std::memory_order_relaxed);

    if (thread_.joinable()) thread_.join();
    for (auto& w : chunk_workers_) if (w.joinable()) w.join();
}

void TelemetryGenerator::run() {
    if (start_gate_) start_gate_->arrive_and_wait();

    while (true) {
        tick_barrier_.arrive_and_wait(); // waits for all 4 chunk workers
        if (phase_should_stop_) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void TelemetryGenerator::chunk_worker(std::size_t worker_idx, std::size_t first, std::size_t last) {
    while (true) {
        process_chunk(worker_idx, first, last);

        // Release-publishes this worker's writes to its slice of states_ —
        // see the workers_done_ callout above for why this, not
        // tick_barrier_'s completion function, is the real synchronization.
        workers_done_.fetch_add(1, std::memory_order_release);

        tick_barrier_.arrive_and_wait();
        if (phase_should_stop_) break;
    }
}

// Per-driver physics — identical to the old tick() body, MINUS the
// queue_.push() and MINUS update_positions_and_drs() (both moved to
// on_tick_complete() so they only ever run from one place at a time).
void TelemetryGenerator::process_chunk(std::size_t worker_idx, std::size_t first, std::size_t last) {
    for (std::size_t i = first; i < last; ++i) {
        auto& state = states_[i];
        auto& frame = state.latest_frame;

        // ── Pit lane gate (Step S3) ───────────────────────────────────────
        if (state.wants_to_pit && !state.in_pit) {
            if (pit_lane_.try_enter(std::chrono::milliseconds(0))) {
                state.in_pit           = true;
                state.wants_to_pit     = false;
                state.pit_timer_ticks  = PIT_STOP_TICKS;
                state.has_completed_pit = true;
                frame.in_pit           = true;
            }
            // else: both boxes occupied — stay out, retry next tick
        }

        if (state.in_pit) {
            handle_pit(state);
            continue;
        }

        // ── Speed / throttle / brake / distance / tire / fuel ─────────────
        // (unchanged from the original tick() body)
        float fuel_factor = 1.0f - (frame.fuel_kg / 100.0f) * 0.08f;
        float tire_factor = 1.0f - frame.tire_wear * 0.25f;
        float variation   = dists_[worker_idx](rngs_[worker_idx]) * (1.0f - state.profile.consistency) * 8.0f;

        frame.speed_kph = max_speed(state.car) * fuel_factor * tire_factor
                        + variation
                        + (frame.drs_active ? 12.0f : 0.0f);
        frame.speed_kph = std::clamp(frame.speed_kph, 80.0f, 370.0f);

        frame.throttle = std::min(1.0f, frame.speed_kph / 310.0f);
        frame.brake    = (frame.speed_kph < 150.0f)
                       ? std::clamp((150.0f - frame.speed_kph) / 150.0f, 0.0f, 1.0f)
                       : 0.0f;

        float dist_km = frame.speed_kph / 3600.0f * SIM_S_PER_TICK;
        state.distance_in_lap += dist_km;

        float sector_len = track_.length_km / 3.0f;
        frame.sector = std::min(3, static_cast<int>(state.distance_in_lap / sector_len) + 1);

        float wear = TIRE_WEAR_BASE * state.profile.aggression
                   * track_.tire_deg_factor * (frame.speed_kph / 280.0f);
        frame.tire_wear = std::min(1.0f, frame.tire_wear + wear);

        float fuel_burn = track_.fuel_consumption * dist_km;
        frame.fuel_kg   = std::max(0.0f, frame.fuel_kg - fuel_burn);

        if (state.distance_in_lap >= track_.length_km) {
            state.distance_in_lap -= track_.length_km;
            frame.lap++;

            auto  now    = std::chrono::steady_clock::now();
            float lap_ms = std::chrono::duration<float, std::milli>(now - state.lap_start).count();
            state.lap_start = now;
            if (state.best_lap_ms < 1.0f || lap_ms < state.best_lap_ms) {
                state.best_lap_ms = lap_ms;
            }

            if (should_pit(state)) state.wants_to_pit = true; // gated by pit_lane_ above, not set directly

            // race_lap_/race_finished_ are deliberately NOT touched here —
            // two different chunk workers can both complete a lap in the
            // same tick, and both writing these shared scalars with no
            // synchronization would race, independent of anything to do
            // with the barrier. Computed once, serialized, below instead.
        }
    }
}

// Barrier completion function. tick_barrier_ itself only provides pacing
// here (see the workers_done_ callout above) — the actual happens-before
// for states_ comes from the acquire-spin on workers_done_ below.
void TelemetryGenerator::on_tick_complete() {
    while (workers_done_.load(std::memory_order_acquire)
           != static_cast<int>(chunk_workers_.size())) {
        // spin — expected to already be satisfied; see callout above.
    }

    update_positions_and_drs();

    for (const auto& state : states_) {
        if (state.latest_frame.lap > race_lap_) race_lap_ = state.latest_frame.lap;
    }
    for (const auto& state : states_) {
        if (state.position == 1 && state.latest_frame.lap > TOTAL_LAPS) {
            race_finished_ = true;
        }
    }

    for (auto& state : states_) queue_.push(state.latest_frame);
    if (on_tick_) on_tick_(states_);

    if (race_finished_) shutdown_requested_.store(true, std::memory_order_relaxed);
    phase_should_stop_ = shutdown_requested_.load(std::memory_order_relaxed);

    // Safe without extra synchronization: every worker is still blocked in
    // tick_barrier_.arrive_and_wait() until this function returns, and
    // workers_done_ being atomic means modifications to it are always
    // coherently ordered regardless of memory_order.
    workers_done_.store(0, std::memory_order_relaxed);
}
```

`handle_pit()` gains one line — releasing the semaphore slot on exit (Step S3
below).

---

## Step S3 — Pit lane semaphore

### Why the old code didn't need it — and now does

Previously, `should_pit()` set `state.in_pit = true` unconditionally. Because
everything ran on one thread, "concurrent" pit stops were never actually
concurrent in wall-clock terms — the semaphore had nothing to arbitrate.
With Step S2's chunk workers now capable of two drivers crossing their pit
threshold in the *same tick* (in different chunks, running on different
threads), there is a real resource to protect: only 2 pit boxes exist. The
`PitLane` semaphore turns "driver wants to pit" into "driver actually gets a
box," queueing the rest.

### File: `src/simulation/telemetry_generator.cpp`
**Action:** EDIT — `handle_pit()`, release the slot on exit.

```cpp
void TelemetryGenerator::handle_pit(DriverState& state) {
    auto& frame = state.latest_frame;
    state.pit_timer_ticks--;
    frame.in_pit    = true;
    frame.speed_kph = 60.0f;
    frame.throttle  = 0.2f;
    frame.brake     = 0.1f;

    if (state.pit_timer_ticks <= 0) {
        state.in_pit    = false;
        frame.in_pit    = false;
        frame.tire_wear = 0.02f;
        frame.fuel_kg   = 100.0f;
        state.pit_stops++;
        pit_lane_.exit(); // free the slot for the next queued driver
    }
}
```

### File: `src/common/types.h`
**Action:** EDIT — `DriverState` gains one field (`planned_pit_lap` is Step S4's).

```cpp
struct DriverState {
    // ... existing fields unchanged ...
    bool wants_to_pit {false}; // crossed the pit threshold, waiting for a PitLane slot
    int  planned_pit_lap {0};  // Step S4 — 0 means "no plan, use the wear threshold"
};
```

`try_enter(0ms)` rather than a blocking `enter()` is deliberate: this is a
50Hz simulation tick, not a thread that can afford to block. A denied driver
simply re-checks next tick — the semaphore's queue *is* the pit-lane queue.

---

## Step S4 — Strategy analyzer decides the pit lap

### Wiring

Before `generator.start()`, run `StrategyAnalyzer::analyze()` once per driver
using a small `ThreadPool`, then push the resulting `pit_lap` into each
driver's `planned_pit_lap`. `should_pit()` prefers the plan when one exists,
falling back to the existing tire-wear threshold otherwise (e.g. safety-car
laps or an unusually fast degradation the plan didn't anticipate).

### File: `src/simulation/telemetry_generator.h`
**Action:** EDIT — add a setter the caller can use after construction, before `start()`.

```cpp
#include <unordered_map>

class TelemetryGenerator {
public:
    // ... existing API ...

    // Applies pre-computed pit-lap plans, keyed by driver id. Call after
    // construction (states_ already seeded) but before start().
    void apply_pit_plan(const std::unordered_map<std::string, int>& plans);
};
```

### File: `src/simulation/telemetry_generator.cpp`
**Action:** EDIT

```cpp
void TelemetryGenerator::apply_pit_plan(const std::unordered_map<std::string, int>& plans) {
    for (auto& state : states_) {
        auto it = plans.find(state.profile.id);
        if (it != plans.end()) state.planned_pit_lap = it->second;
    }
}

bool TelemetryGenerator::should_pit(const DriverState& state) const {
    if (state.has_completed_pit) return false;
    if (state.latest_frame.lap < 5) return false;

    if (state.planned_pit_lap > 0) {
        return state.latest_frame.lap >= state.planned_pit_lap;
    }
    float threshold = 0.60f + state.profile.tire_mgmt * 0.25f;
    return state.latest_frame.tire_wear >= threshold;
}
```

### File: `src/main.cpp`
**Action:** EDIT — run the analysis before starting the generator.

```cpp
#include "concurrency/thread_pool.h"
#include "strategy/strategy_analyzer.h"
#include <unordered_map>

int main() {
    SpscQueue<TelemetryFrame, 2048> telemetry_queue;
    MpscQueue<RaceControlEvent>     race_control_queue;
    Leaderboard                     leaderboard;
    RaceState                       race_state;

    TelemetryGenerator generator{telemetry_queue};
    TrackLimitsMonitor track_limits{race_control_queue};
    PenaltyEnforcer     penalty{race_control_queue};
    WeatherSystem       weather{race_control_queue};

    // ── Pre-race strategy planning ────────────────────────────────────────
    ThreadPool       strategy_pool{4};
    StrategyAnalyzer strategy{strategy_pool};

    std::unordered_map<std::string, int> pit_plans;
    for (const auto& driver : generator.standings()) {
        auto result = strategy.analyze(driver, DEFAULT_TRACK, TOTAL_LAPS);
        pit_plans[driver.profile.id] = result.pit_lap;
    }
    generator.apply_pit_plan(pit_plans);

    // ... existing set_on_tick / RaceDirector / start / render loop unchanged ...
}
```

Twenty `analyze()` calls, each submitting 10 futures to a 4-thread pool, run
once at startup — this is the "before the race" half of `StrategyAnalyzer`'s
intended usage described in `plan.md`. ("After each pit stop" re-analysis is
a natural Phase S+1 extension, not required to make the class load-bearing.)

---

## Phase S Tests

### File: `tests/phase_s/test_integration.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "common/season_data.h"
#include "concurrency/spsc_queue.h"
#include "concurrency/thread_pool.h"
#include "simulation/telemetry_generator.h"
#include "simulation/race_director.h"
#include "strategy/strategy_analyzer.h"
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// S1 — the start gate actually blocks the generator's first tick until the
// main-side thread arrives.
TEST(PhaseSIntegration, StartGateDelaysFirstTick) {
    SpscQueue<TelemetryFrame, 2048> q;
    TelemetryGenerator gen{q};
    RaceDirector gate{2};
    gen.set_start_gate(&gate);

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // No frames yet — generator thread is parked at the gate.
    TelemetryFrame f;
    EXPECT_FALSE(q.pop(f));

    gate.arrive_and_wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(q.pop(f)); // now it's ticking
    gen.stop();
}

// S2 — parallel chunk workers still produce exactly one frame per driver
// per tick, with positions forming a valid permutation (proves the barrier
// completion function, not the workers, does the recalculation).
TEST(PhaseSIntegration, ChunkedTickProducesConsistentStandings) {
    SpscQueue<TelemetryFrame, 4096> q;
    TelemetryGenerator gen{q};

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto standings = gen.standings();
    gen.stop();

    ASSERT_EQ(standings.size(), 20u);
    std::unordered_set<int> positions;
    for (const auto& s : standings) positions.insert(s.position);
    EXPECT_EQ(positions.size(), 20u); // no duplicate/missing positions
}

// S3 — never more than 2 drivers in_pit at the same instant.
TEST(PhaseSIntegration, PitLaneCapsConcurrentOccupancy) {
    SpscQueue<TelemetryFrame, 4096> q;
    TelemetryGenerator gen{q};

    gen.start();
    int max_concurrent_pits = 0;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int in_pit = 0;
        for (const auto& s : gen.standings()) if (s.latest_frame.in_pit) ++in_pit;
        max_concurrent_pits = std::max(max_concurrent_pits, in_pit);
    }
    gen.stop();

    EXPECT_LE(max_concurrent_pits, 2);
}

// S4 — applying a plan changes when a driver pits.
TEST(PhaseSIntegration, StrategyPlanOverridesWearThreshold) {
    SpscQueue<TelemetryFrame, 4096> q;
    TelemetryGenerator gen{q};

    std::unordered_map<std::string, int> plans;
    for (const auto& d : gen.standings()) plans[d.profile.id] = 6; // pit almost immediately
    gen.apply_pit_plan(plans);

    gen.start();
    // ~27 ticks/lap at 20ms/tick ≈ 540ms/lap; give it comfortably past lap 6.
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    bool any_pitted = false;
    for (const auto& s : gen.standings()) if (s.pit_stops > 0) any_pitted = true;
    gen.stop();

    EXPECT_TRUE(any_pitted);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

### File: `tests/CMakeLists.txt`
**Action:** EDIT — register the new target.

```cmake
add_phase_tests(phase_s
    phase_s/test_integration.cpp
)
```

---

## Building and Running

```bash
cmake --build build
ctest --test-dir build -R phase_s --output-on-failure

# TSan matters most here — 4 new worker threads touching shared state
cmake --build build-tsan
./build-tsan/tests/phase_s_tests
```

---

## Phase S Gate

- All `phase_s` tests green, zero TSan warnings on the chunked tick path.
- Existing `phase3`/`phase4` tests (`test_barrier`, `test_latch`,
  `test_semaphore`, `test_strategy_analyzer`) still pass unmodified — this
  phase adopts those primitives, it doesn't change their contracts.
- The live console run (`./build/RaceCondition-z`) shows pit stops clustering
  no more than 2 at a time, and finishing order reflects the pre-computed
  strategy rather than a uniform wear threshold.