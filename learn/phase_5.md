# Phase 5 — Shared State Layer

## Overview
Define the two shared data structures that bridge the simulation (writer) and the UI (readers). This phase drills into `std::shared_mutex` for the leaderboard and explicit `std::atomic` memory ordering for race flags. These are the hardest concepts to get right and the most likely to come up in quant firm interviews.

---

## Files Created This Phase

| File | Action | Purpose |
|------|--------|---------|
| `src/common/leaderboard.hpp` | CREATE | SWMR-protected sorted driver standings |
| `src/common/race_state.hpp` | CREATE | Atomic race flags with documented memory orders |
| `tests/phase5/test_leaderboard.cpp` | CREATE | SWMR stress test, no torn reads |
| `tests/phase5/test_race_state.cpp` | CREATE | Atomic visibility, CAS correctness |

**Also update:** `tests/CMakeLists.txt`.

---

## Step 15 — Leaderboard

### File: `src/common/leaderboard.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include <shared_mutex>
#include <vector>

// Leaderboard holds the sorted race standings.
//
// Access pattern:
//   Writer: TelemetryGenerator (1 thread) — updates once per lap
//   Readers: FTXUI render loop (1 thread) + any status queries
//
// std::shared_mutex (SWMR — Single Writer Multiple Readers):
//   unique_lock  → exclusive: blocks ALL other readers and writers
//   shared_lock  → shared:    multiple readers proceed concurrently
//
// Why not std::mutex? A plain mutex would serialise every snapshot()
// call even when two readers hit it simultaneously. shared_mutex lets
// all readers proceed in parallel — only the writer blocks everyone.
//
// Performance note: shared_mutex has higher overhead than plain mutex
// when there is actually contention. In our case: reads are frequent
// (~10Hz UI) but short (copy 20 structs), and writes are rare (~1/lap).
// shared_mutex pays off here.
//
// The `mutable` keyword: allows snapshot() to be `const` (logically
// read-only) while still locking the mutex (which modifies internal
// lock state). Without mutable, a const method cannot lock a mutex.

class Leaderboard {
public:
    // Called by the simulation thread — takes exclusive lock.
    void update(std::vector<DriverState> sorted_standings) {
        std::unique_lock lock{mutex_};
        standings_ = std::move(sorted_standings);
    }

    // Called by any reader — takes shared lock.
    // Returns a full copy so the caller doesn't need to hold the lock.
    std::vector<DriverState> snapshot() const {
        std::shared_lock lock{mutex_};
        return standings_;
    }

    // Returns the number of drivers currently tracked.
    std::size_t size() const {
        std::shared_lock lock{mutex_};
        return standings_.size();
    }

    // Returns the driver at a given position (1-indexed).
    // Returns an empty DriverState if position is out of range.
    DriverState at_position(int pos) const {
        std::shared_lock lock{mutex_};
        for (const auto& s : standings_) {
            if (s.position == pos) return s;
        }
        return {};
    }

    // Returns the best lap holder (position 1 driver's data).
    std::string fastest_lap_holder() const {
        std::shared_lock lock{mutex_};
        float best = std::numeric_limits<float>::max();
        std::string holder;
        for (const auto& s : standings_) {
            if (s.best_lap_ms > 0.0f && s.best_lap_ms < best) {
                best   = s.best_lap_ms;
                holder = s.profile.id;
            }
        }
        return holder;
    }

private:
    std::vector<DriverState> standings_;
    mutable std::shared_mutex mutex_;
};
```

---

## Step 16 — Race state atomics

### File: `src/common/race_state.hpp`
**Action:** CREATE

```cpp
#pragma once

#include <atomic>
#include <string>
#include <limits>

// RaceState holds global flags that multiple threads read and write.
//
// Every load/store has an explicit memory_order annotation.
// The comments explain WHY that order is sufficient.
//
// ─── Memory order quick reference ────────────────────────────────────────────
//
// memory_order_relaxed  — no synchronisation; just atomicity. Safe only when
//                         the value itself is the only thing that matters
//                         (e.g. a monotonically increasing counter where you
//                         don't care about the order of other writes).
//
// memory_order_release  — on a STORE: all writes before this store are
//                         visible to any thread that does an acquire-load
//                         of the SAME atomic. Think: "I'm done writing;
//                         publish everything I've done."
//
// memory_order_acquire  — on a LOAD: if I see the value written by a
//                         release-store, I also see all writes that
//                         happened before that store. Think: "I'm reading;
//                         show me everything the writer wrote before this."
//
// memory_order_acq_rel  — on a READ-MODIFY-WRITE (e.g. exchange, CAS):
//                         acts as both acquire and release simultaneously.
//
// memory_order_seq_cst  — total global order: the most expensive, rarely
//                         needed. Only use when you have multiple atomics
//                         and threads must agree on the ORDER of all stores.
//
// ─────────────────────────────────────────────────────────────────────────────

