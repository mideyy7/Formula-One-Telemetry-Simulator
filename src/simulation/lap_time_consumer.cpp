#include "simulation/lap_time_consumer.h"
#include "common/season_data.h"
#include <chrono>
#include <string_view>

LapTimeConsumer::LapTimeConsumer(SpscQueue<TelemetryFrame, 2048>& queue, RaceState& race_state)
    : queue_(queue), race_state_(race_state) {}

void LapTimeConsumer::start() {
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void LapTimeConsumer::stop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void LapTimeConsumer::run(std::stop_token st) {
    TelemetryFrame frame;
    while (!st.stop_requested()) {
        if (queue_.pop(frame)) {
            if (frame.lap_time_ms > 0.0f) {
                int idx = driver_index_of(std::string_view(frame.driver_id));
                if (idx >= 0) {
                    race_state_.try_claim_fastest_lap(idx, frame.lap_time_ms);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Drain whatever's left so a race that just finished isn't missing its
    // last few laps' worth of fastest-lap candidates.
    while (queue_.pop(frame)) {
        if (frame.lap_time_ms > 0.0f) {
            int idx = driver_index_of(std::string_view(frame.driver_id));
            if (idx >= 0) {
                race_state_.try_claim_fastest_lap(idx, frame.lap_time_ms);
            }
        }
    }
}
