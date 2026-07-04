# Phase 7 — Features and Polish

## Overview
Add the final F1 features — each one motivated by a concurrency concept you've already learned. Then add sanitizer build presets and write the README section that doubles as your interview cheat sheet.

---

## Files Modified This Phase

| File | Action | What changes |
|------|--------|-------------|
| `src/common/race_state.hpp` | EDIT | Safety car flag already there — add `deploy_safety_car()` logic |
| `src/simulation/telemetry_generator.cpp` | EDIT | React to safety car, push radio messages |
| `src/common/types.hpp` | EDIT | Add `RADIO_MESSAGE` handling |
| `src/ui/race_control_panel.hpp` | EDIT | Show radio messages in feed |
| `src/main.cpp` | EDIT | Race end screen |
| `CMakePresets.json` | CREATE | TSan and ASan build presets |

---

## Step 22 — Safety car (atomic broadcast)

**Concept:** One writer sets a flag with `memory_order_release`. All 20 driver threads read it with `memory_order_acquire`. No lock needed.

The flag is already in `RaceState` from Phase 5. You need to:

### Edit: `src/simulation/telemetry_generator.cpp`
**File to edit:** `telemetry_generator.cpp`  
**Where:** Inside `tick()`, at the start of the per-driver loop, add this block after the pit check:

```cpp
// ── Safety car speed cap ──────────────────────────────────────────────────
// Reads g_race_state.safety_car with acquire — sees the release store from
// wherever the safety car was deployed (race director or track incident).
if (g_race_state.is_safety_car()) {
    frame.speed_kph  = std::min(frame.speed_kph, 80.0f);
    frame.drs_active = false; // DRS not allowed under safety car
}
```

Add the include at the top of the file:
```cpp
#include "common/race_state.hpp"
```

### Where safety car is deployed
Add a random deployment check at the end of `tick()`, after `update_positions_and_drs()`:

```cpp
// ── Random safety car deployment ──────────────────────────────────────────
// 0.1% chance per tick (~every 2000 ticks = ~40 seconds real time)
static std::uniform_real_distribution<float> incident_dist{0.0f, 1.0f};
if (!g_race_state.is_safety_car() && incident_dist(rng_) < 0.001f) {
    g_race_state.deploy_safety_car();

    RaceControlEvent ev;
    ev.type    = RaceControlEvent::Type::SAFETY_CAR;
    ev.lap     = race_lap_;
    ev.message = "SAFETY CAR DEPLOYED";
    ev.timestamp = std::chrono::steady_clock::now();
    // Push to event queue (captured in constructor via reference)
    // ⚠️ PROMPT CLAUDE: "How do I pass the event queue reference into
    //    TelemetryGenerator so it can push safety car events?"
    // For now: print to stderr as a placeholder.
    // TODO: pass MpscQueue<RaceControlEvent>& into TelemetryGenerator constructor
}

// Auto-retract after 5 laps
static int sc_deploy_lap = -1;
if (g_race_state.is_safety_car()) {
    if (sc_deploy_lap < 0) sc_deploy_lap = race_lap_;
    if (race_lap_ - sc_deploy_lap >= 5) {
        g_race_state.retract_safety_car();
        sc_deploy_lap = -1;
    }
}
```

> ⚠️ **PROMPT CLAUDE:** *"I want TelemetryGenerator to push safety car events into the MpscQueue. Currently it only holds a reference to the SpscQueue. How do I add the MpscQueue reference to TelemetryGenerator's constructor without breaking Phase 3 tests?"*

---

## Step 23 — Radio messages

**Concept:** Second MPSC queue — reuses the same pattern from Phase 2.

### Edit: `src/common/types.hpp`
Add radio trigger conditions. No code change needed — `RADIO_MESSAGE` type already exists in `RaceControlEvent::Type`.

### Edit: `src/simulation/telemetry_generator.cpp`
Add radio message generation at lap completion (inside the lap completion block in `tick()`):

```cpp
// ── Radio messages at key moments ─────────────────────────────────────────
// This block goes inside the "if (state.distance_in_lap >= track_.length_km)"
// block, right before the pit decision.

static const std::vector<std::string> push_messages = {
    "Box, box, box",
    "Push, push, push!",
    "Tires are gone, need to box",
    "Copy that, understood",
    "Gap is closing, keep pushing",
    "DRS available next lap",
};

if (frame.tire_wear > 0.70f && !state.has_completed_pit) {
    // Distressed driver radio
    RaceControlEvent radio;
    radio.type      = RaceControlEvent::Type::RADIO_MESSAGE;
    radio.driver_id = state.profile.id;
    radio.lap       = frame.lap;
    radio.message   = state.profile.id + ": \"" + push_messages[frame.lap % push_messages.size()] + "\"";
    radio.timestamp = std::chrono::steady_clock::now();
    // ⚠️ PROMPT CLAUDE: "Pass the event queue into TelemetryGenerator and
    //    push radio messages here."
}
```