struct RaceState {
    // ── race_active ───────────────────────────────────────────────────────────
    // Written ONCE at start (true) and ONCE at end (false).
    // All threads read it every tick to decide whether to exit.
    //
    // Store: release — ensures all simulation setup writes are visible to
    //   threads that subsequently load with acquire.
    // Load:  acquire — threads see the store and all writes before it.
    std::atomic<bool> race_active{false};

    void start_race() { race_active.store(true,  std::memory_order_release); }
    void end_race()   { race_active.store(false, std::memory_order_release); }
    bool is_active()  { return race_active.load(std::memory_order_acquire);  }

    // ── current_lap ──────────────────────────────────────────────────────────
    // Written by TelemetryGenerator. Read by UI for the lap counter display.
    //
    // relaxed load/store is fine here: the lap number is informational.
    // If the UI displays lap N-1 for one frame instead of N, no harm done.
    // We do NOT need the lap update to be ordered relative to other writes.
    std::atomic<int> current_lap{1};

    void set_lap(int lap)  { current_lap.store(lap, std::memory_order_relaxed); }
    int  get_lap()   const { return current_lap.load(std::memory_order_relaxed); }

    // ── safety_car ───────────────────────────────────────────────────────────
    // Written by RaceDirector. Read by all 20 driver threads each tick.
    //
    // release store: ensures the UI also sees the safety_car deployment
    //   alongside any other state the director updated before setting it.
    // acquire load: driver threads see the flag AND all preceding writes.
    //
    // This is an "atomic broadcast" — one writer, many readers, no locks.
    std::atomic<bool> safety_car{false};

    void deploy_safety_car()  { safety_car.store(true,  std::memory_order_release); }
    void retract_safety_car() { safety_car.store(false, std::memory_order_release); }
    bool is_safety_car() const { return safety_car.load(std::memory_order_acquire); }

    // ── fastest_lap_holder ───────────────────────────────────────────────────
    // Updated whenever a driver sets a new fastest lap.
    // Multiple driver threads can simultaneously try to claim fastest lap —
    // CAS ensures exactly one wins.
    //
    // We store an index into DRIVERS (0-19) rather than a string to keep
    // the atomic simple. -1 means "no fastest lap set yet".
    //
    // acq_rel on success: we've written the holder AND all our prior writes
    //   are visible. acquire on failure: we see the current winner.
    std::atomic<int> fastest_lap_holder{-1};
    std::atomic<float> fastest_lap_ms{std::numeric_limits<float>::max()};

    // Returns true if this driver set a new fastest lap.
    bool try_claim_fastest_lap(int driver_index, float lap_ms) {
        float current_best = fastest_lap_ms.load(std::memory_order_acquire);
        while (lap_ms < current_best) {
            if (fastest_lap_ms.compare_exchange_weak(
                    current_best, lap_ms,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                // We updated the time; now update the holder.
                fastest_lap_holder.store(driver_index, std::memory_order_release);
                return true;
            }
            // current_best was updated by CAS to the actual current value.
            // Loop and check again.
        }
        return false;
    }

    int  get_fastest_lap_holder() const {
        return fastest_lap_holder.load(std::memory_order_acquire);
    }
    float get_fastest_lap_ms() const {
        return fastest_lap_ms.load(std::memory_order_acquire);
    }
};

// Global singleton — declared here, defined in main.cpp.
// Use extern to avoid multiple-definition errors across translation units.
extern RaceState g_race_state;
```

> **Interview trap:** Why not `memory_order_seq_cst` everywhere?
> `seq_cst` is the default and the safest, but it's also the most expensive
> because it inserts full memory fences on x86 (MFENCE instruction). On ARM
> it's even more expensive. In a 50Hz simulation loop with 20 drivers each
> doing atomic reads, the overhead adds up. Use the weakest order that is
> still correct — that's what production HFT systems do.

---

## Phase 5 Tests

### File: `tests/phase5/test_leaderboard.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "common/leaderboard.hpp"
#include "common/season_data.hpp"
#include <thread>
#include <atomic>
#include <chrono>

static std::vector<DriverState> make_standings(int n = 20) {
    std::vector<DriverState> v;
    for (int i = 0; i < n; ++i) {
        DriverState s;
        s.profile  = DRIVERS[i];
        s.car      = CARS[i / 2];
        s.position = i + 1;
        s.latest_frame.driver_id = DRIVERS[i].id;
        v.push_back(s);
    }
    return v;
}

TEST(LeaderboardTest, UpdateAndSnapshot) {
    Leaderboard board;
    auto standings = make_standings();
    board.update(standings);

    auto snap = board.snapshot();
    ASSERT_EQ(snap.size(), 20u);
    EXPECT_EQ(snap[0].profile.id, "VER");
}

TEST(LeaderboardTest, SnapshotIsFullCopy) {
    Leaderboard board;
    board.update(make_standings());

    auto snap = board.snapshot();
    EXPECT_EQ(snap.size(), 20u);
    // Modifying the snapshot should not affect the leaderboard.
    snap[0].position = 99;
    EXPECT_EQ(board.at_position(1).profile.id, "VER");
}

// SWMR stress: 1 writer updates every 10ms; 8 readers snapshot as fast as
// possible for 2 seconds. Readers must always see all 20 drivers.
TEST(LeaderboardTest, SwmrNoTearing) {
    Leaderboard board;
    board.update(make_standings());

    std::atomic<bool> stop{false};
    std::atomic<bool> violation{false};

    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = board.snapshot();
                if (snap.size() != 20u) violation.store(true);
            }
        });
    }

    std::thread writer([&] {
        for (int i = 0; i < 200; ++i) {
            board.update(make_standings());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop.store(true);
    });

    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_FALSE(violation.load()) << "Saw partial standings under concurrent read";
}

