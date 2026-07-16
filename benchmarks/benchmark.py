#!/usr/bin/env python3
"""
benchmark.py  —  Build rcz_bench in Release mode, run it, and print
CV metrics for RaceCondition-Z.

Usage:
    python3 benchmark.py          # build + run
    python3 benchmark.py --run    # skip build, re-run existing binary
"""

import subprocess
import json
import sys
import os
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT       = os.path.dirname(SCRIPT_DIR)  # repo root — benchmarks/CMakeLists.txt is only valid via add_subdirectory() from here
BUILD_DIR  = os.path.join(SCRIPT_DIR, "build-bench")
BENCH_BIN  = os.path.join(BUILD_DIR, "benchmarks", "rcz_bench")

W = 66  # display width


# ── Build ──────────────────────────────────────────────────────────────────

def build():
    print("── Building rcz_bench (Release / -O3) " + "─" * 27)
    os.makedirs(BUILD_DIR, exist_ok=True)

    cmake_configure = [
        "cmake", "-B", BUILD_DIR, "-S", ROOT,
        "-DCMAKE_BUILD_TYPE=Release",
    ]
    cmake_build = [
        "cmake", "--build", BUILD_DIR,
        "--target", "rcz_bench",
        "--parallel",
    ]

    for cmd in [cmake_configure, cmake_build]:
        print(f"  $ {' '.join(cmd)}")
        try:
            subprocess.run(cmd, check=True)
        except subprocess.CalledProcessError as e:
            sys.exit(f"\nBuild failed (exit {e.returncode}). "
                     "Fix errors above and retry.")

    print()


# ── Run ───────────────────────────────────────────────────────────────────

