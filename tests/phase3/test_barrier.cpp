#include <gtest/gtest.h>
#include "simulation/lap_barrier.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

TEST(BarrierTest, CompletionFunctionCalledOncePerPhase) {
    constexpr int THREADS = 5;
    constexpr int PHASES  = 10;
    std::atomic<int> completion_count{0};

    LapBarrier barrier{THREADS, [&]{
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