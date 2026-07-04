# Phase 6 — FTXUI Dashboard

## Overview
Wire all shared state into a live terminal dashboard using FTXUI v5. The UI reads the leaderboard via `shared_lock`, drains the MPSC event queue, and pops the SPSC telemetry queue — all from the render thread. A background timer posts a custom event every 100ms to trigger redraws at ~10Hz.

> ⚠️ **PROMPT CLAUDE for rendering refinements:** The code below is complete and compilable. However, FTXUI layouts benefit from tuning once you see them on your terminal. After getting it running, ask: *"The leaderboard panel looks X — how do I adjust the column widths / colors / bar sizes in FTXUI?"*

---

## Files Created This Phase

| File | Action | Purpose |
|------|--------|---------|
| `src/ui/dashboard.hpp` | CREATE | Dashboard class — owns screen + component tree |
| `src/ui/dashboard.cpp` | CREATE | Run loop, refresh timer, shutdown wiring |
| `src/ui/leaderboard_panel.hpp` | CREATE | Left panel — sorted standings |
| `src/ui/telemetry_panel.hpp` | CREATE | Right panel — selected driver data |
| `src/ui/race_control_panel.hpp` | CREATE | Bottom panel — event feed |
| `src/main.cpp` | REWRITE | Full wiring of all components |
| `tests/phase6/test_dashboard_integration.cpp` | CREATE | Integration test — no UI, just data pipeline |

**Also update:** `src/CMakeLists.txt` and `tests/CMakeLists.txt`.

---

## Step 17 — Dashboard skeleton

### File: `src/ui/dashboard.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/leaderboard.hpp"
#include "common/race_state.hpp"
#include "concurrency/mpsc_queue.hpp"
#include "concurrency/spsc_queue.hpp"
#include "common/types.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <vector>
#include <string>
#include <deque>
#include <atomic>

// Dashboard owns the FTXUI screen and all UI components.
// It is created on the main thread and run() blocks until the user quits.

class Dashboard {
public:
    Dashboard(
        Leaderboard&                       leaderboard,
        RaceState&                         race_state,
        MpscQueue<RaceControlEvent>&       event_queue,
        SpscQueue<TelemetryFrame, 2048>&   telemetry_queue
    );

    // Blocks until user presses 'q' or the race ends.
    void run();

    // Called from a background thread to trigger a redraw.
    void request_refresh();

private:
    ftxui::Element render();
    ftxui::Element render_header();

    Leaderboard&                       leaderboard_;
    RaceState&                         race_state_;
    MpscQueue<RaceControlEvent>&       events_;
    SpscQueue<TelemetryFrame, 2048>&   telemetry_;

    ftxui::ScreenInteractive           screen_;
    int                                selected_driver_{0}; // 0-based index into standings

    // Latest telemetry per driver (driver_id → frame)
    std::array<TelemetryFrame, 20>     latest_frames_{};

    // Race control feed — last 15 events
    std::deque<std::string>            event_log_;
    static constexpr std::size_t       MAX_LOG = 15;

    void drain_telemetry();
    void drain_events();
};
```

---

### File: `src/ui/dashboard.cpp`
**Action:** CREATE

```cpp
#include "ui/dashboard.hpp"
#include "ui/leaderboard_panel.hpp"
#include "ui/telemetry_panel.hpp"
#include "ui/race_control_panel.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <thread>
#include <chrono>

using namespace ftxui;

Dashboard::Dashboard(
    Leaderboard&                       leaderboard,
    RaceState&                         race_state,
    MpscQueue<RaceControlEvent>&       event_queue,
    SpscQueue<TelemetryFrame, 2048>&   telemetry_queue)
    : leaderboard_(leaderboard)
    , race_state_(race_state)
    , events_(event_queue)
    , telemetry_(telemetry_queue)
    , screen_(ScreenInteractive::Fullscreen())
{}

void Dashboard::run() {
    // Refresh timer: posts a custom event every 100ms to wake the render loop.
    std::jthread refresh_timer([this](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            screen_.Post(Event::Custom);
        }
    });

    auto renderer = Renderer([this] { return render(); });

    auto with_keys = CatchEvent(renderer, [this](Event ev) -> bool {
        if (ev == Event::Character('q') || ev == Event::Escape) {
            race_state_.end_race();
            screen_.ExitLoopClosure()();
            return true;
        }
        if (ev == Event::ArrowDown) {
            selected_driver_ = std::min(selected_driver_ + 1, 19);
            return true;
        }
        if (ev == Event::ArrowUp) {
            selected_driver_ = std::max(selected_driver_ - 1, 0);
            return true;
        }
        if (ev == Event::Custom) {
            drain_telemetry();
            drain_events();
            return false; // don't consume — let the renderer re-run
        }
        return false;
    });

    screen_.Loop(with_keys);
}

