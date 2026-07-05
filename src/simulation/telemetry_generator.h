#pragma once

#include "common/types.h"
#include "common/season_data.h"
#include "concurrency/spsc_queue.h"
#include <thread>
#include <stop_token>
#include <vector>
#include <random>
#include <functional>

// Simulation constants
inline constexpr float TIME_SCALE     = 120.0f; // sim runs 120x faster than real time
inline constexpr float TICK_S         = 0.02f;  // 20ms per real tick (50Hz)
inline constexpr float SIM_S_PER_TICK = TIME_SCALE * TICK_S; // 2.4 sim-seconds per tick
inline constexpr float TIRE_WEAR_BASE = 0.00130f; // calibrated: tires last ~30 laps
inline constexpr int   PIT_STOP_TICKS = 11;       // 25 sim-seconds / 2.4 ~= 11 ticks
inline constexpr int   TOTAL_LAPS     = 50;

// Callback type: called after every tick with the updated states.
// Used later by the leaderboard to snapshot current standings.
using OnTickCallback = std::function<void(const std::vector<DriverState>&)>;

// Generates fake telemetry for 20 drivers at 50Hz on a background thread.
// This is the "producer" in the producer/consumer pipeline built in Phase 2.
class TelemetryGenerator {
public:
    explicit TelemetryGenerator(
        SpscQueue<TelemetryFrame, 2048>& queue,
        const TrackProfile& track = DEFAULT_TRACK
    );

    // Register a callback invoked after every tick (runs on the producer thread).
    void set_on_tick(OnTickCallback cb) { on_tick_ = std::move(cb); }

    // Start the producer thread.
    void start();

    // Ask the thread to stop, then block until it actually has (join).
    void stop();

    bool is_running() const { return thread_.joinable(); }

    // Returns a copy of the current standings — safe to call from any thread
    // because it copies the vector rather than handing out a reference.
    std::vector<DriverState> standings() const;

    int race_lap() const { return race_lap_; }
    bool race_finished() const { return race_finished_; }

private:
    void run(std::stop_token st);  // thread entry point
    void tick();
    void update_positions_and_drs();
    void handle_pit(DriverState& state);
    bool should_pit(const DriverState& state) const;
    static float max_speed(const CarProfile& car);

    SpscQueue<TelemetryFrame, 2048>& queue_;
    TrackProfile                     track_;
    std::vector<DriverState>         states_;
    std::jthread                     thread_;
    OnTickCallback                   on_tick_;

    std::mt19937                    rng_{42};
    std::normal_distribution<float> dist_{0.0f, 1.0f};

    int  race_lap_      {1};
    bool race_finished_ {false};
};