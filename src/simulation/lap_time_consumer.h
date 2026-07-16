#pragma once

#include "common/types.h"
#include "common/race_state.h"
#include "concurrency/spsc_queue.h"
#include <thread>
#include <stop_token>

// Drains the SPSC telemetry queue on its own thread and feeds completed lap
// times into RaceState's CAS-based fastest-lap claim. This is the consumer
// side of the producer/consumer pipeline TelemetryGenerator feeds — the
// queue's own doc comment always described this pairing; this class is it.
class LapTimeConsumer {
public:
    LapTimeConsumer(SpscQueue<TelemetryFrame, 2048>& queue, RaceState& race_state);

    void start();
    void stop();

private:
    void run(std::stop_token st);

    SpscQueue<TelemetryFrame, 2048>& queue_;
    RaceState&                       race_state_;
    std::jthread                     thread_;
};
