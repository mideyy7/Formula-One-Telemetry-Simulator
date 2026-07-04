#include <gtest/gtest.h>
#include "concurrency/mpsc_queue.h"
#include <thread>
#include <atomic>
#include <vector>

TEST(MpscQueueTest, SingleProducerSingleConsumer) {
    MpscQueue<int> q;
    for (int i = 0; i < 1000; ++i) q.push(i);

    int count = 0;
    int val;
    while (q.pop(val)) ++count;
    EXPECT_EQ(count, 1000);
}

TEST(MpscQueueTest, EmptyPopReturnsFalse) {
    MpscQueue<int> q;
    int val;
    EXPECT_FALSE(q.pop(val));
}

// Multi-producer stress: 4 threads each push N items.
// Consumer counts total pops — must equal 4*N.
TEST(MpscQueueTest, MultiProducerStress) {
    MpscQueue<int> q;
    constexpr int PRODUCERS = 4;
    constexpr int PER_PRODUCER = 10'000;
    std::atomic<int> total_pushed{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < PER_PRODUCER; ++i) {
                q.push(p * PER_PRODUCER + i);
                total_pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : producers) t.join();

    int count = 0;
    int val;
    while (q.pop(val)) ++count;

    EXPECT_EQ(count, PRODUCERS * PER_PRODUCER);
}

// Verify no items are lost under concurrent push+pop.
TEST(MpscQueueTest, NoItemsLostConcurrent) {
    MpscQueue<int> q;
    constexpr int N = 50'000;
    std::atomic<long long> sum_in{0}, sum_out{0};

    std::thread prod1([&] {
        for (int i = 0; i < N; ++i) {
            q.push(i);
            sum_in.fetch_add(i, std::memory_order_relaxed);
        }
    });
    std::thread prod2([&] {
        for (int i = N; i < 2 * N; ++i) {
            q.push(i);
            sum_in.fetch_add(i, std::memory_order_relaxed);
        }
    });

    prod1.join();
    prod2.join();

    int val;
    while (q.pop(val)) {
        sum_out.fetch_add(val, std::memory_order_relaxed);
    }

    EXPECT_EQ(sum_in.load(), sum_out.load());
}