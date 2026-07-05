#pragma once

#include <string>
#include <chrono>


// Telemetry
struct TelemetryFrame {
    std::string driver_id {};
    int lap {0};
    int sector {1};  // 1 or 2 or 3
    float speed_kph {0.0f};
    float throttle {0.0f};
    float brake {0.0f};
    float tire_wear {0.0f};
    float fuel_kg {100.0f};  //remaining
    bool drs_active {false};
    bool in_pit {false};
    float gap_to_leader {0.0f};
};

// Profiles
struct DriverProfile {
    std::string id;
    std::string name;
    std::string team;
    std::string color;
    float aggression;   // how hard they push
    float consistency;  // lap to lap variation
    float tire_mgmt;    // how gently they treat tires
    float risk_tolerance;   // likelihood of mistakes
};

struct CarProfile {
    std::string team;
    float engine_power;         // top speed multiplier
    float aero_efficiency;      // cornering performance
    float cooling;              // tire temperature management
    float reliability;           //DNF probability inverse
};

struct TrackProfile {
    std::string name;
    float length_km;
    int sectors {3};
    float tire_deg_factor {1.0f};       // multiplier on tire wear rate
    float fuel_consumption{2.5f};       // kg per km
};

// RACE STATE

enum class PenaltyState {
    NONE,
    PENDING,
    SERVING,
    SERVED
};

enum class WeatherState {
    DRY,
    DAMP,
    WET
};

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

    Type type;
    std::string driver_id;
    int lap {0};
    std::string message {};
    std::chrono::steady_clock::time_point timestamp {std::chrono::steady_clock::now()};
};

struct DriverState {
    DriverProfile profile;
    CarProfile car;
    TelemetryFrame latest_frame;

    // race bookkeeping
    PenaltyState penalty_state {PenaltyState::NONE};
    int penalty_warnings {0};
    int position {0};
    int pit_stops {0};
    float best_lap_ms {0.0f};

    // internal simulation by TelemetryGenerator
    float distance_in_lap {0.0f};
    bool in_pit {false};
    int pit_timer_ticks {0};
    bool has_completed_pit {false};
    std::chrono::steady_clock::time_point lap_start {std::chrono::steady_clock::now()};
};