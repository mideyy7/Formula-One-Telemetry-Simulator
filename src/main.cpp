#include "common/season_data.h"
#include "common/leaderboard.h"
#include "common/race_state.h"
#include "concurrency/spsc_queue.h"
#include "concurrency/mpsc_queue.h"
#include "simulation/telemetry_generator.h"
#include "race_control/track_limits.h"
#include "race_control/penalty_enforcer.h"
#include "race_control/weather.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>

// Small helpers to print enums as readable text.
static const char* weather_name(WeatherState w) {
    switch (w) {
        case WeatherState::DRY:  return "DRY";
        case WeatherState::DAMP: return "DAMP";
        case WeatherState::WET:  return "WET";
    }
    return "?";
}

static const char* penalty_name(PenaltyState p) {
    switch (p) {
        case PenaltyState::NONE:    return "-";
        case PenaltyState::PENDING: return "PEN";
        case PenaltyState::SERVING: return "SRV";
        case PenaltyState::SERVED:  return "OK";
    }
    return "?";
}

// std::to_string(float) always prints 6 decimal places, which is wide
// enough to overflow a fixed-width column and bleed into the next one.
// Format to exactly 1 decimal place instead.
static std::string format_gap(float gap_seconds, bool is_leader) {
    if (is_leader) return "-";
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << gap_seconds << "s";
    return out.str();
}

int main() {
    // ── Shared data structures (Phase 2 queues, Phase 5 shared state) ────────
    SpscQueue<TelemetryFrame, 2048> telemetry_queue;
    MpscQueue<RaceControlEvent>     race_control_queue;
    Leaderboard                     leaderboard;
    RaceState                      race_state;

    // ── Systems (Phase 3 producer, Phase 4 race control) ──────────────────────
    TelemetryGenerator generator{telemetry_queue};
    TrackLimitsMonitor track_limits{race_control_queue};
    PenaltyEnforcer     penalty{race_control_queue};
    WeatherSystem       weather{race_control_queue};

    // Every tick (50Hz, on the generator's own background thread): sort the
    // standings by position, publish them to the leaderboard, update the
    // shared lap counter, and run the race-control checks for this tick.
    //
    // TrackLimitsMonitor::check()'s BASE_RATE was calibrated (and tested in
    // Phase 4) assuming a call roughly once per sector crossing (~3x/lap),
    // not once every 20ms tick. The race is ~26 ticks/lap over 3 sectors, so
    // one call every ~9 ticks approximates that cadence. Calling it every
    // tick (as a first draft of this file did) rolls the violation dice
    // ~9x too often, and every driver ends up penalized by the end of the
    // race -- unrealistic, and not a bug in TrackLimitsMonitor itself, just
    // a mismatch between how often it's designed to be called and how
    // often this integration was calling it.
    int tick_counter = 0;
    generator.set_on_tick([&](const std::vector<DriverState>& states) {
        auto sorted = states;
        std::sort(sorted.begin(), sorted.end(),
            [](const DriverState& a, const DriverState& b) {
                return a.position < b.position;
            });
        leaderboard.update(std::move(sorted));
        race_state.set_lap(generator.race_lap());

        int lap = generator.race_lap();
        if (++tick_counter % 9 == 0) {
            track_limits.check(states, lap);
        }
        penalty.process_events();
        weather.update(lap);
    });

    std::cout << "RaceCondition-Z -- live console test (Phases 1-5)\n";
    std::cout << std::string(78, '=') << "\n";

    race_state.start_race();
    generator.start();

    // Main thread: every half second, read the shared state (through the
    // exact same locks/atomics the phase3/4/5 tests exercised) and print a
    // snapshot. This is a genuine concurrent read while the generator thread
    // is actively writing -- not just a single-threaded replay.
    auto race_start_time = std::chrono::steady_clock::now();
    const auto safety_timeout = std::chrono::seconds(60);

    while (!generator.race_finished()) {
        if (std::chrono::steady_clock::now() - race_start_time > safety_timeout) {
            std::cout << "\n[safety timeout hit -- stopping early]\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto standings = leaderboard.snapshot();
        if (standings.empty()) continue; // first tick hasn't landed yet

        std::cout << "\nLap " << race_state.get_lap() << "/" << TOTAL_LAPS
                  << "   Weather: " << weather_name(weather.current())
                  << "   Grip: " << std::fixed << std::setprecision(2)
                  << weather.grip_factor() << "\n";

        std::cout << std::left
                   << std::setw(4)  << "Pos"
                   << std::setw(6)  << "Drv"
                   << std::setw(15) << "Team"
                   << std::setw(9)  << "Speed"
                   << std::setw(7)  << "Tires"
                   << std::setw(7)  << "Fuel"
                   << std::setw(8)  << "Gap"
                   << std::setw(5)  << "DRS"
                   << std::setw(5)  << "Pit"
                   << "Pen\n";

        int rows = std::min<int>(10, static_cast<int>(standings.size()));
        for (int i = 0; i < rows; ++i) {
            const auto& s = standings[i];
            const auto& f = s.latest_frame;

            std::string gap = format_gap(f.gap_to_leader, s.position == 1);

            std::cout << std::left
                       << std::setw(4)  << s.position
                       << std::setw(6)  << s.profile.id
                       << std::setw(15) << s.profile.team
                       << std::setw(9)  << (std::to_string(static_cast<int>(f.speed_kph)) + "kph")
                       << std::setw(7)  << (std::to_string(static_cast<int>(f.tire_wear * 100)) + "%")
                       << std::setw(7)  << (std::to_string(static_cast<int>(f.fuel_kg)) + "kg")
                       << std::setw(8)  << gap
                       << std::setw(5)  << (f.drs_active ? "DRS" : "")
                       << std::setw(5)  << (f.in_pit ? "PIT" : "")
                       << penalty_name(penalty.penalty_state(s.profile.id))
                       << "\n";
        }
    }

    race_state.end_race();
    generator.stop();

    std::cout << "\n" << std::string(78, '=') << "\n";
    std::cout << "RACE FINISHED after " << race_state.get_lap() << " laps\n\n";
    std::cout << "Final standings:\n";
    for (const auto& s : leaderboard.snapshot()) {
        std::cout << "P" << s.position << "  " << s.profile.name
                   << " (" << s.profile.team << ")"
                   << "  pit stops: " << s.pit_stops
                   << "  penalty: " << penalty_name(penalty.penalty_state(s.profile.id))
                   << "\n";
    }

    return 0;
}