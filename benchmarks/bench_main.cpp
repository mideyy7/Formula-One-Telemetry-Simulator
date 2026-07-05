#include "concurrency/spsc_queue.h"
#include "concurrency/mpsc_queue.h"
#include "concurrency/thread_pool.h"
#include "common/leaderboard.h"
#include "common/types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <thread>
#include <string>
#include <future>

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
// Count-based: producer pushes exactly N items; we time from the first push
// to when the consumer finishes the last pop. No Clock::now() in the hot
// path — that was adding ~5-10ns per iteration to the previous version.

static double bench_spsc() {
    SpscQueue<TelemetryFrame, 4096> q;
    static constexpr uint64_t N = 10'000'000;

    TelemetryFrame frame;
    std::memcpy(frame.driver_id, "VER", 4);
    frame.speed_kph = 320.0f;
    frame.fuel_kg   = 95.0f;

    std::atomic<int64_t> consumer_end_ns{0};

    std::thread consumer([&] {
        TelemetryFrame f;
        uint64_t count = 0;
        while (count < N) {
            if (q.pop(f)) {
                ++count;
                if (f.speed_kph < 0.0f) __builtin_unreachable();
            }
        }
        consumer_end_ns.store(now_ns(), std::memory_order_release);
    });

    const int64_t start = now_ns();
    for (uint64_t i = 0; i < N; ++i) {
        while (!q.push(frame)) {}
    }
    consumer.join();

    const double secs = (consumer_end_ns.load() - start) / 1e9;
    return static_cast<double>(N) / secs / 1e6;
}

// ── 2. MPSC throughput ─────────────────────────────────────────────────────

static double bench_mpsc(int num_producers) {
    MpscQueue<RaceControlEvent> q;
    std::atomic<bool>     running{true};
    std::atomic<uint64_t> consumed{0};

    auto producer_fn = [&] {
        RaceControlEvent ev;
        ev.type = RaceControlEvent::Type::TRACK_LIMITS;
        std::memcpy(ev.driver_id, "NOR", 4);
        ev.lap  = 5;
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
// Batch-drain: submit BATCH tasks, wait for all to complete, repeat.
// Workers are warm between batches so we measure steady-state dispatch
// latency — not the queueing delay from an instantaneous burst of 200K tasks.

struct LatencyStats { double p50, p95, p99, p999; };

static LatencyStats bench_thread_pool() {
    const int workers = static_cast<int>(std::thread::hardware_concurrency());
    ThreadPool pool{static_cast<std::size_t>(workers)};

    // Warm-up: wake all workers before measuring.
    {
        std::vector<std::future<void>> warm;
        warm.reserve(workers);
        for (int i = 0; i < workers; ++i)
            warm.push_back(pool.submit([] { /* spin warmup */ }));
        for (auto& f : warm) f.wait();
    }

    const int BATCH  = workers;      // one task per worker keeps them all busy
    const int ROUNDS = 10'000;       // repeat this many times
    const int TOTAL  = BATCH * ROUNDS;

    std::vector<int64_t> latencies(TOTAL, 0);

    for (int r = 0; r < ROUNDS; ++r) {
        std::vector<std::future<void>> futures;
        futures.reserve(BATCH);

        for (int i = 0; i < BATCH; ++i) {
            const int     idx       = r * BATCH + i;
            const int64_t submit_ns = now_ns();
            futures.push_back(pool.submit([submit_ns, idx, &latencies] {
                latencies[idx] = now_ns() - submit_ns;
            }));
        }
        for (auto& f : futures) f.wait();
    }

    std::sort(latencies.begin(), latencies.end());
    return {
        percentile(latencies, 50.0),
        percentile(latencies, 95.0),
        percentile(latencies, 99.0),
        percentile(latencies, 99.9)
    };
}

// ── 4. Leaderboard SWMR read throughput ───────────────────────────────────

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

struct Timestamped {
    int64_t push_ns{0};
    int     seq{0};
};

static LatencyStats bench_e2e_latency() {
    SpscQueue<Timestamped, 4096> q;
    static constexpr int N = 100'000;

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