#include <gtest/gtest.h>
#include "common/types.h"
#include "common/season_data.h"
#include <unordered_set>

// Telemetry Frame 
TEST(TelemetryFrameTest, DefaultValues) {
    TelemetryFrame f;
    EXPECT_EQ(f.lap,         0);
    EXPECT_EQ(f.sector,      1);
    EXPECT_FLOAT_EQ(f.speed_kph,    0.0f);
    EXPECT_FLOAT_EQ(f.throttle,     0.0f);
    EXPECT_FLOAT_EQ(f.brake,        0.0f);
    EXPECT_FLOAT_EQ(f.tire_wear,    0.0f);
    EXPECT_FLOAT_EQ(f.fuel_kg,      100.0f);
    EXPECT_FALSE(f.drs_active);
    EXPECT_FALSE(f.in_pit);
    EXPECT_FLOAT_EQ(f.gap_to_leader, 0.0f);
}

// Driver Profile
TEST(DriverProfileTest, TraitsInRange) {
    for (const auto& d : DRIVERS) {
        EXPECT_GE(d.aggression,     0.0f) << d.name;
        EXPECT_LE(d.aggression,     1.0f) << d.name;
        EXPECT_GE(d.consistency,    0.0f) << d.name;
        EXPECT_LE(d.consistency,    1.0f) << d.name;
        EXPECT_GE(d.tire_mgmt,      0.0f) << d.name;
        EXPECT_LE(d.tire_mgmt,      1.0f) << d.name;
        EXPECT_GE(d.risk_tolerance, 0.0f) << d.name;
        EXPECT_LE(d.risk_tolerance, 1.0f) << d.name;
    }
}

// Season Data
TEST(SeasonDataTest, TwentyDrivers) {
    EXPECT_EQ(DRIVERS.size(), 20u);
}

TEST(SeasonDataTest, TenCars) {
    EXPECT_EQ(CARS.size(), 10u);
}

TEST(SeasonDataTest, UniqueDriverIds) {
    std::unordered_set<std::string> ids;
    for (const auto& d : DRIVERS) {
        EXPECT_TRUE(ids.insert(d.id).second)
            << "Duplicate driver ID: " << d.id;
    }
}

TEST(SeasonDataTest, UniqueTeamNames) {
    std::unordered_set<std::string> teams;
    for (const auto& c : CARS) {
        EXPECT_TRUE(teams.insert(c.team).second)
            << "Duplicate team: " << c.team;
    }
}

TEST(SeasonDataTest, CarProfilesInRange) {
    for (const auto& c : CARS) {
        EXPECT_GE(c.engine_power,    0.0f) << c.team;
        EXPECT_LE(c.engine_power,    1.0f) << c.team;
        EXPECT_GE(c.aero_efficiency, 0.0f) << c.team;
        EXPECT_LE(c.aero_efficiency, 1.0f) << c.team;
        EXPECT_GE(c.reliability,     0.0f) << c.team;
        EXPECT_LE(c.reliability,     1.0f) << c.team;
    }
}

TEST(SeasonDataTest, EveryDriverHasACar) {
    for (const auto& d : DRIVERS) {
        bool found = false;
        for (const auto& c : CARS) {
            if (c.team == d.team) { found = true; break; }
        }
        EXPECT_TRUE(found) << "No car found for driver: " << d.name;
    }
}

// Driver State 
TEST(DriverStateTest, DefaultValues) {
    DriverState s;
    EXPECT_EQ(s.penalty_state,    PenaltyState::NONE);
    EXPECT_EQ(s.penalty_warnings, 0);
    EXPECT_EQ(s.position,         0);
    EXPECT_EQ(s.pit_stops,        0);
    EXPECT_FLOAT_EQ(s.best_lap_ms, 0.0f);
    EXPECT_FALSE(s.in_pit);
    EXPECT_FLOAT_EQ(s.distance_in_lap, 0.0f);
} 

// Race Control Events 
TEST(RaceControlEventTest, TimestampIsRecent) {
    auto before = std::chrono::steady_clock::now();
    RaceControlEvent ev{RaceControlEvent::Type::SAFETY_CAR, "VER", 5};
    auto after = std::chrono::steady_clock::now();
    EXPECT_GE(ev.timestamp, before);
    EXPECT_LE(ev.timestamp, after);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

