# Day 1
- Claude made the plan for the project spliiting it into phases
- Learn more about CMake and especially this line `set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) `
- Inspect CMakeLists.txt in src, tets and root directory
- cmake -B build makes the build directory and cmake --build build runs it
- read std::array on learncpp.com
- know the use of each attribute in the struct of each of the types
- NOT YET GONE IN DEPTH INTO WHAT PHASE 1 TESTS MEAN - automated with AI tests


# dAY 2
Two real bugs I caught by test-compiling before writing the real files (both explained in the lesson doc):

std::jthread/std::stop_token need -fexperimental-library on Apple Clang — added conditionally.
std::barrier<std::function<void() noexcept>> doesn't compile on this libc++ — worked around with a noexcept lambda wrapper instead.
A third bug (template capacity mismatch in a test) got caught by the compiler during the build itself.

learn/lesson3.md now has a full file-by-file, function-by-function walkthrough, the gotchas, a "what I'd defend vs. what I'd fix" design retrospective, and a quiz section for interview prep.

Phase 4: Race Control Systems
Fixed that typo, and confirmed the tricky bits work on your machine. Now the actual build. Phase 4 adds four independent systems that all run alongside the Phase 3 simulation thread, each demonstrating a different concurrency tool:

System	Tool	Problem it solves
TrackLimitsMonitor	(uses the MpscQueue from Phase 2)	Probabilistically flags rule violations, pushes events
PenaltyEnforcer	std::atomic + compare_exchange_strong (CAS)	Guarantees exactly one penalty even if multiple threads race to issue it
WeatherSystem	std::shared_mutex	Lets 20 reader threads read weather concurrently, only blocking during the rare write
StrategyAnalyzer	ThreadPool + std::future	Runs 10 independent "what-if" simulations in parallel and picks the best
Let's start with TrackLimitsMonitor — the simplest of the four.

A quick contrast worth noting: Phase 3's TelemetryGenerator used a fixed RNG seed (rng_{42}) so tests got reproducible values. Here we use rng_{std::random_device{}()} — a genuinely random seed — because the tests only check statistical trends (more aggression → more violations over 10,000 tries), not exact values, so determinism isn't needed and real randomness is more honest.

Now the centerpiece: PenaltyEnforcer.

std::atomic + compare_exchange_strong (CAS), explained
Picture this race condition: three "track limits" warnings have already landed for a driver. A 4th and 5th warning arrive at the same instant from two different threads (imagine multiple monitors later). Both threads see warning_count == 3, and both think "I should be the one to issue the penalty!" Without protection, you'd issue two penalties for one offense.

compare_exchange_strong (a "CAS" — Compare-And-Swap) is an atomic instruction that says: "If the current value equals what I expect, swap it to the new value — and tell me whether it worked, all as one indivisible step." Two threads can call it on the same atomic at the same instant; the hardware guarantees only one of them sees expected == actual and succeeds — the other sees the value already changed and fails cleanly. That's how we guarantee exactly one penalty gets issued no matter how many threads race to the same 3rd-warning threshold.


////////BUGGGGGGGGGG//////////////////////
PenaltyEnforcer reads and writes the exact same MpscQueue. When process_events() drains the queue with while (events_.pop(ev)), and mid-drain it wins the CAS and calls events_.push(pen) to announce the penalty, that new PENALTY_ISSUED event lands in the same queue — and the still-running while loop pops it too, sees it isn't TRACK_LIMITS, and silently discards it via continue. The enforcer eats its own announcement before the test (or a future UI) ever sees it. That's why the test found 0 penalties instead of 1, even though the CAS logic itself is correct.

Fix: snapshot everything currently in the queue into a local buffer first, then process that buffer. Any event pushed while processing lands in the queue fresh, for the next call (or another consumer) to see — it's no longer at risk of being swallowed by the same pass that created it.
//////////////////////////////////////////////

///////buggg after running phase 5 to print in terminal
eresting — I let it run to completion and noticed something clearly unrealistic: 19 out of 20 drivers ended up penalized. That's not a Phase 4 bug — TrackLimitsMonitor::check() behaves exactly as designed and tested. The problem is how I'm calling it here: its BASE_RATE was calibrated (and tested) for being called roughly once per sector crossing (~3 times a lap), but I'm calling it every single 20ms tick — about 9x too often — so probability compounds into an almost-guaranteed penalty for everyone. Let me throttle it to match the cadence it was actually designed for.

Rewrote src/main.cpp to actually wire together everything you've built (Phases 1–5): the TelemetryGenerator producer thread, TrackLimitsMonitor + PenaltyEnforcer + WeatherSystem, and the Leaderboard/RaceState shared layer. It runs the simulated race, printing a live standings table every half-second, and prints final results when the race ends (~26 real seconds, since the simulation runs at 120x speed).

How to run it yourself:


cmake --build build
./build/src/RaceCondition-z
Let it run to the end, or Ctrl+C any time.

Two real bugs this caught (by actually running it, not just building it)
Column overflow: the "Gap" number used std::to_string(float), which always prints 6 decimal places (0.000000s) — wide enough to bleed into the next column with no space. Fixed with a proper fixed-precision formatter.
Unrealistic penalties: I first called TrackLimitsMonitor::check() every 20ms tick (50Hz). Its violation probability was calibrated in Phase 4 for being called roughly once per sector crossing (~3x/lap) — calling it 9x too often meant 19 of 20 drivers got penalized by race end. Throttled it to every 9th tick; now it's a realistic 1-in-20.
One thing to keep in mind for when we actually do Phase 6
The official Phase 6 plan (in phase_6.md) wires TrackLimitsMonitor, PenaltyEnforcer, and WeatherSystem all onto one shared event queue, and hands that same queue to the Dashboard for its event feed. That reproduces the exact self-consumption bug we found and fixed in Phase 4 — except now it would also silently swallow WEATHER_CHANGE events before the UI ever saw them. I sidestepped it here by reading each system's live query methods directly (weather.current(), penalty.penalty_state(id)) instead of draining a shared display queue — worth deciding deliberately, not accidentally, when we build the real dashboard.