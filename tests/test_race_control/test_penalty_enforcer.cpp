#include <gtest/gtest.h>
#include "race_control/penalty_enforcer.h"
#include <thread>
#include <atomic>
#include <cstring>
#include <string_view>
#include <vector>
#include <chrono>

static void push_track_limits(MpscQueue<RaceControlEvent>& q,
                               const std::string& id, int lap) {
    RaceControlEvent ev;
    ev.type      = RaceControlEvent::Type::TRACK_LIMITS;
    std::memcpy(ev.driver_id, id.data(), 3);
    ev.lap       = lap;
    ev.timestamp = std::chrono::steady_clock::now();
    q.push(std::move(ev));
}

TEST(PenaltyEnforcerTest, ThreeWarningsTriggerPenalty) {
    MpscQueue<RaceControlEvent> q;
    PenaltyEnforcer enforcer{q};

    push_track_limits(q, "VER", 1);
    push_track_limits(q, "VER", 2);
    push_track_limits(q, "VER", 3);
    enforcer.process_events();

    EXPECT_EQ(enforcer.warning_count("VER"), 3);
    EXPECT_EQ(enforcer.penalty_state("VER"), PenaltyState::PENDING);
}

TEST(PenaltyEnforcerTest, TwoWarningsNoPenalty) {
    MpscQueue<RaceControlEvent> q;
    PenaltyEnforcer enforcer{q};

    push_track_limits(q, "LEC", 1);
    push_track_limits(q, "LEC", 2);
    enforcer.process_events();

    EXPECT_EQ(enforcer.penalty_state("LEC"), PenaltyState::NONE);
}

// Concurrent: 4 threads simultaneously push a 3rd warning for VER.
// Exactly one penalty should be issued (CAS guarantee).
TEST(PenaltyEnforcerTest, ConcurrentThirdWarningExactlyOnePenalty) {
    MpscQueue<RaceControlEvent> q;
    PenaltyEnforcer enforcer{q};

    // Prime with 2 warnings first.
    push_track_limits(q, "VER", 1);
    push_track_limits(q, "VER", 2);
    enforcer.process_events();
    EXPECT_EQ(enforcer.penalty_state("VER"), PenaltyState::NONE);

    // 4 threads simultaneously push a 3rd warning.
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] { push_track_limits(q, "VER", 3); });
    }
    for (auto& t : threads) t.join();

    enforcer.process_events();

    // Count PENALTY_ISSUED events — must be exactly 1.
    int penalty_count = 0;
    RaceControlEvent ev;
    while (q.pop(ev)) {
        if (ev.type == RaceControlEvent::Type::PENALTY_ISSUED &&
            std::string_view{ev.driver_id} == "VER") {
            ++penalty_count;
        }
    }
    EXPECT_EQ(penalty_count, 1);
}

TEST(PenaltyEnforcerTest, StateMachineTransitions) {
    MpscQueue<RaceControlEvent> q;
    PenaltyEnforcer enforcer{q};

    for (int i = 1; i <= 3; ++i) push_track_limits(q, "NOR", i);
    enforcer.process_events();
    EXPECT_EQ(enforcer.penalty_state("NOR"), PenaltyState::PENDING);

    enforcer.driver_entered_pits("NOR");
    EXPECT_EQ(enforcer.penalty_state("NOR"), PenaltyState::SERVING);

    enforcer.driver_exited_pits("NOR");
    EXPECT_EQ(enforcer.penalty_state("NOR"), PenaltyState::SERVED);
}