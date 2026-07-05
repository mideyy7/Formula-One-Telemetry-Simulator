#include "concurrency/spsc_queue.h"
#include "concurrency/mpsc_queue.h"
#include "concurrency/thread_pool.h"
#include "common/leaderboard.h"
#include "common/types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <thread>
#include <string>

using Clock = std::chrono::steady_clock;

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

static double percentile(const std::vector<int64_t>& sorted, double pct) {
    if (sorted.empty()) return 0.0;
    std::size_t idx = static_cast<std::size_t>(pct / 100.0 * (sorted.size() - 1));
    return static_cast<double>(sorted[idx]);
}

// ── 1. SPSC throughput ─────────────────────────────────────────────────────
// Measures peak frames/sec through the lock-free ring buffer.

static double bench_spsc() {
    SpscQueue<TelemetryFrame, 4096> q;
    std::atomic<uint64_t> ops{0};
    std::atomic<bool>     running{true};

    TelemetryFrame frame;
    frame.driver_id = "VER";
    frame.speed_kph = 320.0f;
    frame.fuel_kg   = 95.0f;

    std::thread consumer([&] {
        TelemetryFrame f;
        while (running.load(std::memory_order_relaxed)) {
            if (q.pop(f)) {
                ops.fetch_add(1, std::memory_order_relaxed);
                if (f.speed_kph < 0.0f) __builtin_unreachable();
            }
        }
        while (q.pop(f)) ops.fetch_add(1, std::memory_order_relaxed);
    });

    const auto until = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < until) {
        while (!q.push(frame)) {}
    }
    running.store(false, std::memory_order_release);
    consumer.join();

    return static_cast<double>(ops.load()) / 2.0 / 1e6;
}

// ── 2. MPSC throughput ─────────────────────────────────────────────────────
// N producers racing to push; 1 consumer drains. Measures total events/sec.