void Dashboard::request_refresh() {
    screen_.Post(Event::Custom);
}

// ─── Render ──────────────────────────────────────────────────────────────────

Element Dashboard::render() {
    auto standings = leaderboard_.snapshot();

    // Clamp selected index to valid range.
    selected_driver_ = std::min(selected_driver_,
                                static_cast<int>(standings.size()) - 1);
    selected_driver_ = std::max(selected_driver_, 0);

    const DriverState* selected = standings.empty() ? nullptr
                                : &standings[selected_driver_];

    TelemetryFrame selected_frame;
    if (selected) {
        // Find the latest frame for the selected driver.
        for (auto& f : latest_frames_) {
            if (f.driver_id == selected->profile.id) {
                selected_frame = f;
                break;
            }
        }
    }

    return vbox({
        render_header(),
        separator(),
        hbox({
            render_leaderboard(standings, selected_driver_) | flex,
            separator(),
            render_telemetry(selected_frame, selected) | flex,
        }) | flex,
        separator(),
        render_race_control(event_log_),
    });
}

Element Dashboard::render_header() {
    int lap = race_state_.get_lap();
    bool sc = race_state_.is_safety_car();

    auto title = text("  PITWALL  ") | bold;
    auto lap_text = text(" LAP " + std::to_string(lap) + "/" +
                         std::to_string(TOTAL_LAPS) + " ");
    auto sc_badge = sc
        ? (text(" SAFETY CAR ") | color(Color::Yellow) | bold)
        : text("");

    return hbox({
        title  | color(Color::Red),
        separator(),
        lap_text | color(Color::White),
        sc_badge,
        filler(),
        text(" [↑↓] select driver   [q] quit ") | color(Color::GrayDark),
    }) | bgcolor(Color::Black);
}

// ─── Data draining ────────────────────────────────────────────────────────────

void Dashboard::drain_telemetry() {
    TelemetryFrame frame;
    int drained = 0;
    while (telemetry_.pop(frame) && drained < 100) {
        // Update the slot for this driver.
        for (auto& f : latest_frames_) {
            if (f.driver_id.empty() || f.driver_id == frame.driver_id) {
                f = frame;
                break;
            }
        }
        ++drained;
    }
}

void Dashboard::drain_events() {
    RaceControlEvent ev;
    int drained = 0;
    while (events_.pop(ev) && drained < 5) {
        static const std::unordered_map<RaceControlEvent::Type, std::string> icons = {
            {RaceControlEvent::Type::TRACK_LIMITS,   "[TL] "},
            {RaceControlEvent::Type::PENALTY_ISSUED, "[PEN]"},
            {RaceControlEvent::Type::PIT_ENTRY,      "[PIT]"},
            {RaceControlEvent::Type::PIT_EXIT,       "[OUT]"},
            {RaceControlEvent::Type::WEATHER_CHANGE, "[WX] "},
            {RaceControlEvent::Type::SAFETY_CAR,     "[SC] "},
            {RaceControlEvent::Type::FASTEST_LAP,    "[FL] "},
            {RaceControlEvent::Type::RADIO_MESSAGE,  "[RAD]"},
        };
        auto it = icons.find(ev.type);
        std::string prefix = (it != icons.end()) ? it->second : "[???]";
        event_log_.push_front(prefix + " " + ev.message);
        if (event_log_.size() > MAX_LOG) event_log_.pop_back();
        ++drained;
    }
}
```

---

## Step 18 — Leaderboard panel

### File: `src/ui/leaderboard_panel.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include <ftxui/dom/elements.hpp>
#include <vector>
#include <string>

using namespace ftxui;

// Returns a colored tire wear bar: green → yellow → red.
inline Element tire_bar(float wear) {
    Color c = (wear < 0.40f) ? Color::Green
            : (wear < 0.70f) ? Color::Yellow
            :                  Color::Red;
    return hbox({
        gauge(1.0f - wear) | color(c),
        text(std::to_string(static_cast<int>((1.0f - wear) * 100)) + "%")
              | color(c) | size(WIDTH, EQUAL, 4),
    });
}

