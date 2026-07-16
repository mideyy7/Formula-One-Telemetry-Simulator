#include <gtest/gtest.h>
#include "race_control/track_limits.h"
#include "common/season_data.h"

static DriverState make_driver(const std::string& id, float aggression,
                                float tire_wear, float speed) {
    DriverState s;
    for (const auto& d : DRIVERS)
        if (d.id == id) { s.profile = d; break; }
    s.profile.aggression     = aggression;
    s.latest_frame.tire_wear = tire_wear;
    s.latest_frame.speed_kph = speed;
    s.latest_frame.sector    = 2;
    return s;
}

TEST(TrackLimitsTest, HighAggressionMoreViolations) {
    MpscQueue<RaceControlEvent> q;
    TrackLimitsMonitor mon{q};

    std::vector<DriverState> states_high = {
        make_driver("VER", 1.0f, 0.8f, 300.0f)
    };
    std::vector<DriverState> states_low = {
        make_driver("HAM", 0.1f, 0.1f, 200.0f)
    };

    int high_count = 0, low_count = 0;
    for (int i = 0; i < 10'000; ++i) mon.check(states_high, 1);
    RaceControlEvent ev;
    while (q.pop(ev)) ++high_count;

    for (int i = 0; i < 10'000; ++i) mon.check(states_low, 1);
    while (q.pop(ev)) ++low_count;

    EXPECT_GT(high_count, low_count);
    EXPECT_GT(high_count, 0);
}

TEST(TrackLimitsTest, EventHasCorrectFields) {
    MpscQueue<RaceControlEvent> q;
    TrackLimitsMonitor mon{q};

    // Force many checks until at least one event fires.
    std::vector<DriverState> states = {make_driver("VER", 1.0f, 1.0f, 350.0f)};
    for (int i = 0; i < 10'000 && q.empty(); ++i) mon.check(states, 5);

    RaceControlEvent ev;
    if (q.pop(ev)) {
        EXPECT_EQ(ev.type, RaceControlEvent::Type::TRACK_LIMITS);
        EXPECT_STREQ(ev.driver_id, "VER");
        EXPECT_EQ(ev.lap, 5);
        EXPECT_FALSE(ev.message.empty());
    }
}