### Edit: `src/ui/race_control_panel.hpp`
Radio messages already handled — the `[RAD]` prefix in `drain_events()` in `dashboard.cpp` color-codes them. No change needed.

---

## Step 24 — Race end and results screen

### Edit: `src/main.cpp`
Replace the final print block with a results table. Find this section at the bottom:

```cpp
// ── Shutdown ──────────────────────────────────────────────────────────────
g_race_state.end_race();
generator.stop();
```

And add after the final print:

```cpp
std::cout << "\n";
std::cout << "┌─────────────────────────────────────────────────────┐\n";
std::cout << "│              RACE RESULT — PITWALL GP               │\n";
std::cout << "├────┬──────────────────────┬──────┬──────┬───────────┤\n";
std::cout << "│ P  │ Driver               │ Laps │ Pits │ Best Lap  │\n";
std::cout << "├────┼──────────────────────┼──────┼──────┼───────────┤\n";

auto final_standings = leaderboard.snapshot();
// Sort by position
std::sort(final_standings.begin(), final_standings.end(),
    [](const auto& a, const auto& b){ return a.position < b.position; });

for (const auto& s : final_standings) {
    // Fastest lap holder
    int fl_idx = g_race_state.get_fastest_lap_holder();
    bool is_fl = (fl_idx >= 0 &&
                  DRIVERS[fl_idx].id == s.profile.id);

    // Format best lap
    std::string lap_str = "---";
    if (s.best_lap_ms > 0.0f) {
        float ms   = s.best_lap_ms * 120.0f;
        int   mins = static_cast<int>(ms / 60000);
        float secs = (ms - mins * 60000.0f) / 1000.0f;
        char  buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%06.3f", mins, secs);
        lap_str = buf;
        if (is_fl) lap_str += " \033[35mFL\033[0m";
    }

    char row[128];
    std::snprintf(row, sizeof(row),
        "│ %-2d │ %-20s │ %-4d │ %-4d │ %-9s │\n",
        s.position,
        s.profile.name.substr(0, 20).c_str(),
        s.latest_frame.lap,
        s.pit_stops,
        lap_str.c_str()
    );
    std::cout << row;
}
std::cout << "└────┴──────────────────────┴──────┴──────┴───────────┘\n";
```

---

## Step 25 — CMakePresets.json

### File: `CMakePresets.json`
**Action:** CREATE at the project root.

```json
{
    "version": 3,
    "configurePresets": [
        {
            "name": "debug",
            "displayName": "Debug",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug"
            }
        },
        {
            "name": "tsan",
            "displayName": "ThreadSanitizer",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build-tsan",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_FLAGS": "-fsanitize=thread -g -O1"
            }
        },
        {
            "name": "asan",
            "displayName": "AddressSanitizer",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build-asan",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_FLAGS": "-fsanitize=address -fno-omit-frame-pointer -g"
            }
        },
        {
            "name": "release",
            "displayName": "Release",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build-release",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Release",
                "CMAKE_CXX_FLAGS": "-O3 -march=native"
            }
        }
    ],
    "buildPresets": [
        {"name": "debug",   "configurePreset": "debug"},
        {"name": "tsan",    "configurePreset": "tsan"},
        {"name": "asan",    "configurePreset": "asan"},
        {"name": "release", "configurePreset": "release"}
    ]
}
```

**How to use:**
```bash
# Build with ThreadSanitizer
cmake --preset tsan
cmake --build --preset tsan
./build-tsan/tests/phase2_tests

# Build with AddressSanitizer
cmake --preset asan
cmake --build --preset asan
./build-asan/pitwall

# Build release (fastest)
cmake --preset release
cmake --build --preset release
./build-release/pitwall
```

---

## Phase 7 Tests

No new test files. Instead, run the full system:

```bash
# 1. Full test suite — all phases
ctest --test-dir build --output-on-failure

# 2. TSan clean — zero data races
cmake --preset tsan && cmake --build --preset tsan
ctest --test-dir build-tsan --output-on-failure

# 3. ASan clean — zero memory errors
cmake --preset asan && cmake --build --preset asan
./build-asan/pitwall
# Race for 10 seconds, then press q — must exit cleanly with no ASan output

# 4. Run live demo
./build/pitwall
```

---

## Outstanding TODOs (prompt Claude explicitly for these)

