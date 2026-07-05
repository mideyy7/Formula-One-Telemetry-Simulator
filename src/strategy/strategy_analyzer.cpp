#include "strategy/strategy_analyzer.h"
#include <limits>
#include <algorithm>

StrategyAnalyzer::StrategyAnalyzer(ThreadPool& pool)
    : pool_(pool) {}

StrategyResult StrategyAnalyzer::analyze(
    const DriverState& driver,
    const TrackProfile& track,
    int total_laps)
{
    // Submit all 10 scenarios to the thread pool simultaneously.
    std::vector<std::future<float>> futures;
    futures.reserve(CANDIDATES.size());

    for (int pit_lap : CANDIDATES) {
        futures.push_back(pool_.submit(
            simulate_one_strategy,
            driver.profile, driver.car, track, pit_lap, total_laps
        ));
    }

    // Collect results — each .get() blocks until that task is done.
    StrategyResult best{CANDIDATES[0], std::numeric_limits<float>::max()};
    for (std::size_t i = 0; i < CANDIDATES.size(); ++i) {
        float t = futures[i].get();
        if (t < best.estimated_race_time_s) {
            best = {CANDIDATES[i], t};
        }
    }
    return best;
}

// Lightweight race simulation: estimates total race time given a pit lap.
// This runs on a worker thread — no shared state, fully independent.
float StrategyAnalyzer::simulate_one_strategy(
    const DriverProfile& profile,
    const CarProfile& car,
    const TrackProfile& track,
    int pit_lap,
    int total_laps)
{
    // Base lap time in seconds: top car ~87s, back-marker ~93s
    float base_lap_s = 87.0f + (1.0f - car.engine_power) * 8.0f;

    float time      = 0.0f;
    float tire_wear = 0.0f;
    float fuel_kg   = 100.0f;

    for (int lap = 1; lap <= total_laps; ++lap) {
        // Pit stop
        if (lap == pit_lap) {
            time     += 25.0f;    // pit lane time (seconds)
            tire_wear = 0.02f;    // fresh tires
            fuel_kg   = 100.0f;   // refueled
        }

        // Lap time penalties
        float tire_penalty = tire_wear * 3.0f; // up to ~3s on worn tires
        float fuel_penalty = (fuel_kg / 100.0f) * 0.4f; // heavy fuel = slower

        time += base_lap_s + tire_penalty + fuel_penalty;

        // State evolution
        tire_wear += 0.025f * profile.aggression;
        fuel_kg   -= track.fuel_consumption * track.length_km / total_laps;
        fuel_kg    = std::max(0.0f, fuel_kg);
    }

    return time;
}