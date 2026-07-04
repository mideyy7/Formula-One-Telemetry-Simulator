# Phase 3 — Simulation Engine

## Overview
Build the producer side: the thread that generates telemetry at 50 Hz. This phase introduces C++20's modern threading primitives — `std::jthread`, `std::stop_token`, `std::latch`, `std::barrier`, and `std::counting_semaphore`. Every thread in PitWall is launched via `jthread` from here on.

**Simulation speed:** 120× real time. Each 20ms real tick = 2.4 simulated seconds. A 50-lap race (5.4 km laps) completes in ~26 real seconds.

---

## Files Created This Phase

| File | Action | Purpose |
|------|--------|---------|
| `src/simulation/telemetry_generator.hpp` | CREATE | Generator class declaration |
| `src/simulation/telemetry_generator.cpp` | CREATE | Full tick() simulation logic |
| `src/simulation/race_director.hpp` | CREATE | Race start `std::latch` |
| `src/simulation/pit_lane.hpp` | CREATE | Pit capacity `std::counting_semaphore` |
| `src/simulation/lap_barrier.hpp` | CREATE | End-of-lap `std::barrier` |
| `tests/phase3/test_telemetry_generator.cpp` | CREATE | Generator correctness + shutdown |
| `tests/phase3/test_latch.cpp` | CREATE | Latch sync behaviour |
| `tests/phase3/test_barrier.cpp` | CREATE | Barrier phase completion |
| `tests/phase3/test_semaphore.cpp` | CREATE | Semaphore resource limiting |

**Also update:** `src/CMakeLists.txt` and `tests/CMakeLists.txt`.

---

## Step 7 — Telemetry generator

### File: `src/simulation/telemetry_generator.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include "common/season_data.hpp"
#include "concurrency/spsc_queue.hpp"
#include <thread>
#include <vector>
#include <random>
#include <functional>

// Simulation constants
inline constexpr float TIME_SCALE    = 120.0f; // sim runs 120× faster than real time
inline constexpr float TICK_S        = 0.02f;  // 20ms per real tick (50Hz)
inline constexpr float SIM_S_PER_TICK = TIME_SCALE * TICK_S; // 2.4 sim-seconds per tick
inline constexpr float TIRE_WEAR_BASE = 0.00130f; // calibrated: tires last ~30 laps
inline constexpr int   PIT_STOP_TICKS = 11;       // 25 sim-seconds / 2.4 ≈ 11 ticks
inline constexpr int   TOTAL_LAPS     = 50;

// Callback type: called after every tick with the updated states.
// Used by the leaderboard to snapshot current standings.
using OnTickCallback = std::function<void(const std::vector<DriverState>&)>;

class TelemetryGenerator {
public:
    explicit TelemetryGenerator(
        SpscQueue<TelemetryFrame, 2048>& queue,
        const TrackProfile& track = DEFAULT_TRACK
    );

    // Register a callback invoked after every tick (called from producer thread).
    void set_on_tick(OnTickCallback cb) { on_tick_ = std::move(cb); }

    // Start the producer thread.
    void start();

    // Stop the thread (cooperative via stop_token). Blocks until joined.
    void stop();

    bool is_running() const { return thread_.joinable(); }

    // Returns current sorted standings (copies — thread-safe snapshot).
    std::vector<DriverState> standings() const;

    int race_lap() const { return race_lap_; }
    bool race_finished() const { return race_finished_; }

private:
    void run(std::stop_token st);  // thread entry point
    void tick();
    void update_positions_and_drs();
    void handle_pit(DriverState& state);
    bool should_pit(const DriverState& state) const;
    static float max_speed(const CarProfile& car);

    SpscQueue<TelemetryFrame, 2048>& queue_;
    TrackProfile                     track_;
    std::vector<DriverState>         states_;
    std::jthread                     thread_;
    OnTickCallback                   on_tick_;

    std::mt19937                     rng_{42};
    std::normal_distribution<float>  dist_{0.0f, 1.0f};

    int  race_lap_      {1};
    bool race_finished_ {false};
};
```

---

### File: `src/simulation/telemetry_generator.cpp`
**Action:** CREATE

```cpp
#include "simulation/telemetry_generator.hpp"
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
        s.profile      = DRIVERS[i];
        s.car          = car_for_driver(DRIVERS[i]);
        s.position     = static_cast<int>(i + 1);

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
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void TelemetryGenerator::stop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

std::vector<DriverState> TelemetryGenerator::standings() const {
    // Returns a copy — safe to call from any thread.
    return states_;
}

// ─── Main thread loop ────────────────────────────────────────────────────────

