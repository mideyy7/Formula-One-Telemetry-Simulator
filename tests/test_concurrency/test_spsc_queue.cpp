#include <gtest/gtest.h>
#include "concurrency/spsc_queue.h"
#include <thread>
#include <atomic>
#include <numeric>

// ─── Single-threaded correctness ─────────────────────────────────────────────

TEST(SpscQueueTest, PushPopRoundTrip) {
    SpscQueue<int, 8> q;
    EXPECT_TRUE(q.push(42));
    int val = 0;
    EXPECT_TRUE(q.pop(val));
    EXPECT_EQ(val, 42);
}

TEST(SpscQueueTest, EmptyPopReturnsFalse) {
    SpscQueue<int, 8> q;
    int val;
    EXPECT_FALSE(q.pop(val));
}

TEST(SpscQueueTest, FullPushReturnsFalse) {
    // Capacity=4 means 3 usable slots (one slot is always reserved as sentinel).
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_FALSE(q.push(4)); // queue full
}

TEST(SpscQueueTest, FifoOrder) {
    SpscQueue<int, 16> q;
    for (int i = 0; i < 10; ++i) q.push(i);
    for (int i = 0; i < 10; ++i) {
        int val;
        ASSERT_TRUE(q.pop(val));
        EXPECT_EQ(val, i);
    }
}

TEST(SpscQueueTest, WrapAround) {
    // Push/pop more items than Capacity to exercise the modulo wrap.
    SpscQueue<int, 8> q;
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 5; ++i) q.push(round * 5 + i);
        for (int i = 0; i < 5; ++i) {
            int val;
            ASSERT_TRUE(q.pop(val));
            EXPECT_EQ(val, round * 5 + i);
        }
    }
}

// ─── Concurrent stress ────────────────────────────────────────────────────────
// Run this under ThreadSanitizer: cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"

TEST(SpscQueueTest, ConcurrentStress) {
    SpscQueue<int, 1024> q;
    constexpr int N = 100'000;
    std::atomic<long long> sum_produced{0};
    std::atomic<long long> sum_consumed{0};

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            while (!q.push(i)) { /* spin — queue full, wait for consumer */ }
            sum_produced.fetch_add(i, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&] {
        int received = 0;
        int val;
        while (received < N) {
            if (q.pop(val)) {
                sum_consumed.fetch_add(val, std::memory_order_relaxed);
                ++received;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(sum_produced.load(), sum_consumed.load());
}