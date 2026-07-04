# Phase 1 — Foundation

## Overview
No concurrency yet. This phase sets up the project build system, defines every data type the rest of the project uses, and hardcodes the 2025 F1 season data. Every later phase depends on the types defined here — getting them right now saves refactoring later.

---

## Files Created This Phase

| File | Action | Purpose |
|------|--------|---------|
| `CMakeLists.txt` | CREATE | Root build config — C++20, FetchContent for FTXUI + GoogleTest |
| `src/CMakeLists.txt` | CREATE | Defines `pitwall_lib` header-only library |
| `tests/CMakeLists.txt` | CREATE | Defines per-phase test executables |
| `src/common/types.hpp` | CREATE | All shared data structs and enums |
| `src/common/season_data.hpp` | CREATE | 2025 F1 grid: 20 drivers, 10 cars, 1 track |
| `src/main.cpp` | CREATE | Minimal entry point — prints driver list to verify setup |
| `tests/phase1/test_types.cpp` | CREATE | GoogleTest unit tests for Phase 1 |

---

## Step 1 — CMake root

### File: `CMakeLists.txt`
**Action:** CREATE

```cmake
cmake_minimum_required(VERSION 3.20)
project(pitwall CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)

# FTXUI — terminal UI library
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG        v5.0.0
)
FetchContent_MakeAvailable(ftxui)

# GoogleTest
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(src)
add_subdirectory(tests)
```

> **Verify:** Run `cmake -B build && cmake --build build`. It should download FTXUI and GoogleTest and compile with zero errors.

---

### File: `src/CMakeLists.txt`
**Action:** CREATE

```cmake
# Phase 1: header-only library (no .cpp files yet).
# Phase 3 will upgrade this to STATIC with .cpp source files.
add_library(pitwall_lib INTERFACE)

target_include_directories(pitwall_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pitwall_lib INTERFACE
    ftxui::screen
    ftxui::dom
    ftxui::component
)

# Minimal executable — just verifies the project compiles.
add_executable(pitwall main.cpp)
target_link_libraries(pitwall PRIVATE pitwall_lib)
```

---

### File: `tests/CMakeLists.txt`
**Action:** CREATE

```cmake
enable_testing()

# Helper: creates a test executable for one phase.
# Usage: add_phase_tests(phase1 file1.cpp file2.cpp ...)
function(add_phase_tests phase_name)
    add_executable(${phase_name}_tests ${ARGN})
    target_link_libraries(${phase_name}_tests
        PRIVATE
            pitwall_lib
            GTest::gtest
            GTest::gtest_main
    )
    target_include_directories(${phase_name}_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/src
    )
    add_test(NAME ${phase_name} COMMAND ${phase_name}_tests)
endfunction()

add_phase_tests(phase1 phase1/test_types.cpp)

# Uncomment each phase as you complete it:
# add_phase_tests(phase2
#     phase2/test_spsc_queue.cpp
#     phase2/test_mpsc_queue.cpp
#     phase2/test_thread_pool.cpp
# )
# add_phase_tests(phase3
#     phase3/test_telemetry_generator.cpp
#     phase3/test_latch.cpp
#     phase3/test_barrier.cpp
#     phase3/test_semaphore.cpp
# )
# add_phase_tests(phase4
#     phase4/test_track_limits.cpp
#     phase4/test_penalty_enforcer.cpp
#     phase4/test_weather.cpp
#     phase4/test_strategy_analyzer.cpp
# )
# add_phase_tests(phase5
#     phase5/test_leaderboard.cpp
#     phase5/test_race_state.cpp
# )
# add_phase_tests(phase6 phase6/test_dashboard_integration.cpp)
```

---

## Step 2 — Core types

### File: `src/common/types.hpp`
**Action:** CREATE