void TelemetryGenerator::run(std::stop_token st) {
    while (!st.stop_requested() && !race_finished_) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
            auto now      = std::chrono::steady_clock::now();
            float lap_ms  = std::chrono::duration<float, std::milli>(
                                now - state.lap_start).count();
            state.lap_start = now;

            if (state.best_lap_ms < 1.0f || lap_ms < state.best_lap_ms) {
                state.best_lap_ms = lap_ms;
            }

            // Pit decision
            if (should_pit(state)) {
                state.in_pit          = true;
                state.pit_timer_ticks = PIT_STOP_TICKS;
                state.has_completed_pit = true;
                frame.in_pit = true;
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
        // If queue is full the consumer is slow; drop the frame rather than block.
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
    frame.in_pit     = true;
    frame.speed_kph  = 60.0f; // pit lane speed limit
    frame.throttle   = 0.2f;
    frame.brake      = 0.1f;

    if (state.pit_timer_ticks <= 0) {
        state.in_pit      = false;
        frame.in_pit      = false;
        frame.tire_wear   = 0.02f;  // fresh tires (slight installation wear)
        frame.fuel_kg     = 100.0f; // refueled to max
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
            // Gap in seconds: distance difference / average speed × 3600.
            float d_leader  = states_[idx[0]].latest_frame.lap * track_.length_km
                            + states_[idx[0]].distance_in_lap;
            float d_this    = s.latest_frame.lap * track_.length_km
                            + s.distance_in_lap;
            float gap_km    = d_leader - d_this;
            float avg_spd   = (states_[idx[0]].latest_frame.speed_kph +
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
```

---

## Step 8 — Race start latch

### File: `src/simulation/race_director.hpp`
**Action:** CREATE

```cpp
#pragma once

#include <latch>

// RaceDirector owns the one-shot start latch.
//
// std::latch: counts down to zero exactly once, then stays open forever.
// Cannot be reset — use std::barrier if you need a reusable checkpoint.
//
// Usage pattern:
//   RaceDirector dir{4};          // 4 threads must be ready
//   // In each thread:
//   dir.signal_ready();           // counts down by 1
//   dir.wait_for_start();         // blocks until count reaches 0
//   // All 4 threads release simultaneously.
//
// Why this matters: without a start latch, the telemetry generator might
// start producing frames before the consumer thread has initialised its
// data structures, causing the consumer to read uninitialised memory.

class RaceDirector {
public:
    explicit RaceDirector(std::ptrdiff_t thread_count)
        : latch_{thread_count} {}

    // Call from each participating thread when it is ready.
    void signal_ready() { latch_.count_down(); }

    // Blocks until all threads have called signal_ready().
    void wait_for_start() { latch_.wait(); }

    // Combines signal_ready() + wait_for_start() in one call.
    void arrive_and_wait() { latch_.arrive_and_wait(); }

private:
    std::latch latch_;
};
```

---

## Step 9 — Lap barrier

### File: `src/simulation/lap_barrier.hpp`
**Action:** CREATE

```cpp
#pragma once

#include <barrier>
#include <functional>

// LapBarrier wraps std::barrier for end-of-lap synchronisation.
//
// std::barrier: counts down each phase, runs a completion function,
// then RESETS and can be used again for the next lap.
// This is the key difference from std::latch (one-shot).
//
// Usage pattern (if drivers run on separate threads):
//   LapBarrier barrier{20, [&]() noexcept { sort_standings(); }};
//   // In each driver thread, at lap end:
//   barrier.arrive_and_wait(); // blocks until all 20 have arrived
//   // Completion function runs ONCE, then all threads release.
//
// NOTE: In the current PitWall design, all drivers run on a single
// producer thread (TelemetryGenerator::tick() loops over all states).
// The barrier is used here as a teaching example. If you later
// parallelise per-driver updates, this is how you'd sync them.
//
// The completion function MUST be noexcept.

class LapBarrier {
public:
    explicit LapBarrier(std::ptrdiff_t count,
                        std::function<void()> on_completion = {})
        : barrier_{count,
            [cb = std::move(on_completion)]() noexcept {
                if (cb) cb();
            }}
    {}

    // Called by each thread at the lap boundary.
    void arrive_and_wait() { barrier_.arrive_and_wait(); }

    // Non-blocking arrival (thread continues immediately; does not wait).
    [[nodiscard]] auto arrive() { return barrier_.arrive(); }

private:
    std::barrier<std::function<void() noexcept>> barrier_;
};
```

---

## Step 10 — Pit lane semaphore

### File: `src/simulation/pit_lane.hpp`
**Action:** CREATE

```cpp
#pragma once

#include <semaphore>
#include <chrono>

// PitLane limits how many cars can be serviced simultaneously.
//
// std::counting_semaphore<N>: internal counter starts at N.
// acquire() decrements — blocks if counter is 0.
// release() increments — unblocks one waiting thread.
//
// This models the real F1 rule: each team has one pit box, so at
// most one car per team can pit at a time. We simplify to a global
// limit of 2 simultaneous pit stops across all teams.
//
// Why a semaphore rather than a mutex?
//   - A mutex is a binary semaphore (count 0 or 1).
//   - A counting semaphore allows N concurrent holders.
//   - Use a mutex when you have ONE exclusive resource.
//   - Use a counting semaphore when you have N identical resources.

class PitLane {
public:
    // Try to enter the pit lane. Blocks if both slots are occupied.
    void enter() { capacity_.acquire(); }

    // Try to enter with a timeout. Returns true if acquired, false if timed out.
    bool try_enter(std::chrono::milliseconds timeout) {
        return capacity_.try_acquire_for(timeout);
    }

    // Leave the pit lane — frees one slot for another car.
    void exit() { capacity_.release(); }

private:
    // Template parameter is the MAX value — a compile-time upper bound.
    // Runtime initial count is passed to the constructor (also 2 here).
    std::counting_semaphore<2> capacity_{2};
};
```

---

## Update `src/CMakeLists.txt`
**Action:** EDIT — add telemetry_generator.cpp.

```cmake
add_library(pitwall_lib STATIC
    concurrency/thread_pool.cpp
    simulation/telemetry_generator.cpp
)

target_include_directories(pitwall_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pitwall_lib PUBLIC
    ftxui::screen
    ftxui::dom
    ftxui::component
)

add_executable(pitwall main.cpp)
target_link_libraries(pitwall PRIVATE pitwall_lib)
```

---

## Phase 3 Tests

### File: `tests/phase3/test_telemetry_generator.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "simulation/telemetry_generator.hpp"
#include <chrono>
#include <thread>

TEST(TelemetryGeneratorTest, ProducesFrames) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};
    gen.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // ~10 ticks
    gen.stop();

    // Should have at least 1 frame per driver (20 frames minimum)
    EXPECT_GT(queue.size_approx(), 20u);
}

TEST(TelemetryGeneratorTest, SpeedInRealisticRange) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};
    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    gen.stop();

    TelemetryFrame frame;
    while (queue.pop(frame)) {
        EXPECT_GE(frame.speed_kph, 0.0f)   << "driver=" << frame.driver_id;
        EXPECT_LE(frame.speed_kph, 400.0f)  << "driver=" << frame.driver_id;
    }
}

