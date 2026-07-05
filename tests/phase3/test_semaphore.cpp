#include <gtest/gtest.h>
#include "simulation/pit_lane.h"
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