The following items require a separate focused prompt. Each is marked where it appears in the code with `⚠️ PROMPT CLAUDE`.

| # | What to ask | Where to add it |
|---|-------------|-----------------|
| 1 | *"How do I pass `MpscQueue<RaceControlEvent>&` into `TelemetryGenerator` so it can push safety car and radio events?"* | `telemetry_generator.hpp` constructor, `telemetry_generator.cpp` |
| 2 | *"The FTXUI leaderboard columns are misaligned on my terminal — column widths need adjusting. Here's what I see: [screenshot]"* | `leaderboard_panel.hpp` |
| 3 | *"How do I display per-team ANSI colors from `DriverProfile.color` inside FTXUI elements? Text() ignores raw ANSI codes."* | `leaderboard_panel.hpp`, `telemetry_panel.hpp` |
| 4 | *"I want the safety car to auto-retract using a `std::barrier` after all drivers have slowed down, not just a lap counter. How do I add that?"* | `telemetry_generator.cpp`, `lap_barrier.hpp` |
| 5 | *"How do I add a race-end splash screen in FTXUI that shows the results table before exiting?"* | `dashboard.cpp`, `dashboard.hpp` |

---

## Final Directory Snapshot

After Phase 7 is complete, your project should look like this:

```
pitwall/
├── CMakeLists.txt
├── CMakePresets.json
├── plan.md
├── learn/
│   ├── plan.md
│   ├── phase_1.md  ...  phase_7.md
├── src/
│   ├── main.cpp
│   ├── common/
│   │   ├── types.hpp
│   │   ├── season_data.hpp
│   │   ├── leaderboard.hpp
│   │   └── race_state.hpp
│   ├── concurrency/
│   │   ├── spsc_queue.hpp
│   │   ├── mpsc_queue.hpp
│   │   ├── thread_pool.hpp
│   │   └── thread_pool.cpp
│   ├── simulation/
│   │   ├── telemetry_generator.hpp
│   │   ├── telemetry_generator.cpp
│   │   ├── race_director.hpp
│   │   ├── pit_lane.hpp
│   │   └── lap_barrier.hpp
│   ├── race_control/
│   │   ├── track_limits.hpp
│   │   ├── track_limits.cpp
│   │   ├── penalty_enforcer.hpp
│   │   ├── penalty_enforcer.cpp
│   │   ├── weather.hpp
│   │   └── weather.cpp
│   ├── strategy/
│   │   ├── strategy_analyzer.hpp
│   │   └── strategy_analyzer.cpp
│   └── ui/
│       ├── dashboard.hpp
│       ├── dashboard.cpp
│       ├── leaderboard_panel.hpp
│       ├── telemetry_panel.hpp
│       └── race_control_panel.hpp
└── tests/
    ├── CMakeLists.txt
    ├── phase1/ ... phase6/
```

---

## Interview Cheat Sheet

| Topic | Where in PitWall | The question it answers |
|-------|-----------------|------------------------|
| Lock-free SPSC queue | `concurrency/spsc_queue.hpp` | *"Implement a lock-free ring buffer"* |
| `memory_order_acquire/release` | `spsc_queue.hpp` comments | *"What is acquire/release semantics? Why not seq_cst?"* |
| False sharing + `alignas(64)` | `spsc_queue.hpp` head_/tail_ | *"What is false sharing and how do you prevent it?"* |
| CAS loop | `concurrency/mpsc_queue.hpp` push() | *"Explain compare-and-swap with an example"* |
| ABA problem | `mpsc_queue.hpp` comment | *"What is the ABA problem? How does your queue avoid it?"* |
| Thread pool | `concurrency/thread_pool.hpp/.cpp` | *"Design a thread pool from scratch"* |
| SWMR / `shared_mutex` | `common/leaderboard.hpp` | *"When would you use shared_mutex over mutex?"* |
| `jthread` + `stop_token` | `simulation/telemetry_generator.cpp` | *"How do you safely stop a thread in modern C++?"* |
| `std::latch` | `simulation/race_director.hpp` | *"What's the difference between latch and barrier?"* |
| `std::barrier` | `simulation/lap_barrier.hpp` | *"How do you synchronise N threads at a checkpoint?"* |
| `std::counting_semaphore` | `simulation/pit_lane.hpp` | *"Semaphore vs mutex — when do you use each?"* |
| Atomic state machine + CAS | `race_control/penalty_enforcer.cpp` | *"How do you implement a lock-free state machine?"* |
| Atomic broadcast | `common/race_state.hpp` safety_car | *"How do you signal many threads without a lock?"* |
| Explicit memory ordering | `common/race_state.hpp` comments | *"Walk me through memory ordering on this code"* |