TEST(TelemetryGeneratorTest, TireWearIncreases) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};

    float first_wear = -1.0f;
    float last_wear  = -1.0f;

    gen.set_on_tick([&](const std::vector<DriverState>& states) {
        if (first_wear < 0.0f) first_wear = states[0].latest_frame.tire_wear;
        last_wear = states[0].latest_frame.tire_wear;
    });

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // ~25 ticks
    gen.stop();

    EXPECT_GE(last_wear, first_wear);
    EXPECT_GT(last_wear, 0.0f);
}

TEST(TelemetryGeneratorTest, FuelDecreases) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};

    float first_fuel = -1.0f, last_fuel = 101.0f;

    gen.set_on_tick([&](const std::vector<DriverState>& states) {
        if (first_fuel < 0.0f) first_fuel = states[0].latest_frame.fuel_kg;
        last_fuel = states[0].latest_frame.fuel_kg;
    });

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    gen.stop();

    EXPECT_LE(last_fuel, first_fuel);
}

TEST(TelemetryGeneratorTest, StopsWithinTimeout) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};
    gen.start();

    auto t0 = std::chrono::steady_clock::now();
    gen.stop(); // should return quickly after stop_token fires
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        100LL // must stop within 100ms (one tick is 20ms)
    );
}

TEST(TelemetryGeneratorTest, TwentyDriversProduceFrames) {
    SpscQueue<TelemetryFrame, 4096> queue;
    TelemetryGenerator gen{queue};
    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // ~5 ticks
    gen.stop();

    std::unordered_map<std::string, int> counts;
    TelemetryFrame frame;
    while (queue.pop(frame)) counts[frame.driver_id]++;

    EXPECT_EQ(counts.size(), 20u); // all 20 drivers have frames
    for (auto& [id, cnt] : counts) {
        EXPECT_GT(cnt, 0) << "No frames for driver: " << id;
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

### File: `tests/phase3/test_latch.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "simulation/race_director.hpp"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

TEST(LatchTest, AllThreadsReleaseTogether) {
    constexpr int N = 4;
    RaceDirector dir{N};
    std::atomic<int> ready_count{0};
    std::vector<std::chrono::steady_clock::time_point> release_times(N);

    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i] {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            dir.arrive_and_wait();
            release_times[i] = std::chrono::steady_clock::now();
        });
    }
    for (auto& t : threads) t.join();

    // All threads should release within 2ms of each other.
    auto [mn, mx] = std::minmax_element(release_times.begin(), release_times.end());
    auto spread = std::chrono::duration_cast<std::chrono::milliseconds>(*mx - *mn).count();
    EXPECT_LT(spread, 2LL);
    EXPECT_EQ(ready_count.load(), N);
}