// Returns a colored speed text.
inline Element speed_color(float kph) {
    Color c = (kph > 280.0f) ? Color::Green
            : (kph > 180.0f) ? Color::Yellow
            :                  Color::Red;
    return text(std::to_string(static_cast<int>(kph)) + "kph") | color(c);
}

// Renders the full leaderboard panel.
inline Element render_leaderboard(
    const std::vector<DriverState>& standings,
    int selected_idx)
{
    if (standings.empty()) {
        return window(text("LEADERBOARD"), text(" Waiting..."));
    }

    Elements rows;
    rows.push_back(
        hbox({
            text("Pos") | size(WIDTH, EQUAL, 4) | bold,
            text("Driver")  | size(WIDTH, EQUAL, 6) | bold,
            text("Team")    | size(WIDTH, EQUAL, 13)| bold,
            text("Lap")     | size(WIDTH, EQUAL, 5) | bold,
            text("Gap")     | size(WIDTH, EQUAL, 9) | bold,
            text("Tire")    | size(WIDTH, EQUAL, 12)| bold,
            text("Spd")     | size(WIDTH, EQUAL, 8) | bold,
            text("Status")  | flex | bold,
        }) | color(Color::GrayDark)
    );
    rows.push_back(separator());

    for (int i = 0; i < static_cast<int>(standings.size()); ++i) {
        const auto& s = standings[i];
        const auto& f = s.latest_frame;

        // Position badge color
        Color pos_color = (s.position == 1) ? Color::Gold1
                        : (s.position == 2) ? Color::GrayLight
                        : (s.position == 3) ? Color::Orange1
                        :                     Color::White;

        // Gap display
        std::string gap_str = (s.position == 1) ? "LEADER   "
                            : "+" + [&] {
                                char buf[16];
                                std::snprintf(buf, sizeof(buf), "%.3fs", f.gap_to_leader);
                                return std::string(buf);
                              }();

        // Status badges
        std::string status;
        if (f.in_pit)  status += "[PIT] ";
        if (f.drs_active) status += "DRS ";
        if (s.penalty_state == PenaltyState::PENDING) status += "[!PEN]";
        if (s.penalty_state == PenaltyState::SERVING) status += "[PEN]";

        Element row = hbox({
            text(std::to_string(s.position)) | size(WIDTH, EQUAL, 4) | color(pos_color) | bold,
            text(s.profile.id)               | size(WIDTH, EQUAL, 6),
            text(s.profile.team)             | size(WIDTH, EQUAL, 13)| color(Color::GrayDark),
            text(std::to_string(f.lap))      | size(WIDTH, EQUAL, 5),
            text(gap_str)                    | size(WIDTH, EQUAL, 9),
            tire_bar(f.tire_wear)            | size(WIDTH, EQUAL, 12),
            speed_color(f.speed_kph)         | size(WIDTH, EQUAL, 8),
            text(status)                     | flex,
        });

        // Highlight selected driver
        if (i == selected_idx) {
            row = row | bgcolor(Color::Blue) | bold;
        }

        rows.push_back(row);
    }

    return window(
        text(" LEADERBOARD ") | bold,
        vbox(rows)
    );
}
```

---

## Step 19 — Telemetry panel

### File: `src/ui/telemetry_panel.hpp`
**Action:** CREATE

```cpp
#pragma once

#include "common/types.hpp"
#include <ftxui/dom/elements.hpp>
#include <string>
#include <cstdio>

using namespace ftxui;

// Renders a labeled gauge bar with percentage or value text.
inline Element labeled_gauge(const std::string& label,
                              float value_0_to_1,
                              Color bar_color,
                              const std::string& value_text) {
    return hbox({
        text(label) | size(WIDTH, EQUAL, 10) | color(Color::GrayDark),
        gauge(value_0_to_1) | color(bar_color) | flex,
        text(" " + value_text) | size(WIDTH, EQUAL, 9),
    });
}

