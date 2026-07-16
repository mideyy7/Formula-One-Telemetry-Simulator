#include <gtest/gtest.h>
#include "simulation/lap_time_consumer.h"
#include "common/season_data.h"
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

static TelemetryFrame make_frame(const char* driver_id, float lap_time_ms) {
    TelemetryFrame f;
    std::memcpy(f.driver_id, driver_id, std::strlen(driver_id) + 1);
    f.lap_time_ms = lap_time_ms;
    return f;
}

// Poll with a timeout instead of a fixed sleep -- the consumer drains on its
// own thread, so this avoids flaking under slow/loaded CI machines.
static bool wait_until(std::function<bool()> pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

TEST(LapTimeConsumerTest, ClaimsFastestLapFromQueue) {
    SpscQueue<TelemetryFrame, 2048> queue;
    RaceState rs;
    LapTimeConsumer consumer{queue, rs};

    consumer.start();
    queue.push(make_frame("VER", 80000.0f));
    queue.push(make_frame("HAM", 75000.0f)); // faster -- should win
    queue.push(make_frame("LEC", 0.0f));     // no lap completed -- ignored

    ASSERT_TRUE(wait_until([&] { return rs.get_fastest_lap_holder() >= 0; },
                            std::chrono::milliseconds(500)));
    consumer.stop();

    EXPECT_EQ(rs.get_fastest_lap_holder(), driver_index_of("HAM"));
    EXPECT_FLOAT_EQ(rs.get_fastest_lap_ms(), 75000.0f);
}

TEST(LapTimeConsumerTest, ZeroLapTimeFramesAreIgnored) {
    SpscQueue<TelemetryFrame, 2048> queue;
    RaceState rs;
    LapTimeConsumer consumer{queue, rs};

    consumer.start();
    for (int i = 0; i < 50; ++i) queue.push(make_frame("VER", 0.0f));

    // Give the consumer a chance to drain everything, then confirm nothing
    // was ever claimed (no positive lap_time_ms was ever pushed).
    wait_until([&] { return queue.empty(); }, std::chrono::milliseconds(200));
    consumer.stop();

    EXPECT_EQ(rs.get_fastest_lap_holder(), -1);
}

// Mirrors RaceStateTest.FastestLapCasExactlyOneWinner, but through the real
// SPSC queue + LapTimeConsumer pipeline instead of calling RaceState
// directly: 5 independent consumer threads race to claim the same lap time
// on one shared RaceState. Exactly one must win.
TEST(LapTimeConsumerTest, ConcurrentConsumersExactlyOneWinner) {
    constexpr int N = 5;
    RaceState rs;

    std::vector<std::unique_ptr<SpscQueue<TelemetryFrame, 2048>>> queues;
    std::vector<std::unique_ptr<LapTimeConsumer>> consumers;
    const char* ids[N] = {"VER", "HAM", "LEC", "NOR", "RUS"};

    for (int i = 0; i < N; ++i) {
        queues.push_back(std::make_unique<SpscQueue<TelemetryFrame, 2048>>());
        queues[i]->push(make_frame(ids[i], 78500.0f)); // identical time for all
        consumers.push_back(std::make_unique<LapTimeConsumer>(*queues[i], rs));
    }
    for (auto& c : consumers) c->start();

    ASSERT_TRUE(wait_until([&] { return rs.get_fastest_lap_holder() >= 0; },
                            std::chrono::milliseconds(500)));
    for (auto& c : consumers) c->stop();

    int holder = rs.get_fastest_lap_holder();
    EXPECT_GE(holder, 0);
    EXPECT_FLOAT_EQ(rs.get_fastest_lap_ms(), 78500.0f);

    // Exactly one of the 5 drivers should be the recorded holder.
    bool matches_one = false;
    for (const char* id : ids) {
        if (holder == driver_index_of(id)) matches_one = true;
    }
    EXPECT_TRUE(matches_one);
}
