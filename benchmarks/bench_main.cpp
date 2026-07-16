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
#include <deque>
#include <mutex>
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

struct LatencyStats { double p50, p95, p99, p999; };

static LatencyStats stats_from(std::vector<int64_t>& latencies) {
    std::sort(latencies.begin(), latencies.end());
    return {
        percentile(latencies, 50.0),
        percentile(latencies, 95.0),
        percentile(latencies, 99.0),
        percentile(latencies, 99.9)
    };
}

// ── 1. SPSC throughput ─────────────────────────────────────────────────────
// Count-based: producer pushes exactly N items; we time from the first push
// to when the consumer finishes the last pop. No Clock::now() in the hot
// path — that was adding ~5-10ns per iteration to the previous version.
//
// Templated on Align so the same body backs both the normal (Align=64,
// cache-line padded) queue and the Align=8 (natural alignment only, no
// cache-line padding, false-sharing-prone) ablation variant below — see
// bench_spsc_no_padding(). 8 is the practical floor: std::atomic<size_t>
// itself requires >= 8-byte alignment, so this is "as little padding as
// the type allows", not zero.

template<std::size_t Align>
static double bench_spsc_throughput_impl() {
    SpscQueue<TelemetryFrame, 4096, Align> q;
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

static double bench_spsc() { return bench_spsc_throughput_impl<64>(); }

// ── 1b. Cache-line padding ablation ────────────────────────────────────────
// Same benchmark, same queue, only Align changes: 64 (a real cache line,
// the shipped default) vs 8 (natural alignment only -- tail_/head_cache_
// and head_/tail_cache_ collapse onto the same cache line, so the
// producer's writes to tail_ and the consumer's writes to head_ now
// invalidate each other's cache line every op). This turns "the padding
// avoids false sharing" from an assertion in a comment into a measured
// number.
static double bench_spsc_no_padding() { return bench_spsc_throughput_impl<8>(); }

// ── 1c. SPSC vs a naive std::mutex + std::deque baseline ──────────────────
// Answers the question the throughput number alone can't: does the
// lock-free design actually buy anything over the obvious alternative?
// Time-bounded (2s) rather than count-based like bench_spsc() -- a fixed
// 10M-iteration count that took ~0.2s lock-free would take dramatically
// longer under mutex contention, so a fixed window keeps runtime sane
// while still producing a stable ops/sec estimate.
static double bench_spsc_mutex_baseline() {
    std::deque<TelemetryFrame> q;
    std::mutex m;

    TelemetryFrame frame;
    std::memcpy(frame.driver_id, "VER", 4);
    frame.speed_kph = 320.0f;
    frame.fuel_kg   = 95.0f;

    std::atomic<bool>     running{true};
    std::atomic<uint64_t> consumed{0};

    std::thread consumer([&] {
        while (true) {
            bool got = false;
            {
                std::lock_guard lock{m};
                if (!q.empty()) {
                    q.pop_front();
                    got = true;
                }
            }
            if (got) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else if (!running.load(std::memory_order_relaxed)) {
                break; // producer stopped and queue is drained
            }
        }
    });

    const auto until = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < until) {
        std::lock_guard lock{m};
        q.push_back(frame);
    }
    running.store(false, std::memory_order_release);
    consumer.join();

    return static_cast<double>(consumed.load()) / 2.0 / 1e6;
}