inline Element render_telemetry(
    const TelemetryFrame& frame,
    const DriverState* state)
{
    if (!state || frame.driver_id.empty()) {
        return window(text(" TELEMETRY "), text(" No driver selected "));
    }

    const auto& p = state->profile;

    // Format lap time
    std::string lap_time_str = "---";
    if (state->best_lap_ms > 0.0f) {
        // Multiply by TIME_SCALE to get simulated race time in ms
        float race_ms = state->best_lap_ms * 120.0f;
        int   mins    = static_cast<int>(race_ms / 60000);
        float secs    = (race_ms - mins * 60000.0f) / 1000.0f;
        char  buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%06.3f", mins, secs);
        lap_time_str = buf;
    }

    // Speed: 0-370 kph scale
    float speed_norm = frame.speed_kph / 370.0f;
    Color speed_color = (frame.speed_kph > 280.0f) ? Color::Green
                      : (frame.speed_kph > 150.0f) ? Color::Yellow
                      :                              Color::Red;

    // Tire wear bar: INVERTED — full = new tire, empty = worn
    Color tire_color  = (frame.tire_wear < 0.40f) ? Color::Green
                      : (frame.tire_wear < 0.70f) ? Color::Yellow
                      :                             Color::Red;

    // Fuel bar
    Color fuel_color  = (frame.fuel_kg > 50.0f)  ? Color::Green
                      : (frame.fuel_kg > 20.0f)  ? Color::Yellow
                      :                            Color::Red;

    // DRS indicator
    Element drs = frame.drs_active
        ? (text(" DRS ACTIVE ") | color(Color::Green) | bold)
        : (text(" DRS OFF    ") | color(Color::GrayDark));

    // Safety car (read from global — OK for display)
    char speed_buf[16];
    std::snprintf(speed_buf, sizeof(speed_buf), "%.0f kph", frame.speed_kph);

    char fuel_buf[16];
    std::snprintf(fuel_buf, sizeof(fuel_buf), "%.1f kg", frame.fuel_kg);

    char tire_buf[8];
    std::snprintf(tire_buf, sizeof(tire_buf), "%d%%", static_cast<int>((1.0f - frame.tire_wear) * 100));

    char throttle_buf[6], brake_buf[6];
    std::snprintf(throttle_buf, sizeof(throttle_buf), "%d%%", static_cast<int>(frame.throttle * 100));
    std::snprintf(brake_buf,    sizeof(brake_buf),    "%d%%", static_cast<int>(frame.brake * 100));

    return window(
        hbox({text(" "), text(p.id) | bold | color(Color::Red),
              text(" — "), text(p.team) | color(Color::GrayDark), text(" ")}),
        vbox({
            text(""),
            labeled_gauge("Speed",    speed_norm,               speed_color, speed_buf),
            labeled_gauge("Throttle", frame.throttle,           Color::Green, throttle_buf),
            labeled_gauge("Brake",    frame.brake,              Color::Red,   brake_buf),
            labeled_gauge("Tire",     1.0f - frame.tire_wear,   tire_color,   tire_buf),
            labeled_gauge("Fuel",     frame.fuel_kg / 100.0f,   fuel_color,   fuel_buf),
            separator(),
            hbox({
                text("LAP ") | color(Color::GrayDark),
                text(std::to_string(frame.lap)) | bold,
                text("  S") | color(Color::GrayDark),
                text(std::to_string(frame.sector)) | bold,
                filler(),
                drs,
            }),
            hbox({
                text("Best Lap: ") | color(Color::GrayDark),
                text(lap_time_str) | bold | color(Color::Magenta),
            }),
            hbox({
                text("Pit stops: ") | color(Color::GrayDark),
                text(std::to_string(state->pit_stops)) | bold,
            }),
        })
    );
}
```

---

## Step 20 — Race control panel

### File: `src/ui/race_control_panel.hpp`
**Action:** CREATE

```cpp
#pragma once

#include <ftxui/dom/elements.hpp>
#include <deque>
#include <string>

using namespace ftxui;

inline Element render_race_control(const std::deque<std::string>& log) {
    Elements lines;
    for (const auto& entry : log) {
        Color c = Color::White;
        if (entry.find("[PEN]") != std::string::npos) c = Color::Red;
        else if (entry.find("[FL]")  != std::string::npos) c = Color::Magenta;
        else if (entry.find("[SC]")  != std::string::npos) c = Color::Yellow;
        else if (entry.find("[WX]")  != std::string::npos) c = Color::Cyan;
        else if (entry.find("[TL]")  != std::string::npos) c = Color::Orange1;

        lines.push_back(text(entry) | color(c));
    }
    if (lines.empty()) lines.push_back(text(" No events yet ") | color(Color::GrayDark));

    return window(
        text(" RACE CONTROL ") | bold,
        hbox(lines | flex) | size(HEIGHT, EQUAL, 4)
    );
}
```

---

## Step 21 — Full `src/main.cpp` (REWRITE)

### File: `src/main.cpp`
**Action:** REWRITE — replace the minimal Phase 1 version.

```cpp
#include "common/types.hpp"
#include "common/season_data.hpp"
#include "common/leaderboard.hpp"
#include "common/race_state.hpp"
#include "concurrency/spsc_queue.hpp"
#include "concurrency/mpsc_queue.hpp"
#include "concurrency/thread_pool.hpp"
#include "simulation/telemetry_generator.hpp"
#include "simulation/race_director.hpp"
#include "race_control/track_limits.hpp"
#include "race_control/penalty_enforcer.hpp"
#include "race_control/weather.hpp"
#include "strategy/strategy_analyzer.hpp"
#include "ui/dashboard.hpp"
#include "simulation/telemetry_generator.hpp"

