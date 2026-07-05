#include <gtest/gtest.h>
#include "simulation/race_director.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>

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