// ── 1d. SPSC push() call latency ───────────────────────────────────────────
// bench_spsc() measures throughput; bench_e2e_latency() (below) measures
// push-to-pop round trip. Neither reports the distribution of a single
// push() call's own cost. Timestamping every call adds overhead the
// throughput benchmark deliberately avoids -- accepted here because
// latency distribution, not raw throughput, is the point. Includes any
// spin-wait if the queue is transiently full, which is a real part of the
// latency a caller would actually observe, not just the atomic op cost.
static LatencyStats bench_spsc_push_latency() {
    SpscQueue<TelemetryFrame, 4096> q;
    static constexpr int N = 100'000;

    TelemetryFrame frame;
    std::memcpy(frame.driver_id, "VER", 4);

    std::atomic<bool> running{true};
    std::thread consumer([&] {
        TelemetryFrame f;
        while (running.load(std::memory_order_relaxed)) { q.pop(f); }
        while (q.pop(f)) {} // drain stragglers
    });

    std::vector<int64_t> latencies;
    latencies.reserve(N);
    for (int i = 0; i < N; ++i) {
        const int64_t start = now_ns();
        while (!q.push(frame)) {}
        latencies.push_back(now_ns() - start);
    }
    running.store(false, std::memory_order_relaxed);
    consumer.join();

    return stats_from(latencies);
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

// ── 2b. MPSC vs a naive std::mutex + std::deque baseline ──────────────────
// Same shape as bench_spsc_mutex_baseline(), extended to N producers
// contending on one mutex -- exactly the case where a lock-free design is
// expected to pull ahead the most.
static double bench_mpsc_mutex_baseline(int num_producers) {
    std::deque<RaceControlEvent> q;
    std::mutex m;
    std::atomic<bool>     running{true};
    std::atomic<uint64_t> consumed{0};

    auto producer_fn = [&] {
        RaceControlEvent ev;
        ev.type = RaceControlEvent::Type::TRACK_LIMITS;
        std::memcpy(ev.driver_id, "NOR", 4);
        ev.lap = 5;
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < until) {
            std::lock_guard lock{m};
            q.push_back(ev);
        }
    };

    std::thread consumer([&] {
        while (true) {
            bool got = false;
            {
                std::lock_guard lock{m};
                if (!q.empty()) {
                    q.pop_front();
                    got = true;
                }
            }
            if (got) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else if (!running.load(std::memory_order_relaxed)) {
                break;
            }
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (int i = 0; i < num_producers; ++i) producers.emplace_back(producer_fn);
    for (auto& t : producers) t.join();

    running.store(false, std::memory_order_release);
    consumer.join();

    return static_cast<double>(consumed.load()) / 2.0 / 1e6;
}

// ── 2c. MPSC push() call latency ───────────────────────────────────────────
// Per-call latency across N concurrent producers -- captures the real cost
// of push() under contention (heap allocation for the node + the atomic
// exchange + the link store), not just an aggregate rate.
static LatencyStats bench_mpsc_push_latency(int num_producers) {
    MpscQueue<RaceControlEvent> q;
    std::atomic<bool> running{true};

    std::thread consumer([&] {
        RaceControlEvent ev;
        while (running.load(std::memory_order_relaxed) || !q.empty()) {
            q.pop(ev);
        }
    });

    static constexpr int PER_PRODUCER = 20'000;
    std::vector<std::vector<int64_t>> per_thread(num_producers);
    std::vector<std::thread> producers;
    producers.reserve(num_producers);

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p] {
            auto& latencies = per_thread[p];
            latencies.reserve(PER_PRODUCER);
            RaceControlEvent ev;
            ev.type = RaceControlEvent::Type::TRACK_LIMITS;
            std::memcpy(ev.driver_id, "NOR", 4);
            for (int i = 0; i < PER_PRODUCER; ++i) {
                const int64_t start = now_ns();
                q.push(ev);
                latencies.push_back(now_ns() - start);
            }
        });
    }
    for (auto& t : producers) t.join();
    running.store(false, std::memory_order_release);
    consumer.join();

    std::vector<int64_t> all;
    all.reserve(static_cast<std::size_t>(num_producers) * PER_PRODUCER);
    for (auto& v : per_thread) all.insert(all.end(), v.begin(), v.end());
    return stats_from(all);
}

// ── 3. Thread pool task-dispatch latency ───────────────────────────────────
// Batch-drain: submit BATCH (= worker count) tasks, wait for all to
// complete, repeat for THREAD_POOL_ROUNDS rounds. Workers are warm between
// batches so we measure steady-state dispatch latency, not the queueing
// delay from one instantaneous burst of BATCH*ROUNDS tasks. Total
// submissions scales with hardware_concurrency() -- main() reports the
// real number in the JSON output rather than a hardcoded guess.
static constexpr int THREAD_POOL_ROUNDS = 10'000;

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
    const int ROUNDS = THREAD_POOL_ROUNDS;
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

    return stats_from(latencies);
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

