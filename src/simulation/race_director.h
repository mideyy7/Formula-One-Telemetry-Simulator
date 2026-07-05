#pragma once

#include <latch>

// RaceDirector owns the one-shot "lights out" start latch.
//
// std::latch: a counter that starts at N and only ever counts DOWN to 0.
// Once it hits 0 it stays open forever — it cannot be reset. Use
// std::barrier (see lap_barrier.h) if you need a reusable checkpoint.
//
// Usage pattern:
//   RaceDirector dir{4};          // 4 threads must be ready
//   // In each thread:
//   dir.arrive_and_wait();        // counts down by 1, then blocks
//   // All 4 threads release simultaneously once the 4th arrives.
//
// Why this matters here: without a start latch, the telemetry generator
// could start pushing frames before the consumer thread has finished
// setting up, causing the consumer to read from a queue nobody is
// listening to yet (or to miss the very first frames).

class RaceDirector {
public:
    explicit RaceDirector(std::ptrdiff_t thread_count)
        : latch_{thread_count} {}

    // Call from each participating thread when it is ready.
    void signal_ready() { latch_.count_down(); }

    // Blocks until all threads have called signal_ready().
    void wait_for_start() { latch_.wait(); }

    // Combines signal_ready() + wait_for_start() in one call.
    void arrive_and_wait() { latch_.arrive_and_wait(); }

private:
    std::latch latch_;
};