def run_benchmark() -> dict:
    if not os.path.exists(BENCH_BIN):
        sys.exit(f"Binary not found: {BENCH_BIN}\nRun without --run to build first.")

    print("── Running benchmarks (~20 s) " + "─" * 36)
    print(f"  $ {BENCH_BIN}\n")

    result = subprocess.run(
        [BENCH_BIN],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        sys.exit(f"Benchmark binary exited with code {result.returncode}.")

    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as e:
        print("Raw output:", result.stdout[:500], file=sys.stderr)
        sys.exit(f"Failed to parse JSON output: {e}")


# ── Formatting helpers ─────────────────────────────────────────────────────

def fmt_ns(ns: float) -> str:
    """Format a nanosecond value with the most readable unit."""
    if ns < 1_000:
        return f"{ns:.0f} ns"
    if ns < 1_000_000:
        return f"{ns / 1_000:.1f} µs"
    return f"{ns / 1_000_000:.2f} ms"

def fmt_mops(mops: float) -> str:
    if mops >= 1_000:
        return f"{mops / 1_000:.2f}B ops/sec"
    return f"{mops:.2f}M ops/sec"

def fmt_x(factor: float) -> str:
    return f"{factor:.1f}x"


# ── Results display ────────────────────────────────────────────────────────

def print_results(d: dict):
    sep  = "═" * W
    dash = "─" * W

    print(f"\n{sep}")
    print(f"  RaceCondition-Z  ·  Benchmark Results")
    print(f"  Hardware: {d['hardware_concurrency']} logical cores")
    print(sep)

    spsc_speedup_mutex = d['spsc_throughput_mops'] / d['spsc_mutex_baseline_mops']
    spsc_speedup_pad   = d['spsc_throughput_mops'] / d['spsc_no_padding_throughput_mops']
    print(f"\n  1. Lock-free SPSC Queue  (1 producer → 1 consumer, TelemetryFrame)")
    print(f"     Throughput              : {fmt_mops(d['spsc_throughput_mops'])}")
    print(f"     vs std::mutex+deque     : {fmt_mops(d['spsc_mutex_baseline_mops'])}  ({fmt_x(spsc_speedup_mutex)} faster)")
    print(f"     vs no cache-line padding: {fmt_mops(d['spsc_no_padding_throughput_mops'])}  ({fmt_x(spsc_speedup_pad)} faster with padding)")
    print(f"     push() latency  p50 {fmt_ns(d['spsc_push_p50_ns'])} / p95 {fmt_ns(d['spsc_push_p95_ns'])} "
          f"/ p99 {fmt_ns(d['spsc_push_p99_ns'])} / p99.9 {fmt_ns(d['spsc_push_p999_ns'])}")

    mpsc_speedup_mutex = d['mpsc_throughput_mops'] / d['mpsc_mutex_baseline_mops']
    print(f"\n  2. Lock-free MPSC Queue  ({d['mpsc_producers']} producers → 1 consumer, RaceControlEvent)")
    print(f"     Throughput              : {fmt_mops(d['mpsc_throughput_mops'])}")
    print(f"     vs std::mutex+deque     : {fmt_mops(d['mpsc_mutex_baseline_mops'])}  ({fmt_x(mpsc_speedup_mutex)} faster)")
    print(f"     push() latency  p50 {fmt_ns(d['mpsc_push_p50_ns'])} / p95 {fmt_ns(d['mpsc_push_p95_ns'])} "
          f"/ p99 {fmt_ns(d['mpsc_push_p99_ns'])} / p99.9 {fmt_ns(d['mpsc_push_p999_ns'])}")

    print(f"\n  3. Thread Pool  ({d['thread_pool_workers']} workers, 200K tasks, submit→start latency)")
    print(f"     p50   : {fmt_ns(d['thread_pool_p50_ns'])}")
    print(f"     p95   : {fmt_ns(d['thread_pool_p95_ns'])}")
    print(f"     p99   : {fmt_ns(d['thread_pool_p99_ns'])}")
    print(f"     p99.9 : {fmt_ns(d['thread_pool_p999_ns'])}")

    print(f"\n  4. Leaderboard SWMR  ({d['leaderboard_readers']} readers + 1 writer, 20-driver snapshot)")
    print(f"     Read throughput  : {fmt_mops(d['leaderboard_mreads_per_sec'])}")
    print(f"     Write latency (under concurrent read load):")
    print(f"       p50 {fmt_ns(d['leaderboard_write_p50_ns'])} / p95 {fmt_ns(d['leaderboard_write_p95_ns'])} "
          f"/ p99 {fmt_ns(d['leaderboard_write_p99_ns'])} / p99.9 {fmt_ns(d['leaderboard_write_p999_ns'])}")

    print(f"\n  5. End-to-End Pipeline  (SPSC push→pop latency, 100K samples)")
    print(f"     p50   : {fmt_ns(d['pipeline_e2e_p50_ns'])}")
    print(f"     p95   : {fmt_ns(d['pipeline_e2e_p95_ns'])}")
    print(f"     p99   : {fmt_ns(d['pipeline_e2e_p99_ns'])}")
    print(f"     p99.9 : {fmt_ns(d['pipeline_e2e_p999_ns'])}")

    # ── CV bullets ──────────────────────────────────────────────────────────
    # Every number here comes straight from this run's JSON -- no claim that
    # isn't backed by a field the binary actually measured.
    spsc_mops = d["spsc_throughput_mops"]
    tp_p99    = fmt_ns(d["thread_pool_p99_ns"])
    lb_mops   = d["leaderboard_mreads_per_sec"]
    readers   = d["leaderboard_readers"]
    e2e_p99   = fmt_ns(d["pipeline_e2e_p99_ns"])
    workers   = d["thread_pool_workers"]
    mpsc_n    = d["mpsc_producers"]
    mpsc_mops = d["mpsc_throughput_mops"]

    print(f"\n{dash}")
    print(f"  Suggested CV bullets  (copy-paste the numbers below)\n")

    bullets = [
        (
            f"Engineered a lock-free SPSC ring buffer (acquire/release memory "
            f"ordering, cache-line-aligned heads) sustaining {spsc_mops:.0f}M "
            f"frames/sec — {spsc_speedup_mutex:.1f}x a std::mutex+deque baseline "
            f"measured on the same workload, and {spsc_speedup_pad:.1f}x faster "
            f"than the same queue with cache-line padding removed, confirming "
            f"the false-sharing fix actually matters."
        ),
        (
            f"Implemented a lock-free MPSC event bus (Vyukov linked-list design, "
            f"exchange-based enqueue) reaching {mpsc_mops:.0f}M events/sec across "
            f"{mpsc_n} concurrent producers — {mpsc_speedup_mutex:.1f}x a "
            f"std::mutex+deque baseline under the same contention."
        ),
        (
            f"Built a thread pool ({workers} workers, std::packaged_task + "
            f"std::future) achieving p99 task-dispatch latency of {tp_p99} "
            f"over 200K submissions."
        ),
        (
            f"Protected shared leaderboard with std::shared_mutex (SWMR pattern), "
            f"sustaining {lb_mops:.0f}M snapshot reads/sec across {readers} "
            f"concurrent readers while separately measuring the writer's own "
            f"update() latency under that same read pressure, not just read "
            f"throughput in isolation."
        ),
        (
            f"Measured end-to-end pipeline latency (producer push → consumer pop) "
            f"at {e2e_p99} p99 across 100K timestamped frames, and separately "
            f"isolated push()-call latency from round-trip latency to distinguish "
            f"queue-op cost from consumer scheduling delay."
        ),
    ]

    for i, b in enumerate(bullets, 1):
        # Word-wrap to W-4 chars with hanging indent
        words = b.split()
        line  = f"  {i}. "
        indent = "     "
        for w in words:
            if len(line) + len(w) + 1 > W:
                print(line)
                line = indent + w + " "
            else:
                line += w + " "
        print(line.rstrip())
        print()

    print(dash)
    print("  Run `python3 benchmark.py --run` to skip the build step next time.")
    print(dash + "\n")


# ── Entry point ────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="RaceCondition-Z benchmark runner")
    parser.add_argument("--run", action="store_true",
                        help="Skip build step; run existing binary")
    args = parser.parse_args()

    if not args.run:
        build()

    data = run_benchmark()
    print_results(data)


if __name__ == "__main__":
    main()