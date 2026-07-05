#pragma once

#include "common/types.h"
#include "concurrency/mpsc_queue.h"
#include <vector>
#include <random>

// TrackLimitsMonitor is called periodically (later: submitted to the thread
// pool). For each driver, it probabilistically decides whether they exceeded
// track limits at a sector boundary. If so, it pushes a TRACK_LIMITS event.
//
// Violation probability formula:
//   P = base_rate x aggression x (1 + tire_wear) x speed_factor
// where speed_factor = speed_kph / 280.0 (normalised to typical F1 speed).

class TrackLimitsMonitor {
public:
    explicit TrackLimitsMonitor(MpscQueue<RaceControlEvent>& event_queue);

    // Call once per monitoring cycle with current driver states.
    void check(const std::vector<DriverState>& states, int current_lap);

private:
    MpscQueue<RaceControlEvent>& events_;
    std::mt19937                 rng_{std::random_device{}()};
    std::uniform_real_distribution<float> coin_{0.0f, 1.0f};

    static constexpr float BASE_RATE = 0.004f; // ~0.4% per sector crossing
};