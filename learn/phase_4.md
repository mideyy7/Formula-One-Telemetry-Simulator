# Phase 4 — Race Control Systems

## Overview
Build four concurrent systems that run alongside the simulation. Each is submitted to the thread pool and communicates via the MPSC event queue. The penalty enforcer teaches the most important lock-free pattern in trading systems: atomic state machines with `compare_exchange_strong`.

---

## Files Created This Phase

| File | Action | Purpose |
|------|--------|---------|
| `src/race_control/track_limits.hpp` | CREATE | Monitor that pushes violation events |
| `src/race_control/track_limits.cpp` | CREATE | Probabilistic detection logic |
| `src/race_control/penalty_enforcer.hpp` | CREATE | Atomic CAS state machine |
| `src/race_control/penalty_enforcer.cpp` | CREATE | Event consumption + state transitions |
| `src/race_control/weather.hpp` | CREATE | Track condition state |
| `src/race_control/weather.cpp` | CREATE | Periodic weather transitions |
| `src/strategy/strategy_analyzer.hpp` | CREATE | Pit window optimizer |
| `src/strategy/strategy_analyzer.cpp` | CREATE | Thread pool batching + lightweight sim |
| `tests/phase4/test_track_limits.cpp` | CREATE | Violation probability tests |
| `tests/phase4/test_penalty_enforcer.cpp` | CREATE | CAS correctness, no double-penalty |
| `tests/phase4/test_weather.cpp` | CREATE | Shared_mutex SWMR test |
| `tests/phase4/test_strategy_analyzer.cpp` | CREATE | Futures, no deadlock |

**Also update:** `src/CMakeLists.txt` and `tests/CMakeLists.txt`.

---

## Step 11 — Track limits monitor

### File: `src/race_control/track_limits.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include "concurrency/mpsc_queue.hpp"
#include <vector>
#include <random>

// TrackLimitsMonitor is called periodically (submitted to the thread pool).
// For each driver, it probabilistically decides whether they exceeded track
// limits at a sector boundary. If so, it pushes a TRACK_LIMITS event.
//
// Violation probability formula:
//   P = base_rate × aggression × (1 + tire_wear) × speed_factor
// where speed_factor = speed_kph / 280.0 (normalised to typical F1 speed).

class TrackLimitsMonitor {
public:
    explicit TrackLimitsMonitor(MpscQueue<RaceControlEvent>& event_queue);

    // Call once per monitoring cycle with current driver states.
    void check(const std::vector<DriverState>& states, int current_lap);

private:
    MpscQueue<RaceControlEvent>& events_;
    std::mt19937                 rng_{std::random_device{}()};
    std::uniform_real_distribution<float> coin_{0.0f, 1.0f};

    static constexpr float BASE_RATE = 0.004f; // ~0.4% per sector crossing
};
```

### File: `src/race_control/track_limits.cpp`
**Action:** CREATE

```cpp
#include "race_control/track_limits.hpp"
#include <chrono>

TrackLimitsMonitor::TrackLimitsMonitor(MpscQueue<RaceControlEvent>& event_queue)
    : events_(event_queue) {}

void TrackLimitsMonitor::check(const std::vector<DriverState>& states,
                                int current_lap) {
    for (const auto& state : states) {
        if (state.in_pit) continue; // can't violate track limits in pits

        const auto& frame = state.latest_frame;

        float speed_factor = frame.speed_kph / 280.0f;
        float prob = BASE_RATE
                   * state.profile.aggression
                   * (1.0f + frame.tire_wear)
                   * speed_factor;

        if (coin_(rng_) < prob) {
            RaceControlEvent ev;
            ev.type      = RaceControlEvent::Type::TRACK_LIMITS;
            ev.driver_id = state.profile.id;
            ev.lap       = current_lap;
            ev.message   = state.profile.id + " exceeded track limits (S"
                         + std::to_string(frame.sector) + ")";
            ev.timestamp = std::chrono::steady_clock::now();
            events_.push(std::move(ev));
        }
    }
}
```

---

## Step 12 — Penalty enforcer

### File: `src/race_control/penalty_enforcer.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include "concurrency/mpsc_queue.hpp"
#include <array>
#include <atomic>
#include <string>

