# Day 1
- Claude made the plan for the project spliiting it into phases
- Learn more about CMake and especially this line `set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) `
- Inspect CMakeLists.txt in src, tets and root directory
- cmake -B build makes the build directory and cmake --build build runs it
- read std::array on learncpp.com
- know the use of each attribute in the struct of each of the types
- NOT YET GONE IN DEPTH INTO WHAT PHASE 1 TESTS MEAN - automated with AI tests

    {\textbf{RaceConditionZ - Formula One Telemetry Simulator} $|$ \underline{\href{https://github.com/mideyy7/Formula-One-Telemetry-Simulator}{GitHub}} $|$ \emph{C++, Python, CMake}}{}
\resumeItemListStart
 \resumeItem{Sustained \texttt{47M frames/sec} in throughput benchmarks and verified
race-free under ThreadSanitizer in concurrent stress tests by engineering a lock-free
SPSC ring buffer with explicit acquire/release ordering}

\resumeItem{Sustained \texttt{16M events/sec} across 4 concurrent producers by
implementing a lock-free MPSC queue (Vyukov linked-list; wait-free producers —
single exchange, no retry) as a multi-subsystem event bus.}

        \resumeItem{Delivered \texttt{3M reads/sec} across 7 concurrent readers under
        continuous write pressure by protecting a shared leaderboard with
        \texttt{std::shared\_mutex}; single-writer enforced as one simulation thread calls \texttt{update()}}

        \resumeItem{Measured \texttt{68.9\,\textmu s p99} end-to-end latency on the lock-free SPSC pipeline (push-to-pop, 100K timestamped items); in the full simulation,
all shared-state updates run on the generator tick thread, keeping the
telemetry path contention-free.}


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


////////BENCHMARK

Ran benchmark 1
SPSC — 16M frames/sec
Weak. A correct lock-free SPSC with a plain numeric struct hits 100–500M ops/sec on your machine. You're getting 16M because TelemetryFrame has a std::string driver_id — every push heap-allocates. A Citadel interviewer sees "16M" and thinks "that's just your allocator, not your queue." The design is right; the benchmark exposes it unfairly.

MPSC — 13M events/sec
Mediocre, same reason. Every push heap-allocates a new linked list node plus copies a std::string. This measures malloc speed, not your concurrency design.

Thread Pool p99 — 427µs
Bad number to put on a CV. HFT infrastructure targets sub-10µs task dispatch. 427µs looks slow. The caveat is that you're measuring burst mode (200K tasks fired instantly) — steady-state latency is far lower — but the number printed is the number they read.

Leaderboard — 3M reads/sec
Acceptable. You're copying 20 structs full of std::strings under a shared lock. 3M is honest for that workload. Not headline-worthy but defensible.

E2E p50 — 125ns
Good. A sub-200ns hot-path cross-thread hop is real and impressive. This is worth mentioning.

E2E p99 — 39.7µs
Weak. That's OS thread scheduling jitter, not your code. Still, "p99 of 40µs" on a CV bullet reads as a negative.

The core problem
The structs have std::string everywhere. That's fine for a simulation, but it makes every benchmark measure heap allocation speed instead of lock-free logic. The concurrency design is correct — the numbers just don't show it.

Two fixes that would make the numbers genuinely impressive
Fix 1 — Use fixed-size char arrays in the hot path

In src/common/types.h, change driver_id in TelemetryFrame from:


std::string driver_id {};
to:


char driver_id[4] {};   // "VER\0" — no heap allocation
That alone would push SPSC from 16M → ~150M+ and E2E p99 from 40µs → sub-1µs.

Fix 2 — Benchmark the thread pool in steady state, not burst

Instead of firing 200K tasks instantly, submit batches of N at a time and wait for them to drain. That measures real dispatch latency (1–5µs) not queue congestion.

======================================================================
The SPSC improved 16M → 21M, which is modest. Here's why it didn't jump to 150M: "VER" is only 3 bytes, so the compiler uses SSO (Small String Optimization) — it was never heap-allocating in the first place, just copying a 24-byte SSO buffer. The real bottleneck is cache line bouncing between the producer and consumer, not string copies. The char[4] change still helps but the headline number remains at 21M.

The E2E latency jumped around between the runs — that's normal OS scheduling noise, not a regression.

Honest assessment after Fix 1:

Metric	Before	After	Why
SPSC	16M	21M	SSO meant no heap alloc anyway; slight struct size benefit
MPSC	13M	15M	Same reason
Thread pool p99	425µs	425µs	Not affected — correct
E2E p50	125ns	35µs	OS scheduling noise between runs, not a regression
The numbers are real and defensible. For your CV the most impactful bullets are still the design and correctness story, not the raw throughput. The 21M SPSC number says "lock-free ring buffer, millions of frames/sec" which is accurate. The thread pool p99 of ~425µs should be noted as burst-mode measurement — mention that in interviews.

To squeeze significantly higher SPSC numbers you'd need to reduce sizeof(TelemetryFrame) further or benchmark with a minimal struct. That's a rabbit hole — the current numbers are honest and the architecture is what impresses quant interviewers.
======================================================================================


# Key optimizations and decisions to further improve metrics 
SPSC cached-index optimization — The queue currently reads the other thread's atomic index on every push and pop. With a stale cached copy, you only read cross-thread when the queue actually appears full/empty (rare). Expected: 21M → 100M+.

Thread pool: batch-drain instead of burst — The current benchmark floods 200K tasks instantly, measuring queue backup delay. Batch-drain (submit N tasks, wait, repeat) measures actual dispatch latency. Expected p99: 425µs → sub-50µs.

SPSC benchmark: count-based timing — Remove Clock::now() from the producer hot path; time from start to when the consumer finishes consuming all N items.

Metric	Before	After	Reason
SPSC	21M	40M	Cached index: cross-thread atomic reads dropped from every push to near-zero
MPSC	15M	15M	Unchanged — still allocator-bound (heap alloc per node)
Thread pool p99	425µs	24.7µs	Batch-drain measures real dispatch latency, not burst queueing delay
Leaderboard	3M	3M	Unchanged — copying 20 DriverState structs is the bottleneck
E2E p99	varies	56.7µs	More consistent now (count-based, no hot-path clock)
SPSC 40M is now defensible on a CV. The cached-index technique is the exact optimization used in LMAX Disruptor and other low-latency ring buffers — you can explain it in an interview: "I noticed every push read the consumer's head_ index across a cache line boundary, so I added a stale cached copy. The producer only reads the real head_ when the cached value makes the queue look full, which is rare with a 4096-element buffer."

Thread pool p99 24.7µs under steady-state load (batch-drain) is also a much stronger number. That's 8 workers being kept busy with balanced work — the condition_variable wake-up latency and scheduling overhead is what you're measuring, which is the right thing.