// ── 4b. Leaderboard write latency ──────────────────────────────────────────
// The read-side benchmark above only tells half the shared_mutex story.
// This measures the writer's update() cost -- an exclusive lock plus a
// 20-element vector copy -- under concurrent read pressure, which is the
// actual tradeoff a SWMR lock makes: readers proceed in parallel, but the
// writer can be held up behind them.
static LatencyStats bench_leaderboard_write_latency(int num_readers) {
    Leaderboard lb;
    std::vector<DriverState> seed(20);
    for (int i = 0; i < 20; ++i) {
        seed[i].position   = i + 1;
        seed[i].profile.id = "D" + std::to_string(i);
    }
    lb.update(seed);

    std::atomic<bool> running{true};
    std::vector<std::thread> readers;
    readers.reserve(num_readers);
    for (int i = 0; i < num_readers; ++i) {
        readers.emplace_back([&] {
            while (running.load(std::memory_order_relaxed)) {
                auto snap = lb.snapshot();
                if (snap.empty()) __builtin_unreachable();
            }
        });
    }

    static constexpr int N = 5'000;
    std::vector<int64_t> latencies;
    latencies.reserve(N);
    std::vector<DriverState> data = seed;
    for (int i = 0; i < N; ++i) {
        const int64_t start = now_ns();
        lb.update(data);
        latencies.push_back(now_ns() - start);
    }

    running.store(false, std::memory_order_release);
    for (auto& t : readers) t.join();

    return stats_from(latencies);
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
    return stats_from(latencies);
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());

    const double spsc            = bench_spsc();
    const double spsc_no_padding = bench_spsc_no_padding();
    const double spsc_mutex      = bench_spsc_mutex_baseline();
    const auto   spsc_lat        = bench_spsc_push_latency();

    const int    mpsc_n     = std::max(2, cores / 2);
    const double mpsc       = bench_mpsc(mpsc_n);
    const double mpsc_mutex = bench_mpsc_mutex_baseline(mpsc_n);
    const auto   mpsc_lat   = bench_mpsc_push_latency(mpsc_n);

    const auto   tp       = bench_thread_pool();
    const int    tp_total = cores * THREAD_POOL_ROUNDS;

    const int    readers    = std::max(2, cores - 1);
    const double lb         = bench_leaderboard(readers);
    const auto   lb_write   = bench_leaderboard_write_latency(readers);

    const auto   e2e = bench_e2e_latency();

    fprintf(stdout,
        "{\n"
        "  \"hardware_concurrency\": %d,\n"
        "  \"spsc_throughput_mops\": %.2f,\n"
        "  \"spsc_no_padding_throughput_mops\": %.2f,\n"
        "  \"spsc_mutex_baseline_mops\": %.2f,\n"
        "  \"spsc_push_p50_ns\":  %.0f,\n"
        "  \"spsc_push_p95_ns\":  %.0f,\n"
        "  \"spsc_push_p99_ns\":  %.0f,\n"
        "  \"spsc_push_p999_ns\": %.0f,\n"
        "  \"mpsc_producers\": %d,\n"
        "  \"mpsc_throughput_mops\": %.2f,\n"
        "  \"mpsc_mutex_baseline_mops\": %.2f,\n"
        "  \"mpsc_push_p50_ns\":  %.0f,\n"
        "  \"mpsc_push_p95_ns\":  %.0f,\n"
        "  \"mpsc_push_p99_ns\":  %.0f,\n"
        "  \"mpsc_push_p999_ns\": %.0f,\n"
        "  \"thread_pool_workers\": %d,\n"
        "  \"thread_pool_total_submissions\": %d,\n"
        "  \"thread_pool_p50_ns\":  %.0f,\n"
        "  \"thread_pool_p95_ns\":  %.0f,\n"
        "  \"thread_pool_p99_ns\":  %.0f,\n"
        "  \"thread_pool_p999_ns\": %.0f,\n"
        "  \"leaderboard_readers\": %d,\n"
        "  \"leaderboard_mreads_per_sec\": %.2f,\n"
        "  \"leaderboard_write_p50_ns\":  %.0f,\n"
        "  \"leaderboard_write_p95_ns\":  %.0f,\n"
        "  \"leaderboard_write_p99_ns\":  %.0f,\n"
        "  \"leaderboard_write_p999_ns\": %.0f,\n"
        "  \"pipeline_e2e_p50_ns\":  %.0f,\n"
        "  \"pipeline_e2e_p95_ns\":  %.0f,\n"
        "  \"pipeline_e2e_p99_ns\":  %.0f,\n"
        "  \"pipeline_e2e_p999_ns\": %.0f\n"
        "}\n",
        cores,
        spsc,
        spsc_no_padding,
        spsc_mutex,
        spsc_lat.p50, spsc_lat.p95, spsc_lat.p99, spsc_lat.p999,
        mpsc_n, mpsc,
        mpsc_mutex,
        mpsc_lat.p50, mpsc_lat.p95, mpsc_lat.p99, mpsc_lat.p999,
        cores,
        tp_total,
        tp.p50, tp.p95, tp.p99, tp.p999,
        readers, lb,
        lb_write.p50, lb_write.p95, lb_write.p99, lb_write.p999,
        e2e.p50, e2e.p95, e2e.p99, e2e.p999
    );
    return 0;
}
