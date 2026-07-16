#include <gtest/gtest.h>
#include "simulation/telemetry_generator.h"
#include <chrono>
#include <thread>
#include <unordered_map>

TEST(TelemetryGeneratorTest, ProducesFrames) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};
    gen.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // ~10 ticks
    gen.stop();

    // Should have at least 1 frame per driver (20 frames minimum)
    EXPECT_GT(queue.size_approx(), 20u);
}

TEST(TelemetryGeneratorTest, SpeedInRealisticRange) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};
    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    gen.stop();

    TelemetryFrame frame;
    while (queue.pop(frame)) {
        EXPECT_GE(frame.speed_kph, 0.0f)   << "driver=" << frame.driver_id;
        EXPECT_LE(frame.speed_kph, 400.0f) << "driver=" << frame.driver_id;
    }
}

TEST(TelemetryGeneratorTest, TireWearIncreases) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};

    float first_wear = -1.0f;
    float last_wear  = -1.0f;

    gen.set_on_tick([&](const std::vector<DriverState>& states) {
        if (first_wear < 0.0f) first_wear = states[0].latest_frame.tire_wear;
        last_wear = states[0].latest_frame.tire_wear;
    });

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // ~25 ticks
    gen.stop();

    EXPECT_GE(last_wear, first_wear);
    EXPECT_GT(last_wear, 0.0f);
}

TEST(TelemetryGeneratorTest, FuelDecreases) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};

    float first_fuel = -1.0f, last_fuel = 101.0f;

    gen.set_on_tick([&](const std::vector<DriverState>& states) {
        if (first_fuel < 0.0f) first_fuel = states[0].latest_frame.fuel_kg;
        last_fuel = states[0].latest_frame.fuel_kg;
    });

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    gen.stop();

    EXPECT_LE(last_fuel, first_fuel);
}

TEST(TelemetryGeneratorTest, StopsWithinTimeout) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};
    gen.start();

    auto t0 = std::chrono::steady_clock::now();
    gen.stop(); // should return quickly after stop_token fires
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        100LL // must stop within 100ms (one tick is 20ms)
    );
}

TEST(TelemetryGeneratorTest, TwentyDriversProduceFrames) {
    SpscQueue<TelemetryFrame, 2048> queue;
    TelemetryGenerator gen{queue};
    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // ~5 ticks
    gen.stop();

    std::unordered_map<std::string, int> counts;
    TelemetryFrame frame;
    while (queue.pop(frame)) counts[frame.driver_id]++;

    EXPECT_EQ(counts.size(), 20u); // all 20 drivers have frames
    for (auto& [id, cnt] : counts) {
        EXPECT_GT(cnt, 0) << "No frames for driver: " << id;
    }
}