```cpp
#pragma once

#include <string>
#include <chrono>

// ─── Telemetry ────────────────────────────────────────────────────────────────

struct TelemetryFrame {
    std::string driver_id   {};
    int         lap         {0};
    int         sector      {1};      // 1, 2, or 3
    float       speed_kph   {0.0f};
    float       throttle    {0.0f};   // 0.0 (none) – 1.0 (full)
    float       brake       {0.0f};   // 0.0 (none) – 1.0 (full lock)
    float       tire_wear   {0.0f};   // 0.0 (new) – 1.0 (destroyed)
    float       fuel_kg     {100.0f}; // kg remaining
    bool        drs_active  {false};
    bool        in_pit      {false};
    float       gap_to_leader{0.0f}; // seconds behind leader
};

// ─── Profiles ─────────────────────────────────────────────────────────────────

struct DriverProfile {
    std::string id;
    std::string name;
    std::string team;
    std::string color;          // ANSI escape code, e.g. "\033[31m"
    float       aggression;     // 0.0 – 1.0: how hard they push
    float       consistency;    // 0.0 – 1.0: lap-to-lap variation
    float       tire_mgmt;      // 0.0 – 1.0: how gently they treat tires
    float       risk_tolerance; // 0.0 – 1.0: likelihood of mistakes
};

struct CarProfile {
    std::string team;
    float       engine_power;     // 0.0 – 1.0: top speed multiplier
    float       aero_efficiency;  // 0.0 – 1.0: cornering performance
    float       cooling;          // 0.0 – 1.0: tire temp management
    float       reliability;      // 0.0 – 1.0: DNF probability inverse
};

struct TrackProfile {
    std::string name;
    float       length_km;
    int         sectors         {3};
    float       tire_deg_factor {1.0f}; // multiplier on tire wear rate
    float       fuel_consumption{2.5f}; // kg per km
};

// ─── Race state ───────────────────────────────────────────────────────────────

enum class PenaltyState { NONE, PENDING, SERVING, SERVED };

enum class WeatherState { DRY, DAMP, WET };

struct RaceControlEvent {
    enum class Type {
        TRACK_LIMITS,
        PENALTY_ISSUED,
        PIT_ENTRY,
        PIT_EXIT,
        WEATHER_CHANGE,
        SAFETY_CAR,
        FASTEST_LAP,
        RADIO_MESSAGE
    };

    Type        type;
    std::string driver_id;
    int         lap     {0};
    std::string message {};
    std::chrono::steady_clock::time_point timestamp{std::chrono::steady_clock::now()};
};

// ─── Combined driver state ────────────────────────────────────────────────────

struct DriverState {
    DriverProfile  profile;
    CarProfile     car;
    TelemetryFrame latest_frame;

    // Race bookkeeping
    PenaltyState penalty_state   {PenaltyState::NONE};
    int          penalty_warnings{0};
    int          position        {0};
    int          pit_stops       {0};
    float        best_lap_ms     {0.0f}; // best lap in real milliseconds

    // Internal simulation (managed by TelemetryGenerator)
    float  distance_in_lap  {0.0f};  // km into current lap
    bool   in_pit           {false};
    int    pit_timer_ticks  {0};     // ticks remaining in pit stop
    bool   has_completed_pit{false}; // has pitted at least once
    std::chrono::steady_clock::time_point lap_start{std::chrono::steady_clock::now()};
};
```

> **Why `best_lap_ms` is real milliseconds, not race time:** The lap timer measures wall-clock time between lap completions. Because the simulation runs at 120× speed, 1 real millisecond ≈ 120 milliseconds of race time. The display layer can multiply by 120 to show a realistic lap time.

---

## Step 3 — Season data

### File: `src/common/season_data.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include <array>

// ─── 2025 F1 Driver profiles ──────────────────────────────────────────────────
// Fields: id, name, team, ansi_color, aggression, consistency, tire_mgmt, risk

inline const std::array<DriverProfile, 20> DRIVERS = {{
    {"VER", "Max Verstappen",    "Red Bull",     "\033[34m",  0.85f, 0.95f, 0.80f, 0.70f},
    {"TSU", "Yuki Tsunoda",      "Red Bull",     "\033[34m",  0.75f, 0.75f, 0.65f, 0.70f},
    {"LEC", "Charles Leclerc",   "Ferrari",      "\033[31m",  0.80f, 0.85f, 0.75f, 0.80f},
    {"HAM", "Lewis Hamilton",    "Ferrari",      "\033[31m",  0.70f, 0.92f, 0.90f, 0.55f},
    {"NOR", "Lando Norris",      "McLaren",      "\033[33m",  0.78f, 0.88f, 0.72f, 0.72f},
    {"PIA", "Oscar Piastri",     "McLaren",      "\033[33m",  0.65f, 0.85f, 0.80f, 0.60f},
    {"RUS", "George Russell",    "Mercedes",     "\033[37m",  0.72f, 0.90f, 0.82f, 0.65f},
    {"ANT", "Kimi Antonelli",    "Mercedes",     "\033[37m",  0.80f, 0.65f, 0.60f, 0.75f},
    {"ALO", "Fernando Alonso",   "Aston Martin", "\033[32m",  0.75f, 0.93f, 0.92f, 0.68f},
    {"STR", "Lance Stroll",      "Aston Martin", "\033[32m",  0.60f, 0.70f, 0.68f, 0.55f},
    {"GAS", "Pierre Gasly",      "Alpine",       "\033[35m",  0.70f, 0.78f, 0.70f, 0.65f},
    {"DOO", "Jack Doohan",       "Alpine",       "\033[35m",  0.72f, 0.62f, 0.58f, 0.70f},
    {"ALB", "Alex Albon",        "Williams",     "\033[36m",  0.68f, 0.82f, 0.78f, 0.62f},
    {"SAI", "Carlos Sainz",      "Williams",     "\033[36m",  0.75f, 0.88f, 0.85f, 0.67f},
    {"HUL", "Nico Hulkenberg",   "Haas",         "\033[91m",  0.72f, 0.80f, 0.72f, 0.65f},
    {"OCO", "Esteban Ocon",      "Haas",         "\033[91m",  0.68f, 0.75f, 0.70f, 0.62f},
    {"LAW", "Liam Lawson",       "RB",           "\033[94m",  0.76f, 0.72f, 0.65f, 0.73f},
    {"HAD", "Isack Hadjar",      "RB",           "\033[94m",  0.74f, 0.65f, 0.60f, 0.70f},
    {"BOT", "Valtteri Bottas",   "Sauber",       "\033[90m",  0.65f, 0.82f, 0.80f, 0.55f},
    {"BEA", "Oliver Bearman",    "Sauber",       "\033[90m",  0.73f, 0.68f, 0.62f, 0.72f},
}};