TEST(LeaderboardTest, AtPosition) {
    Leaderboard board;
    board.update(make_standings());

    auto p1 = board.at_position(1);
    EXPECT_EQ(p1.position, 1);
    EXPECT_EQ(p1.profile.id, "VER");

    auto invalid = board.at_position(99);
    EXPECT_EQ(invalid.position, 0); // default DriverState
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

### File: `tests/phase5/test_race_state.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "common/race_state.hpp"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

// Define the global (normally done in main.cpp — do it here for tests).
RaceState g_race_state;

TEST(RaceStateTest, StartStopRace) {
    RaceState rs;
    EXPECT_FALSE(rs.is_active());
    rs.start_race();
    EXPECT_TRUE(rs.is_active());
    rs.end_race();
    EXPECT_FALSE(rs.is_active());
}

// Safety car broadcast: producer sets flag; 10 consumer threads read it.
// All must see true within 50ms.
TEST(RaceStateTest, SafetyCarBroadcast) {
    RaceState rs;
    std::atomic<int> seen_count{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> readers;
    for (int i = 0; i < 10; ++i) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {} // spin-wait for start
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(50);
            while (std::chrono::steady_clock::now() < deadline) {
                if (rs.is_safety_car()) {
                    seen_count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    rs.deploy_safety_car();

    for (auto& t : readers) t.join();
    EXPECT_EQ(seen_count.load(), 10);
}

// Lap counter: 1 writer increments 1000 times; final value must be 1000.
TEST(RaceStateTest, LapCounterNoLostUpdates) {
    RaceState rs;
    for (int i = 1; i <= 1000; ++i) rs.set_lap(i);
    EXPECT_EQ(rs.get_lap(), 1000);
}

// Fastest lap CAS: 5 threads simultaneously try to claim fastest lap.
// Exactly the one with the lowest time should win.
TEST(RaceStateTest, FastestLapCasExactlyOneWinner) {
    RaceState rs;
    std::atomic<int> win_count{0};

    // All threads use the same "fastest" time — only one should win.
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&, i] {
            if (rs.try_claim_fastest_lap(i, 78500.0f)) {
                win_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(win_count.load(), 1);
    EXPECT_GE(rs.get_fastest_lap_holder(), 0);
    EXPECT_LT(rs.get_fastest_lap_holder(), 5);
}

// Memory ordering test: producer writes data THEN sets flag with release.
// Consumer acquires flag THEN reads data. Data must be visible.
TEST(RaceStateTest, ReleaseAcquireHappensBefore) {
    std::atomic<int>  data{0};
    std::atomic<bool> flag{false};

    std::thread producer([&] {
        data.store(42, std::memory_order_relaxed);
        flag.store(true, std::memory_order_release); // publishes data write
    });

    std::thread consumer([&] {
        while (!flag.load(std::memory_order_acquire)) {} // spins until flag
        // After acquiring flag, data write from producer MUST be visible.
        EXPECT_EQ(data.load(std::memory_order_relaxed), 42);
    });

    producer.join();
    consumer.join();
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

## Update `tests/CMakeLists.txt`
**Action:** EDIT — uncomment the phase5 block.

```cmake
add_phase_tests(phase5
    phase5/test_leaderboard.cpp
    phase5/test_race_state.cpp
)
```

---

## Building and Running

```bash
cmake --build build
ctest --test-dir build -R phase5 --output-on-failure

# TSan — especially important for the SWMR and atomic tests
cmake --build build-tsan
./build-tsan/tests/phase5_tests
```

---

## Phase 5 Gate

All tests green, zero TSan warnings:

```
[ RUN      ] LeaderboardTest.UpdateAndSnapshot
[ RUN      ] LeaderboardTest.SnapshotIsFullCopy
[ RUN      ] LeaderboardTest.SwmrNoTearing
[ RUN      ] LeaderboardTest.AtPosition
[ RUN      ] RaceStateTest.StartStopRace
[ RUN      ] RaceStateTest.SafetyCarBroadcast
[ RUN      ] RaceStateTest.LapCounterNoLostUpdates
[ RUN      ] RaceStateTest.FastestLapCasExactlyOneWinner
[ RUN      ] RaceStateTest.ReleaseAcquireHappensBefore
[  PASSED  ] 9 tests.
==================
WARNING: ThreadSanitizer: 0 issues found.
==================
```
