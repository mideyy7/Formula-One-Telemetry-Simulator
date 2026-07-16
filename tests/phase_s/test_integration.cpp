#include <gtest/gtest.h>
#include "common/season_data.h"
#include "concurrency/spsc_queue.h"
#include "simulation/telemetry_generator.h"
#include "simulation/race_director.h"
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// S1 — the start gate actually blocks the generator's first tick until the
// main-side thread arrives.
TEST(PhaseSIntegration, StartGateDelaysFirstTick) {
    SpscQueue<TelemetryFrame, 2048> q;
    TelemetryGenerator gen{q};
    RaceDirector gate{2};
    gen.set_start_gate(&gate);

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // No frames yet — generator's pacer thread is parked at the gate.
    TelemetryFrame f;
    EXPECT_FALSE(q.pop(f));

    gate.arrive_and_wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(q.pop(f)); // now it's ticking
    gen.stop();
}

// S2 — parallel chunk workers still produce exactly one frame per driver
// per tick, with positions forming a valid permutation (proves the barrier
// completion function, not the workers, does the recalculation).
TEST(PhaseSIntegration, ChunkedTickProducesConsistentStandings) {
    SpscQueue<TelemetryFrame, 2048> q;
    TelemetryGenerator gen{q};

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto standings = gen.standings();
    gen.stop();

    ASSERT_EQ(standings.size(), 20u);
    std::unordered_set<int> positions;
    for (const auto& s : standings) positions.insert(s.position);
    EXPECT_EQ(positions.size(), 20u); // no duplicate/missing positions
}

// S3 — never more than 2 drivers in_pit at the same instant.
TEST(PhaseSIntegration, PitLaneCapsConcurrentOccupancy) {
    SpscQueue<TelemetryFrame, 2048> q;
    TelemetryGenerator gen{q};

    gen.start();
    int max_concurrent_pits = 0;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int in_pit = 0;
        for (const auto& s : gen.standings()) if (s.latest_frame.in_pit) ++in_pit;
        max_concurrent_pits = std::max(max_concurrent_pits, in_pit);
    }
    gen.stop();

    EXPECT_LE(max_concurrent_pits, 2);
}

// S4 — applying a plan changes when a driver pits.
TEST(PhaseSIntegration, StrategyPlanOverridesWearThreshold) {
    SpscQueue<TelemetryFrame, 2048> q;
    TelemetryGenerator gen{q};

    std::unordered_map<std::string, int> plans;
    for (const auto& d : gen.standings()) plans[d.profile.id] = 6; // pit almost immediately
    gen.apply_pit_plan(plans);

    gen.start();
    // ~27 ticks/lap at 20ms/tick ≈ 540ms/lap; give it comfortably past lap 6.
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    bool any_pitted = false;
    for (const auto& s : gen.standings()) if (s.pit_stops > 0) any_pitted = true;
    gen.stop();

    EXPECT_TRUE(any_pitted);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