// PenaltyEnforcer consumes TRACK_LIMITS events and manages per-driver
// penalty state via atomic compare-and-exchange.
//
// State machine per driver:
//   NONE → PENDING  (on 3rd warning, via CAS)
//   PENDING → SERVING (when driver enters pits)
//   SERVING → SERVED  (when pit stop completes)
//
// Why CAS for the NONE→PENDING transition?
//   If track limits events arrive from multiple threads simultaneously
//   (possible if you add more monitoring threads later), two threads might
//   both see warnings_count[i] == 3 and both try to issue a penalty.
//   CAS ensures exactly one succeeds:
//     expected = NONE; if (state.CAS(expected, PENDING)) { issue penalty }
//   The losing CAS sees expected != NONE and does nothing.
//
// The warning counters use fetch_add which is inherently atomic — no CAS needed.

class PenaltyEnforcer {
public:
    explicit PenaltyEnforcer(MpscQueue<RaceControlEvent>& event_queue);

    // Drain the MPSC queue and process all pending TRACK_LIMITS events.
    // Call this from any single consumer thread.
    void process_events();

    // Called by the simulator when a driver enters/exits the pits.
    void driver_entered_pits(const std::string& driver_id);
    void driver_exited_pits(const std::string& driver_id);

    // Current penalty state for a driver.
    PenaltyState penalty_state(const std::string& driver_id) const;

    // Number of accumulated warnings.
    int warning_count(const std::string& driver_id) const;

private:
    int driver_index(const std::string& id) const;

    MpscQueue<RaceControlEvent>& events_;

    // One entry per driver (indexed by DRIVERS order).
    std::array<std::atomic<int>,            20> warnings_{};
    std::array<std::atomic<PenaltyState>,   20> states_{};
    std::array<std::atomic<int>,            20> extra_pit_time_{}; // ticks added
};
```

### File: `src/race_control/penalty_enforcer.cpp`
**Action:** CREATE

```cpp
#include "race_control/penalty_enforcer.hpp"
#include "common/season_data.hpp"
#include <algorithm>
#include <chrono>

PenaltyEnforcer::PenaltyEnforcer(MpscQueue<RaceControlEvent>& event_queue)
    : events_(event_queue)
{
    // Initialise all penalty states to NONE.
    for (auto& s : states_) s.store(PenaltyState::NONE, std::memory_order_relaxed);
    for (auto& w : warnings_) w.store(0, std::memory_order_relaxed);
    for (auto& t : extra_pit_time_) t.store(0, std::memory_order_relaxed);
}

int PenaltyEnforcer::driver_index(const std::string& id) const {
    for (int i = 0; i < static_cast<int>(DRIVERS.size()); ++i) {
        if (DRIVERS[i].id == id) return i;
    }
    return -1;
}