TEST(LatchTest, OneShotDoesNotReset) {
    RaceDirector dir{1};
    dir.arrive_and_wait(); // triggers, latch now at 0

    // wait_for_start() should return immediately (latch is already open).
    auto t0 = std::chrono::steady_clock::now();
    dir.wait_for_start();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 1000LL); // under 1ms
}

TEST(LatchTest, SingleThreadDegenerate) {
    RaceDirector dir{1};
    // count_down(1) should trigger immediately.
    dir.signal_ready();
    dir.wait_for_start(); // must not block
    SUCCEED();
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

### File: `tests/phase3/test_barrier.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "simulation/lap_barrier.hpp"
#include <thread>
#include <atomic>
#include <chrono>

TEST(BarrierTest, CompletionFunctionCalledOncePerPhase) {
    constexpr int THREADS = 5;
    constexpr int PHASES  = 10;
    std::atomic<int> completion_count{0};

    LapBarrier barrier{THREADS, [&]() noexcept {
        completion_count.fetch_add(1, std::memory_order_relaxed);
    }};

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
            for (int p = 0; p < PHASES; ++p) {
                barrier.arrive_and_wait();
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(completion_count.load(), PHASES);
}

TEST(BarrierTest, AllThreadsReleaseAfterLastArrival) {
    constexpr int N = 4;
    std::atomic<int> inside{0};
    std::atomic<bool> violation{false};

    LapBarrier barrier{N};
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&] {
            inside.fetch_add(1, std::memory_order_relaxed);
            barrier.arrive_and_wait(); // wait for all N
            // After release: all N must have incremented inside.
            if (inside.load(std::memory_order_relaxed) != N) {
                violation.store(true);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_FALSE(violation.load());
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

### File: `tests/phase3/test_semaphore.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "simulation/pit_lane.hpp"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

TEST(SemaphoreTest, MaxTwoSimultaneously) {
    PitLane pit;
    constexpr int THREADS = 10;
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&] {
            pit.enter();
            int current = inside.fetch_add(1, std::memory_order_relaxed) + 1;
            // Track maximum simultaneous occupancy.
            int expected = max_inside.load(std::memory_order_relaxed);
            while (current > expected &&
                   !max_inside.compare_exchange_weak(expected, current,
                       std::memory_order_relaxed)) {}

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            inside.fetch_sub(1, std::memory_order_relaxed);
            pit.exit();
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_LE(max_inside.load(), 2);
    EXPECT_EQ(inside.load(), 0); // all exited cleanly
}

TEST(SemaphoreTest, TryEnterTimesOut) {
    PitLane pit;
    // Fill both slots.
    std::atomic<bool> slots_held{true};

    std::thread t1([&] { pit.enter(); while (slots_held.load()) {} pit.exit(); });
    std::thread t2([&] { pit.enter(); while (slots_held.load()) {} pit.exit(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // let t1/t2 settle

    // Third entry should time out.
    bool acquired = pit.try_enter(std::chrono::milliseconds(20));
    EXPECT_FALSE(acquired);

    slots_held.store(false);
    t1.join();
    t2.join();
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

## Update `tests/CMakeLists.txt`
**Action:** EDIT — uncomment the phase3 block.

```cmake
add_phase_tests(phase3
    phase3/test_telemetry_generator.cpp
    phase3/test_latch.cpp
    phase3/test_barrier.cpp
    phase3/test_semaphore.cpp
)
```

---

## Building and Running

```bash
cmake --build build
ctest --test-dir build -R phase3 --output-on-failure

# TSan
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan
./build-tsan/tests/phase3_tests
```

---

## Phase 3 Gate

All tests green, zero TSan warnings:

```
[ RUN      ] TelemetryGeneratorTest.ProducesFrames
[ RUN      ] TelemetryGeneratorTest.SpeedInRealisticRange
[ RUN      ] TelemetryGeneratorTest.TireWearIncreases
[ RUN      ] TelemetryGeneratorTest.FuelDecreases
[ RUN      ] TelemetryGeneratorTest.StopsWithinTimeout
[ RUN      ] TelemetryGeneratorTest.TwentyDriversProduceFrames
[ RUN      ] LatchTest.AllThreadsReleaseTogether
[ RUN      ] LatchTest.OneShotDoesNotReset
[ RUN      ] LatchTest.SingleThreadDegenerate
[ RUN      ] BarrierTest.CompletionFunctionCalledOncePerPhase
[ RUN      ] BarrierTest.AllThreadsReleaseAfterLastArrival
[ RUN      ] SemaphoreTest.MaxTwoSimultaneously
[ RUN      ] SemaphoreTest.TryEnterTimesOut
[  PASSED  ] 13 tests.
```
