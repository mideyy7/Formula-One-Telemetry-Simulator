#include "race_control/track_limits.h"
#include <chrono>
#include <cstring>

TrackLimitsMonitor::TrackLimitsMonitor(MpscQueue<RaceControlEvent>& event_queue)
    : events_(event_queue) {}

void TrackLimitsMonitor::check(const std::vector<DriverState>& states,
                                int current_lap) {
    for (const auto& state : states) {
        if (state.in_pit) continue; // can't violate track limits in pits

        const auto& frame = state.latest_frame;

        float speed_factor = frame.speed_kph / 280.0f;
        float prob = BASE_RATE
                   * state.profile.aggression
                   * (1.0f + frame.tire_wear)
                   * speed_factor;

        if (coin_(rng_) < prob) {
            RaceControlEvent ev;
            ev.type      = RaceControlEvent::Type::TRACK_LIMITS;
            std::memcpy(ev.driver_id, state.profile.id.data(), 3);
            ev.lap       = current_lap;
            ev.message   = state.profile.id + " exceeded track limits (S"
                         + std::to_string(frame.sector) + ")";
            ev.timestamp = std::chrono::steady_clock::now();
            events_.push(std::move(ev));
        }
    }
}