#include <gtest/gtest.h>
#include "common/race_state.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

// Define the global declared `extern` in race_state.h.
// (In the real binary this would live in main.cpp; test binaries don't
// link main.cpp, so this file provides the one definition this binary needs.)
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
// Exactly one should win.
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