// ─── Car performance profiles ─────────────────────────────────────────────────
// Fields: team, engine_power, aero_efficiency, cooling, reliability

inline const std::array<CarProfile, 10> CARS = {{
    {"Red Bull",     0.97f, 0.95f, 0.90f, 0.92f},
    {"Ferrari",      0.96f, 0.94f, 0.88f, 0.90f},
    {"McLaren",      0.95f, 0.96f, 0.87f, 0.91f},
    {"Mercedes",     0.93f, 0.92f, 0.89f, 0.93f},
    {"Aston Martin", 0.88f, 0.87f, 0.85f, 0.88f},
    {"Alpine",       0.83f, 0.82f, 0.80f, 0.84f},
    {"Williams",     0.82f, 0.81f, 0.79f, 0.83f},
    {"Haas",         0.80f, 0.79f, 0.78f, 0.81f},
    {"RB",           0.84f, 0.83f, 0.81f, 0.85f},
    {"Sauber",       0.78f, 0.77f, 0.76f, 0.79f},
}};

// ─── Default track (generic F1 circuit) ──────────────────────────────────────

inline const TrackProfile DEFAULT_TRACK = {
    "Circuit de PitWall",
    5.4f,   // length_km
    3,      // sectors
    1.0f,   // tire_deg_factor
    2.3f,   // fuel_consumption kg/km
};

// ─── Helper: get car profile for a driver ─────────────────────────────────────

inline const CarProfile& car_for_driver(const DriverProfile& d) {
    for (const auto& car : CARS) {
        if (car.team == d.team) return car;
    }
    return CARS[9]; // fallback: Sauber (last place)
}
```

---

### File: `src/main.cpp`
**Action:** CREATE (minimal — just verifies the project compiles and season data loads)

```cpp
#include "common/season_data.hpp"
#include <iostream>
#include <iomanip>

int main() {
    std::cout << "PitWall — 2025 F1 Grid\n";
    std::cout << std::string(40, '-') << '\n';

    for (std::size_t i = 0; i < DRIVERS.size(); ++i) {
        const auto& d = DRIVERS[i];
        const auto& c = car_for_driver(d);
        std::cout
            << std::setw(2) << (i + 1) << ". "
            << d.color
            << std::left << std::setw(20) << d.name
            << "\033[0m"
            << "  (" << d.team << ")"
            << "  engine=" << c.engine_power
            << '\n';
    }
    return 0;
}
```

> **Verify:** `./build/pitwall` should print all 20 drivers with their team colors.

---

## Phase 1 Tests

### File: `tests/phase1/test_types.cpp`
**Action:** CREATE

```cpp
#include <gtest/gtest.h>
#include "common/types.hpp"
#include "common/season_data.hpp"
#include <unordered_set>

// ─── TelemetryFrame ───────────────────────────────────────────────────────────

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

// ─── DriverProfile ────────────────────────────────────────────────────────────

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

// ─── Season data ──────────────────────────────────────────────────────────────

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

// ─── DriverState ─────────────────────────────────────────────────────────────

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

// ─── RaceControlEvent ────────────────────────────────────────────────────────

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
```

---

## Building and Running

```bash
# Configure (first time only)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build

# Run Phase 1 tests
./build/tests/phase1_tests

# Or via ctest
ctest --test-dir build -R phase1 --output-on-failure

# Run the executable (verify driver list prints)
./build/src/RaceCondition-z
```

---

## Phase 1 Gate

All 8 tests must pass before starting Phase 2:

```
[ RUN      ] TelemetryFrameTest.DefaultValues
[ RUN      ] DriverProfileTest.TraitsInRange
[ RUN      ] SeasonDataTest.TwentyDrivers
[ RUN      ] SeasonDataTest.TenCars
[ RUN      ] SeasonDataTest.UniqueDriverIds
[ RUN      ] SeasonDataTest.UniqueTeamNames
[ RUN      ] SeasonDataTest.CarProfilesInRange
[ RUN      ] SeasonDataTest.EveryDriverHasACar
[ RUN      ] DriverStateTest.DefaultValues
[ RUN      ] RaceControlEventTest.TimestampIsRecent
[  PASSED  ] 10 tests.
```