static double bench_mpsc(int num_producers) {
    MpscQueue<RaceControlEvent> q;
    std::atomic<bool>     running{true};
    std::atomic<uint64_t> consumed{0};

    auto producer_fn = [&] {
        RaceControlEvent ev;
        ev.type      = RaceControlEvent::Type::TRACK_LIMITS;
        ev.driver_id = "NOR";
        ev.lap       = 5;
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < until) q.push(ev);
    };

    std::thread consumer([&] {
        RaceControlEvent ev;
        while (running.load(std::memory_order_relaxed) || !q.empty()) {
            if (q.pop(ev)) consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (int i = 0; i < num_producers; ++i)
        producers.emplace_back(producer_fn);
    for (auto& t : producers) t.join();

    running.store(false, std::memory_order_release);
    consumer.join();

    return static_cast<double>(consumed.load()) / 2.0 / 1e6;
}

// ── 3. Thread pool task-dispatch latency ───────────────────────────────────
// Measures time from pool.submit() call to first instruction inside the task.

struct LatencyStats { double p50, p95, p99, p999; };

static LatencyStats bench_thread_pool() {
    const int N = 200'000;
    ThreadPool pool{std::thread::hardware_concurrency()};

    // Pre-allocate; each task writes to its own index — no mutex needed.
    std::vector<int64_t> latencies(N, 0);

    for (int i = 0; i < N; ++i) {
        const int64_t submit_ns = now_ns();
        const int     idx       = i;
        pool.submit([submit_ns, idx, &latencies]() {
            latencies[idx] = now_ns() - submit_ns;
        });
    }
    pool.submit([] {}).get(); // sentinel — blocks until all prior tasks finish

    std::sort(latencies.begin(), latencies.end());
    return {
        percentile(latencies, 50.0),
        percentile(latencies, 95.0),
        percentile(latencies, 99.0),
        percentile(latencies, 99.9)
    };
}

// ── 4. Leaderboard SWMR read throughput ───────────────────────────────────
// Measures snapshot() calls/sec with N concurrent readers + 1 background writer.

static double bench_leaderboard(int num_readers) {
    Leaderboard lb;

    std::vector<DriverState> seed(20);
    for (int i = 0; i < 20; ++i) {
        seed[i].position   = i + 1;
        seed[i].profile.id = "D" + std::to_string(i);
    }
    lb.update(seed);

    std::atomic<bool>     running{true};
    std::atomic<uint64_t> total_reads{0};

    std::thread writer([&] {
        std::vector<DriverState> data = seed;
        while (running.load(std::memory_order_relaxed)) {
            lb.update(data);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    std::vector<std::thread> readers;
    readers.reserve(num_readers);
    for (int i = 0; i < num_readers; ++i) {
        readers.emplace_back([&] {
            uint64_t local = 0;
            const auto until = Clock::now() + std::chrono::seconds(2);
            while (Clock::now() < until) {
                auto snap = lb.snapshot();
                if (snap.empty()) __builtin_unreachable();
                ++local;
            }
            total_reads.fetch_add(local, std::memory_order_relaxed);
        });
    }

    for (auto& t : readers) t.join();
    running.store(false, std::memory_order_release);
    writer.join();

    return static_cast<double>(total_reads.load()) / 2.0 / 1e6;
}

// ── 5. End-to-end pipeline latency ────────────────────────────────────────
// Stamps a timestamp into each item at push; consumer measures pop - push.

struct Timestamped {
    int64_t push_ns{0};
    int     seq{0};
};

static LatencyStats bench_e2e_latency() {
    SpscQueue<Timestamped, 4096> q;
    const int N = 100'000;

    std::vector<int64_t> latencies;
    latencies.reserve(N);

    std::thread consumer([&] {
        Timestamped item;
        for (int received = 0; received < N; ) {
            if (q.pop(item)) {
                latencies.push_back(now_ns() - item.push_ns);
                ++received;
            }
        }
    });

    for (int i = 0; i < N; ++i) {
        Timestamped item{now_ns(), i};
        while (!q.push(item)) {}
    }

    consumer.join();
    std::sort(latencies.begin(), latencies.end());
    return {
        percentile(latencies, 50.0),
        percentile(latencies, 95.0),
        percentile(latencies, 99.0),
        percentile(latencies, 99.9)
    };
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());

    // Run sequentially so benchmarks don't compete for CPU.
    const double spsc = bench_spsc();

    const int    mpsc_n = std::max(2, cores / 2);
    const double mpsc   = bench_mpsc(mpsc_n);

    const auto   tp      = bench_thread_pool();

    const int    readers = std::max(2, cores - 1);
    const double lb      = bench_leaderboard(readers);

    const auto   e2e = bench_e2e_latency();

    fprintf(stdout,
        "{\n"
        "  \"hardware_concurrency\": %d,\n"
        "  \"spsc_throughput_mops\": %.2f,\n"
        "  \"mpsc_producers\": %d,\n"
        "  \"mpsc_throughput_mops\": %.2f,\n"
        "  \"thread_pool_workers\": %d,\n"
        "  \"thread_pool_p50_ns\":  %.0f,\n"
        "  \"thread_pool_p95_ns\":  %.0f,\n"
        "  \"thread_pool_p99_ns\":  %.0f,\n"
        "  \"thread_pool_p999_ns\": %.0f,\n"
        "  \"leaderboard_readers\": %d,\n"
        "  \"leaderboard_mreads_per_sec\": %.2f,\n"
        "  \"pipeline_e2e_p50_ns\":  %.0f,\n"
        "  \"pipeline_e2e_p95_ns\":  %.0f,\n"
        "  \"pipeline_e2e_p99_ns\":  %.0f,\n"
        "  \"pipeline_e2e_p999_ns\": %.0f\n"
        "}\n",
        cores,
        spsc,
        mpsc_n, mpsc,
        cores,
        tp.p50, tp.p95, tp.p99, tp.p999,
        readers, lb,
        e2e.p50, e2e.p95, e2e.p99, e2e.p999
    );
    return 0;
}