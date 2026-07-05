#pragma once

#include "common/types.h"
#include "concurrency/thread_pool.h"
#include <array>
#include <vector>
#include <future>

// StrategyAnalyzer finds the optimal pit lap for a driver.
// It submits 10 lightweight race simulations to the thread pool —
// one per candidate pit lap — and collects results via std::future.
//
// Key pattern: task batching with futures.
//   1. Submit all tasks at once (non-blocking).
//   2. Call future.get() on each (blocks until that task finishes).
//   3. Pick the best result.
//
// This is the same pattern used in quant systems to parallelise
// independent scenario analyses (pricing models, risk runs, etc.).

struct StrategyResult {
    int   pit_lap;
    float estimated_race_time_s; // lower is better
};

class StrategyAnalyzer {
public:
    explicit StrategyAnalyzer(ThreadPool& pool);

    // Returns the optimal pit lap for the given driver.
    // Blocks until all 10 simulations complete.
    StrategyResult analyze(const DriverState& driver,
                           const TrackProfile& track,
                           int total_laps);

    // Candidate pit laps to evaluate.
    static constexpr std::array<int, 10> CANDIDATES = {
        12, 15, 18, 21, 24, 27, 30, 33, 36, 39
    };

private:
    static float simulate_one_strategy(
        const DriverProfile& profile,
        const CarProfile& car,
        const TrackProfile& track,
        int pit_lap,
        int total_laps
    );

    ThreadPool& pool_;
};