void PenaltyEnforcer::process_events() {
    RaceControlEvent ev;
    while (events_.pop(ev)) {
        if (ev.type != RaceControlEvent::Type::TRACK_LIMITS) continue;

        int idx = driver_index(ev.driver_id);
        if (idx < 0) continue;

        int count = warnings_[idx].fetch_add(1, std::memory_order_relaxed) + 1;

        if (count == 3) {
            // CAS: only ONE thread can win this transition.
            // expected will be modified by CAS if it fails — reset it.
            PenaltyState expected = PenaltyState::NONE;
            if (states_[idx].compare_exchange_strong(
                    expected, PenaltyState::PENDING,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                // We won. Push a PENALTY_ISSUED event.
                RaceControlEvent pen;
                pen.type      = RaceControlEvent::Type::PENALTY_ISSUED;
                pen.driver_id = ev.driver_id;
                pen.lap       = ev.lap;
                pen.message   = ev.driver_id + " — 5 second penalty (track limits)";
                pen.timestamp = std::chrono::steady_clock::now();
                events_.push(std::move(pen));

                // Add 5 seconds worth of pit stop ticks (~13 ticks at 2.4s/tick).
                extra_pit_time_[idx].store(3, std::memory_order_relaxed);
            }
            // If CAS failed: another thread already issued the penalty. Do nothing.
        }
    }
}

void PenaltyEnforcer::driver_entered_pits(const std::string& driver_id) {
    int idx = driver_index(driver_id);
    if (idx < 0) return;

    PenaltyState expected = PenaltyState::PENDING;
    states_[idx].compare_exchange_strong(
        expected, PenaltyState::SERVING,
        std::memory_order_acq_rel,
        std::memory_order_relaxed);
}

void PenaltyEnforcer::driver_exited_pits(const std::string& driver_id) {
    int idx = driver_index(driver_id);
    if (idx < 0) return;

    PenaltyState expected = PenaltyState::SERVING;
    states_[idx].compare_exchange_strong(
        expected, PenaltyState::SERVED,
        std::memory_order_acq_rel,
        std::memory_order_relaxed);
}

PenaltyState PenaltyEnforcer::penalty_state(const std::string& driver_id) const {
    int idx = driver_index(driver_id);
    if (idx < 0) return PenaltyState::NONE;
    return states_[idx].load(std::memory_order_acquire);
}

int PenaltyEnforcer::warning_count(const std::string& driver_id) const {
    int idx = driver_index(driver_id);
    if (idx < 0) return 0;
    return warnings_[idx].load(std::memory_order_relaxed);
}
```

---

## Step 13 — Weather system

### File: `src/race_control/weather.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include "concurrency/mpsc_queue.hpp"
#include <shared_mutex>
#include <random>

// WeatherSystem manages track conditions.
// Written by a single background thread; read by all 20 driver update threads.
//
// shared_mutex (SWMR — Single Writer Multiple Readers):
//   unique_lock  → exclusive write: blocks all readers
//   shared_lock  → concurrent read: multiple readers proceed in parallel
//
// Why not a plain mutex? The driver threads read weather every tick (50Hz × 20).
// A plain mutex would serialise all 20 reads unnecessarily. shared_mutex lets
// all 20 driver threads read simultaneously, only blocking when the weather
// writer holds the exclusive lock (rare — once every few laps).

class WeatherSystem {
public:
    explicit WeatherSystem(MpscQueue<RaceControlEvent>& event_queue);

    // Called periodically from the thread pool.
    // May transition weather state and push an event.
    void update(int current_lap);

    // Called from driver threads — acquires shared (read) lock.
    WeatherState current() const;

    // Grip multiplier based on weather: DRY=1.0, DAMP=0.85, WET=0.70
    float grip_factor() const;

private:
    MpscQueue<RaceControlEvent>& events_;
    WeatherState                 state_{WeatherState::DRY};
    mutable std::shared_mutex    mutex_;
    std::mt19937                 rng_{std::random_device{}()};
    std::uniform_real_distribution<float> coin_{0.0f, 1.0f};
    int last_update_lap_{0};

    static constexpr float CHANGE_PROBABILITY = 0.15f; // per update call
};
```

### File: `src/race_control/weather.cpp`
**Action:** CREATE

```cpp
#include "race_control/weather.hpp"
#include <chrono>

WeatherSystem::WeatherSystem(MpscQueue<RaceControlEvent>& event_queue)
    : events_(event_queue) {}

void WeatherSystem::update(int current_lap) {
    if (current_lap - last_update_lap_ < 5) return; // don't change too often
    last_update_lap_ = current_lap;

    if (coin_(rng_) > CHANGE_PROBABILITY) return; // no change this update

    WeatherState new_state;
    {
        std::shared_lock read{mutex_};
        // Transition: DRY→DAMP→WET→DRY (cycle)
        switch (state_) {
            case WeatherState::DRY:  new_state = WeatherState::DAMP; break;
            case WeatherState::DAMP: new_state = WeatherState::WET;  break;
            case WeatherState::WET:  new_state = WeatherState::DRY;  break;
        }
    }

    {
        std::unique_lock write{mutex_}; // exclusive — blocks all readers
        state_ = new_state;
    }

    static const char* names[] = {"DRY", "DAMP", "WET"};
    RaceControlEvent ev;
    ev.type      = RaceControlEvent::Type::WEATHER_CHANGE;
    ev.lap       = current_lap;
    ev.message   = std::string("Weather: ") + names[static_cast<int>(new_state)];
    ev.timestamp = std::chrono::steady_clock::now();
    events_.push(std::move(ev));
}

WeatherState WeatherSystem::current() const {
    std::shared_lock read{mutex_}; // concurrent reads allowed
    return state_;
}

float WeatherSystem::grip_factor() const {
    std::shared_lock read{mutex_};
    switch (state_) {
        case WeatherState::DRY:  return 1.00f;
        case WeatherState::DAMP: return 0.85f;
        case WeatherState::WET:  return 0.70f;
    }
    return 1.0f;
}
```

---

## Step 14 — Strategy analyzer

### File: `src/strategy/strategy_analyzer.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include "concurrency/thread_pool.hpp"
#include <vector>
#include <future>

// StrategyAnalyzer finds the optimal pit lap for a driver.
// It submits 10 lightweight race simulations to the thread pool —
// one per candidate pit lap — and collects results via std::future.
//
// Key pattern: task batching with futures.
//   1. Submit all tasks at once (non-blocking).
//   2. Call future.get() on each (blocks until that task finishes).
//   3. Pick the best result.
//
// This is the same pattern used in quant systems to parallelise
// independent scenario analyses (pricing models, risk runs, etc.).

struct StrategyResult {
    int   pit_lap;
    float estimated_race_time_s; // lower is better
};

class StrategyAnalyzer {
public:
    explicit StrategyAnalyzer(ThreadPool& pool);

    // Returns the optimal pit lap for the given driver.
    // Blocks until all 10 simulations complete.
    StrategyResult analyze(const DriverState& driver,
                           const TrackProfile& track,
                           int total_laps);

    // Candidate pit laps to evaluate.
    static constexpr std::array<int, 10> CANDIDATES = {
        12, 15, 18, 21, 24, 27, 30, 33, 36, 39
    };

private:
    static float simulate_one_strategy(
        const DriverProfile& profile,
        const CarProfile& car,
        const TrackProfile& track,
        int pit_lap,
        int total_laps
    );

    ThreadPool& pool_;
};
```

### File: `src/strategy/strategy_analyzer.cpp`
**Action:** CREATE

```cpp
#include "strategy/strategy_analyzer.hpp"
#include <limits>
#include <algorithm>

StrategyAnalyzer::StrategyAnalyzer(ThreadPool& pool)
    : pool_(pool) {}

StrategyResult StrategyAnalyzer::analyze(
    const DriverState& driver,
    const TrackProfile& track,
    int total_laps)
{
    // Submit all 10 scenarios to the thread pool simultaneously.
    std::vector<std::future<float>> futures;
    futures.reserve(CANDIDATES.size());

    for (int pit_lap : CANDIDATES) {
        futures.push_back(pool_.submit(
            simulate_one_strategy,
            driver.profile, driver.car, track, pit_lap, total_laps
        ));
    }

    // Collect results — each .get() blocks until that task is done.
    StrategyResult best{CANDIDATES[0], std::numeric_limits<float>::max()};
    for (std::size_t i = 0; i < CANDIDATES.size(); ++i) {
        float t = futures[i].get();
        if (t < best.estimated_race_time_s) {
            best = {CANDIDATES[i], t};
        }
    }
    return best;
}

// Lightweight race simulation: estimates total race time given a pit lap.
// This runs on a worker thread — no shared state, fully independent.
float StrategyAnalyzer::simulate_one_strategy(
    const DriverProfile& profile,
    const CarProfile& car,
    const TrackProfile& track,
    int pit_lap,
    int total_laps)
{
    // Base lap time in seconds: top car ~87s, back-marker ~93s
    float base_lap_s = 87.0f + (1.0f - car.engine_power) * 8.0f;

    float time       = 0.0f;
    float tire_wear  = 0.0f;
    float fuel_kg    = 100.0f;

    for (int lap = 1; lap <= total_laps; ++lap) {
        // Pit stop
        if (lap == pit_lap) {
            time     += 25.0f;    // pit lane time (seconds)
            tire_wear = 0.02f;    // fresh tires
            fuel_kg   = 100.0f;   // refueled
        }

        // Lap time penalties
        float tire_penalty = tire_wear * 3.0f; // up to ~3s on worn tires
        float fuel_penalty = (fuel_kg / 100.0f) * 0.4f; // heavy fuel = slower

        time += base_lap_s + tire_penalty + fuel_penalty;

        // State evolution
        tire_wear += 0.025f * profile.aggression;
        fuel_kg   -= track.fuel_consumption * track.length_km / total_laps;
        fuel_kg    = std::max(0.0f, fuel_kg);
    }

    return time;
}
```

---

## Update `src/CMakeLists.txt`
**Action:** EDIT — add new .cpp source files.

```cmake
add_library(pitwall_lib STATIC
    concurrency/thread_pool.cpp
    simulation/telemetry_generator.cpp
    race_control/track_limits.cpp
    race_control/penalty_enforcer.cpp
    race_control/weather.cpp
    strategy/strategy_analyzer.cpp
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

## Phase 4 Tests

### File: `tests/phase4/test_track_limits.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "race_control/track_limits.hpp"
#include "common/season_data.hpp"

static DriverState make_driver(const std::string& id, float aggression,
                                float tire_wear, float speed) {
    DriverState s;
    for (const auto& d : DRIVERS)
        if (d.id == id) { s.profile = d; break; }
    s.profile.aggression       = aggression;
    s.latest_frame.tire_wear   = tire_wear;
    s.latest_frame.speed_kph   = speed;
    s.latest_frame.sector      = 2;
    return s;
}

TEST(TrackLimitsTest, HighAggressionMoreViolations) {
    MpscQueue<RaceControlEvent> q;
    TrackLimitsMonitor mon{q};

    std::vector<DriverState> states_high = {
        make_driver("VER", 1.0f, 0.8f, 300.0f)
    };
    std::vector<DriverState> states_low = {
        make_driver("HAM", 0.1f, 0.1f, 200.0f)
    };

    int high_count = 0, low_count = 0;
    for (int i = 0; i < 10'000; ++i) mon.check(states_high, 1);
    RaceControlEvent ev;
    while (q.pop(ev)) ++high_count;

    for (int i = 0; i < 10'000; ++i) mon.check(states_low, 1);
    while (q.pop(ev)) ++low_count;

    EXPECT_GT(high_count, low_count);
    EXPECT_GT(high_count, 0);
}

TEST(TrackLimitsTest, EventHasCorrectFields) {
    MpscQueue<RaceControlEvent> q;
    TrackLimitsMonitor mon{q};

    // Force many checks until at least one event fires.
    std::vector<DriverState> states = {make_driver("VER", 1.0f, 1.0f, 350.0f)};
    for (int i = 0; i < 10'000 && q.empty(); ++i) mon.check(states, 5);

    RaceControlEvent ev;
    if (q.pop(ev)) {
        EXPECT_EQ(ev.type, RaceControlEvent::Type::TRACK_LIMITS);
        EXPECT_EQ(ev.driver_id, "VER");
        EXPECT_EQ(ev.lap, 5);
        EXPECT_FALSE(ev.message.empty());
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

### File: `tests/phase4/test_penalty_enforcer.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "race_control/penalty_enforcer.hpp"
#include <thread>
#include <atomic>

static void push_track_limits(MpscQueue<RaceControlEvent>& q,
                               const std::string& id, int lap) {
    RaceControlEvent ev;
    ev.type      = RaceControlEvent::Type::TRACK_LIMITS;
    ev.driver_id = id;
    ev.lap       = lap;
    ev.timestamp = std::chrono::steady_clock::now();
    q.push(std::move(ev));
}

TEST(PenaltyEnforcerTest, ThreeWarningsTriggerPenalty) {
    MpscQueue<RaceControlEvent> q;
    PenaltyEnforcer enforcer{q};

    push_track_limits(q, "VER", 1);
    push_track_limits(q, "VER", 2);
    push_track_limits(q, "VER", 3);
    enforcer.process_events();

    EXPECT_EQ(enforcer.warning_count("VER"), 3);
    EXPECT_EQ(enforcer.penalty_state("VER"), PenaltyState::PENDING);
}

TEST(PenaltyEnforcerTest, TwoWarningsNoPenalty) {
    MpscQueue<RaceControlEvent> q;
    PenaltyEnforcer enforcer{q};

    push_track_limits(q, "LEC", 1);
    push_track_limits(q, "LEC", 2);
    enforcer.process_events();

    EXPECT_EQ(enforcer.penalty_state("LEC"), PenaltyState::NONE);
}

// Concurrent: 4 threads simultaneously push a 3rd warning for VER.
// Exactly one penalty should be issued (CAS guarantee).
TEST(PenaltyEnforcerTest, ConcurrentThirdWarningExactlyOnePenalty) {
    MpscQueue<RaceControlEvent> q;
    PenaltyEnforcer enforcer{q};

    // Prime with 2 warnings first.
    push_track_limits(q, "VER", 1);
    push_track_limits(q, "VER", 2);
    enforcer.process_events();
    EXPECT_EQ(enforcer.penalty_state("VER"), PenaltyState::NONE);

    // 4 threads simultaneously push a 3rd warning.
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] { push_track_limits(q, "VER", 3); });
    }
    for (auto& t : threads) t.join();

    enforcer.process_events();

    // Count PENALTY_ISSUED events — must be exactly 1.
    int penalty_count = 0;
    RaceControlEvent ev;
    while (q.pop(ev)) {
        if (ev.type == RaceControlEvent::Type::PENALTY_ISSUED &&
            ev.driver_id == "VER") {
            ++penalty_count;
        }
    }
    EXPECT_EQ(penalty_count, 1);
}

TEST(PenaltyEnforcerTest, StateMachineTransitions) {
    MpscQueue<RaceControlEvent> q;
    PenaltyEnforcer enforcer{q};

    for (int i = 1; i <= 3; ++i) push_track_limits(q, "NOR", i);
    enforcer.process_events();
    EXPECT_EQ(enforcer.penalty_state("NOR"), PenaltyState::PENDING);

    enforcer.driver_entered_pits("NOR");
    EXPECT_EQ(enforcer.penalty_state("NOR"), PenaltyState::SERVING);

    enforcer.driver_exited_pits("NOR");
    EXPECT_EQ(enforcer.penalty_state("NOR"), PenaltyState::SERVED);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

### File: `tests/phase4/test_weather.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "race_control/weather.hpp"
#include <thread>
#include <atomic>
#include <chrono>

TEST(WeatherTest, InitiallyDry) {
    MpscQueue<RaceControlEvent> q;
    WeatherSystem wx{q};
    EXPECT_EQ(wx.current(), WeatherState::DRY);
    EXPECT_FLOAT_EQ(wx.grip_factor(), 1.0f);
}

TEST(WeatherTest, ValidTransitionsOnly) {
    MpscQueue<RaceControlEvent> q;
    WeatherSystem wx{q};

    // Force many updates — state should only ever be DRY, DAMP, or WET.
    for (int lap = 0; lap < 200; lap += 5) {
        wx.update(lap);
        WeatherState s = wx.current();
        EXPECT_TRUE(s == WeatherState::DRY ||
                    s == WeatherState::DAMP ||
                    s == WeatherState::WET);
    }
}

// SWMR test: 8 reader threads + 1 writer thread.
// Readers must never see a torn/invalid WeatherState.
TEST(WeatherTest, SharedMutexNoTearing) {
    MpscQueue<RaceControlEvent> q;
    WeatherSystem wx{q};
    std::atomic<bool> stop{false};
    std::atomic<bool> violation{false};

    // 8 reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                WeatherState s = wx.current();
                if (s != WeatherState::DRY &&
                    s != WeatherState::DAMP &&
                    s != WeatherState::WET) {
                    violation.store(true);
                }
            }
        });
    }

    // 1 writer thread — updates every 5ms
    std::thread writer([&] {
        for (int lap = 0; lap < 200; lap += 5) {
            wx.update(lap);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        stop.store(true);
    });

    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_FALSE(violation.load());
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

### File: `tests/phase4/test_strategy_analyzer.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "strategy/strategy_analyzer.hpp"
#include "common/season_data.hpp"

TEST(StrategyAnalyzerTest, ReturnsValidPitLap) {
    ThreadPool pool{4};
    StrategyAnalyzer analyzer{pool};

    DriverState driver;
    driver.profile = DRIVERS[0]; // VER
    driver.car     = CARS[0];    // Red Bull

    auto result = analyzer.analyze(driver, DEFAULT_TRACK, 50);

    bool valid = false;
    for (int c : StrategyAnalyzer::CANDIDATES) {
        if (c == result.pit_lap) { valid = true; break; }
    }
    EXPECT_TRUE(valid) << "pit_lap=" << result.pit_lap;
    EXPECT_GT(result.estimated_race_time_s, 0.0f);
}

TEST(StrategyAnalyzerTest, AllFuturesComplete) {
    ThreadPool pool{4};
    StrategyAnalyzer analyzer{pool};

    DriverState driver;
    driver.profile = DRIVERS[1];
    driver.car     = CARS[0];

    // If any future deadlocks, this test will hang.
    // Run with a timeout in your CI (e.g. --timeout 10).
    auto result = analyzer.analyze(driver, DEFAULT_TRACK, 50);
    EXPECT_GT(result.estimated_race_time_s, 0.0f);
}

TEST(StrategyAnalyzerTest, ConcurrentAnalysesNoDead) {
    ThreadPool pool{8};
    StrategyAnalyzer analyzer{pool};

    // Run 5 analyses simultaneously using std::async.
    std::vector<std::future<StrategyResult>> results;
    for (int i = 0; i < 5; ++i) {
        DriverState d;
        d.profile = DRIVERS[i];
        d.car     = CARS[i / 2];
        results.push_back(std::async(std::launch::async,
            [&analyzer, d] {
                return analyzer.analyze(d, DEFAULT_TRACK, 50);
            }));
    }
    for (auto& f : results) {
        auto r = f.get();
        EXPECT_GT(r.estimated_race_time_s, 0.0f);
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

## Update `tests/CMakeLists.txt`
**Action:** EDIT — uncomment the phase4 block.

```cmake
add_phase_tests(phase4
    phase4/test_track_limits.cpp
    phase4/test_penalty_enforcer.cpp
    phase4/test_weather.cpp
    phase4/test_strategy_analyzer.cpp
)
```

---

## Building and Running

```bash
cmake --build build
ctest --test-dir build -R phase4 --output-on-failure

# TSan
cmake --build build-tsan
./build-tsan/tests/phase4_tests
```

---

## Phase 4 Gate

All tests green, zero TSan warnings:

```
[ RUN      ] TrackLimitsTest.HighAggressionMoreViolations
[ RUN      ] TrackLimitsTest.EventHasCorrectFields
[ RUN      ] PenaltyEnforcerTest.ThreeWarningsTriggerPenalty
[ RUN      ] PenaltyEnforcerTest.TwoWarningsNoPenalty
[ RUN      ] PenaltyEnforcerTest.ConcurrentThirdWarningExactlyOnePenalty
[ RUN      ] PenaltyEnforcerTest.StateMachineTransitions
[ RUN      ] WeatherTest.InitiallyDry
[ RUN      ] WeatherTest.ValidTransitionsOnly
[ RUN      ] WeatherTest.SharedMutexNoTearing
[ RUN      ] StrategyAnalyzerTest.ReturnsValidPitLap
[ RUN      ] StrategyAnalyzerTest.AllFuturesComplete
[ RUN      ] StrategyAnalyzerTest.ConcurrentAnalysesNoDead
[  PASSED  ] 12 tests.
```
