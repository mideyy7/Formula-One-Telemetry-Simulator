#pragma once

#include "common/types.h"
#include "concurrency/mpsc_queue.h"
#include <shared_mutex>
#include <random>

// WeatherSystem manages track conditions.
// Written by a single background thread; read by all 20 driver update calls.
//
// shared_mutex (SWMR — Single Writer Multiple Readers):
//   unique_lock  -> exclusive write: blocks all readers
//   shared_lock  -> concurrent read: multiple readers proceed in parallel
//
// Why not a plain mutex? The driver threads read weather every tick (50Hz x 20).
// A plain mutex would serialise all 20 reads unnecessarily. shared_mutex lets
// all 20 driver reads happen simultaneously, only blocking when the weather
// writer holds the exclusive lock (rare — once every few laps).

class WeatherSystem {
public:
    explicit WeatherSystem(MpscQueue<RaceControlEvent>& event_queue);

    // Called periodically (later: from the thread pool).
    // May transition weather state and push an event.
    void update(int current_lap);

    // Called from driver code — acquires a shared (read) lock.
    WeatherState current() const;

    // Grip multiplier based on weather: DRY=1.0, DAMP=0.85, WET=0.70
    float grip_factor() const;

private:
    MpscQueue<RaceControlEvent>& events_;
    WeatherState                 state_{WeatherState::DRY};
    mutable std::shared_mutex    mutex_;
    std::mt19937                 rng_{std::random_device{}()};
    std::uniform_real_distribution<float> coin_{0.0f, 1.0f};
    int last_update_lap_{0};

    static constexpr float CHANGE_PROBABILITY = 0.15f; // per update call
};
