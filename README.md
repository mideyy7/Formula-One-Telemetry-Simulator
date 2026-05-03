# F1 Telemetry Simulator

A multi-threaded Formula 1 race simulator written in C++17. Generates realistic telemetry at 50Hz and renders a live, color-coded leaderboard in your terminal.

Built to explore C++ concurrency patterns — specifically producer-consumer pipelines with condition variables, parallel futures, and thread-safe state machines in a domain that makes the data interesting.

## What it simulates

- **Full 2025 F1 grid** — 20 drivers across 10 teams, each with individual aggression, consistency, tire management, and risk tolerance profiles
- **Tire wear** — progressive degradation; pit stop threshold varies per driver (65–90% wear)
- **Fuel load** — cars start at 100 kg, burn 0.2 kg/km; full load costs ~5 kph top speed and refuelling happens at pit stops
- **DRS** — activates in sector 1 when a driver is within 1 second of the car ahead; gives ~12 kph boost
- **Gap to leader** — computed from raw distance delta at a 200 kph reference speed and shown live on the leaderboard
- **Fastest lap** — personal bests tracked per driver; overall holder gets a purple `⚡FL` badge
- **Track limits** — violations checked at sector boundaries; 3 warnings issue a timed penalty enforced during the next pit stop
- **Pit stop strategy** — optional pre-race optimal pit lap search using parallel simulation (`std::async` / `std::future`)

## Architecture

```
┌──────────────────┐       ┌──────────────────┐       ┌──────────────────┐
│ TelemetryGen     │──────▶│   RingBuffer     │──────▶│ Consumer Thread  │
│ (Producer, 50Hz) │       │ (1024, blocking) │       │ (Leaderboard)    │
└──────────────────┘       └──────────────────┘       └──────────────────┘
```

The ring buffer uses `std::condition_variable` so threads sleep when waiting — no busy-spinning. `TrackLimitsMonitor` and `PenaltyEnforcer` run concurrently with mutex-protected state.

## Build

**Requirements:** C++17 compiler, pthreads (GCC 7+, Clang 5+, or MSVC 2017+)

```bash
g++ -std=c++17 -I src \
  src/main.cpp \
  src/telemetry/TelemetryGenerator.cpp \
  src/strategy/RaceSimulator.cpp \
  src/strategy/StrategyAnalyzer.cpp \
  src/race-control/TrackLimitsMonitor.cpp \
  src/race-control/PenaltyEnforcer.cpp \
  -o f1-telemetry -pthread
```

Swap `g++` for `clang++` — same flags.

## Run

```bash
./f1-telemetry
```

At startup you'll be asked if you want to run strategy analysis. Type `y`, then enter comma-separated driver indices (e.g. `4,6,1`) to compute optimal pit laps for those drivers before the race begins.

## Leaderboard

The terminal display refreshes every tick and shows all 20 drivers:

```
🏁 LAP 15/50 🏁
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
P1  🔴 Charles Leclerc    ████████░░  Lap 15  220 kph  Tire 31%  [LEADER]
P2  🔵 Max Verstappen     ███████░░░  Lap 15  217 kph  Tire 28%  +1.4s
P3  🟠 Lando Norris       ██████░░░░  Lap 14  211 kph  Tire 44%  +8.2s  DRS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
⚡ Fastest Lap: Max Verstappen  1:18.432
```

**Badges:**
| Badge | Meaning |
|---|---|
| Gold `LEADER` | Race leader (P1) |
| Grey `+X.Xs` | Gap to leader |
| Green `DRS` | Rear wing open |
| Purple `⚡FL` | Fastest lap holder |
| Purple `[IN PITS]` | Currently in pit lane |

**Color coding:** speed (green/yellow/red), tire wear (green/yellow/red), fuel load (green/yellow/red), top-3 positions (gold/silver/bronze)

**Team emojis:** 🔴 Ferrari · 🔵 Red Bull · ⚪ Mercedes · 🟠 McLaren · 🟢 Aston Martin · 💙 Alpine & Williams · ⚫ Racing Bulls & Kick Sauber

## Project structure

```
f1-telemetry/
├── src/
│   ├── main.cpp
│   ├── common/types.h               # TelemetryFrame, DriverProfile, CarProfile, TrackProfile
│   ├── data/season_data.h           # 2025 driver & team definitions
│   ├── ingestion/RingBuffer.h       # Thread-safe circular buffer
│   ├── telemetry/
│   │   ├── TelemetryGenerator.h
│   │   └── TelemetryGenerator.cpp   # Core simulation loop
│   ├── strategy/
│   │   ├── StrategyAnalyzer.{h,cpp} # Parallel optimal pit lap search
│   │   └── RaceSimulator.{h,cpp}    # Lightweight sim used by analyzer
│   └── race-control/
│       ├── TrackLimitsMonitor.{h,cpp}
│       └── PenaltyEnforcer.{h,cpp}
└── README.md
```

