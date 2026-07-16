#pragma once

#include "common/types.h"
#include <array>
#include <string_view>


// 2025 F1 Driver Profiles
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

// Car Performance Profiles
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

// Default Track
inline const TrackProfile DEFAULT_TRACK = {
    "Circuit de Wall",
    5.4f,   // length_km
    3,      // sectors
    1.0f,   // tire_deg_factor
    2.3f,   // fuel_consumption kg/km
};

// helper to get car profile for a driver
inline const CarProfile& car_for_driver(const DriverProfile&  d) {
    for (const auto& car: CARS) {
        if (car.team == d.team) return car;
    }
    return CARS[9]; // fallback: Sauber (last place)
}

// helper to resolve a driver_id (e.g. "VER") to its index in DRIVERS (0-19).
// Returns -1 if not found.
inline int driver_index_of(std::string_view id) {
    for (int i = 0; i < static_cast<int>(DRIVERS.size()); ++i) {
        if (DRIVERS[i].id == id) return i;
    }
    return -1;
}