#include <iostream>
#include <thread>
#include <chrono>

// Global race state (declared extern in race_state.hpp)
RaceState g_race_state;

int main() {
    // ── Shared data structures ────────────────────────────────────────────────
    SpscQueue<TelemetryFrame, 2048> telemetry_queue;
    MpscQueue<RaceControlEvent>     event_queue;
    Leaderboard                     leaderboard;
    ThreadPool                      pool{4};

    // ── Systems ───────────────────────────────────────────────────────────────
    TelemetryGenerator  generator{telemetry_queue};
    TrackLimitsMonitor  track_limits{event_queue};
    PenaltyEnforcer     penalty{event_queue};
    WeatherSystem       weather{event_queue};
    StrategyAnalyzer    strategy{pool};

    // ── Race start latch (3 participants: generator, race_control, UI) ────────
    RaceDirector director{3};

    // ── Wire generator → leaderboard update ───────────────────────────────────
    generator.set_on_tick([&](const std::vector<DriverState>& states) {
        // Sort by position and push to leaderboard.
        auto sorted = states;
        std::sort(sorted.begin(), sorted.end(),
            [](const DriverState& a, const DriverState& b) {
                return a.position < b.position;
            });
        leaderboard.update(sorted);
        g_race_state.set_lap(generator.race_lap());

        // Trigger track limits check via thread pool.
        pool.submit([&, states, lap = generator.race_lap()] {
            track_limits.check(states, lap);
            penalty.process_events();
            weather.update(lap);
        });
    });

    // ── Race control thread (drains events, processes penalties) ──────────────
    std::jthread race_control_thread([&](std::stop_token st) {
        director.arrive_and_wait(); // wait for all systems ready
        while (!st.stop_requested() && g_race_state.is_active()) {
            penalty.process_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    // ── Start simulation ──────────────────────────────────────────────────────
    g_race_state.start_race();
    generator.start();

    director.arrive_and_wait(); // generator ready

    // ── Launch dashboard (blocks until user quits or race ends) ───────────────
    Dashboard dashboard{leaderboard, g_race_state, event_queue, telemetry_queue};
    director.arrive_and_wait(); // UI ready — all 3 arrived, race begins

    dashboard.run();

    // ── Shutdown ──────────────────────────────────────────────────────────────
    g_race_state.end_race();
    generator.stop();
    // race_control_thread stops automatically via stop_token on destruction

    std::cout << "\nRace finished. Final standings:\n";
    for (const auto& s : leaderboard.snapshot()) {
        std::cout << "P" << s.position << "  " << s.profile.name
                  << "  (laps: " << s.latest_frame.lap << ")\n";
    }

    return 0;
}
```

---

## Update `src/CMakeLists.txt`
**Action:** EDIT — add dashboard.cpp.

```cmake
add_library(pitwall_lib STATIC
    concurrency/thread_pool.cpp
    simulation/telemetry_generator.cpp
    race_control/track_limits.cpp
    race_control/penalty_enforcer.cpp
    race_control/weather.cpp
    strategy/strategy_analyzer.cpp
    ui/dashboard.cpp
)

target_include_directories(pitwall_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pitwall_lib PUBLIC
    ftxui::screen
    ftxui::dom
    ftxui::component
)

add_executable(pitwall main.cpp)
target_link_libraries(pitwall PRIVATE pitwall_lib)
```

---

## Phase 6 Tests

### File: `tests/phase6/test_dashboard_integration.cpp`
**Action:** CREATE

> This test runs the full data pipeline (generator + race control) but does NOT launch the FTXUI screen. It verifies that the queues are populated and drained correctly.

```cpp
#include <gtest/gtest.h>
#include "common/leaderboard.hpp"
#include "common/race_state.hpp"
#include "concurrency/spsc_queue.hpp"
#include "concurrency/mpsc_queue.hpp"
#include "simulation/telemetry_generator.hpp"
#include "race_control/track_limits.hpp"
#include "race_control/penalty_enforcer.hpp"
#include <thread>
#include <chrono>
#include <unordered_set>

RaceState g_race_state; // definition for tests

TEST(IntegrationTest, PipelinePopulatesQueues) {
    SpscQueue<TelemetryFrame, 2048> tq;
    MpscQueue<RaceControlEvent>     eq;
    Leaderboard                     lb;

    TelemetryGenerator gen{tq};
    gen.set_on_tick([&](const std::vector<DriverState>& states) {
        auto sorted = states;
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b){ return a.position < b.position; });
        lb.update(sorted);
    });

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // ~25 ticks
    gen.stop();

    // Leaderboard should have 20 drivers
    EXPECT_EQ(lb.size(), 20u);

    // SPSC queue should have been written to
    EXPECT_GT(tq.size_approx(), 0u);
}

TEST(IntegrationTest, LeaderboardPositionsAreUnique) {
    SpscQueue<TelemetryFrame, 2048> tq;
    Leaderboard lb;

    TelemetryGenerator gen{tq};
    gen.set_on_tick([&](const std::vector<DriverState>& states) {
        auto s = states;
        std::sort(s.begin(), s.end(),
            [](const auto& a, const auto& b){ return a.position < b.position; });
        lb.update(s);
    });

    gen.start();
    // Run for at least 1 lap (~26 ticks * 20ms = ~520ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    gen.stop();

    auto snap = lb.snapshot();
    ASSERT_EQ(snap.size(), 20u);

    std::unordered_set<int> positions;
    for (const auto& s : snap) {
        EXPECT_TRUE(positions.insert(s.position).second)
            << "Duplicate position: " << s.position;
    }
    EXPECT_EQ(positions.size(), 20u);
}

TEST(IntegrationTest, AllDriversProduceFrames) {
    SpscQueue<TelemetryFrame, 4096> tq;
    TelemetryGenerator gen{tq};

    gen.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    gen.stop();

    std::unordered_set<std::string> driver_ids;
    TelemetryFrame f;
    while (tq.pop(f)) driver_ids.insert(f.driver_id);

    EXPECT_EQ(driver_ids.size(), 20u);
}

TEST(IntegrationTest, ShutdownClean) {
    SpscQueue<TelemetryFrame, 2048> tq;
    TelemetryGenerator gen{tq};
    gen.start();

    auto t0 = std::chrono::steady_clock::now();
    gen.stop();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // Must stop within 100ms
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        100LL
    );
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

## Update `tests/CMakeLists.txt`
**Action:** EDIT — uncomment the phase6 block.

```cmake
add_phase_tests(phase6 phase6/test_dashboard_integration.cpp)
```

---

## Building and Running

```bash
cmake --build build

# Run integration tests (no terminal UI)
ctest --test-dir build -R phase6 --output-on-failure

# Run the actual application (opens FTXUI dashboard)
./build/pitwall

# ASan — catches memory leaks and use-after-free
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -g"
cmake --build build-asan
./build-asan/pitwall
```

---

## Phase 6 Gate

All integration tests pass, ASan clean, and the live dashboard:
- Shows 20 drivers updating in real time
- Arrow keys change the selected driver in the telemetry panel
- Race control feed shows events as they occur
- `q` exits cleanly (no hang, no crash)

```
[ RUN      ] IntegrationTest.PipelinePopulatesQueues
[ RUN      ] IntegrationTest.LeaderboardPositionsAreUnique
[ RUN      ] IntegrationTest.AllDriversProduceFrames
[ RUN      ] IntegrationTest.ShutdownClean
[  PASSED  ] 4 tests.
```

> ⚠️ **PROMPT CLAUDE if the dashboard doesn't look right:**
> - Layout issues: *"The leaderboard columns are misaligned — here's what I see: [paste screenshot]. How do I fix the column widths in FTXUI?"*
> - Color issues: *"The team colors are all showing as white — how do I apply per-team colors from the DriverProfile.color string in FTXUI?"*
> - Refresh issues: *"The dashboard isn't updating in real time. Here's the render loop: [paste code]. What's wrong?"*
