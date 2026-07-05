# Lesson 4 — Race Control Systems

My own reference for Phase 4: what every file does, function by function, the
bugs I actually hit, and an honest retrospective — written so I can explain
this to someone else (or defend it in an interview) without hand-waving.

---

## 0. The big picture

Phase 3 gave us a producer thread generating telemetry. Phase 4 adds four
independent systems that would run *alongside* that simulation, each judged
on a different rule of the race, and each built to showcase one more C++
concurrency tool:

| System | Tool | Problem it solves |
|---|---|---|
| `TrackLimitsMonitor` | plain probability + the `MpscQueue` from Phase 2 | Randomly (but realistically) flags rule violations, pushes events |
| `PenaltyEnforcer` | `std::atomic` + `compare_exchange_strong` (CAS) | Guarantees exactly one penalty even if many threads race to issue it |
| `WeatherSystem` | `std::shared_mutex` | Lets many readers read weather concurrently, only blocking during the rare write |
| `StrategyAnalyzer` | `ThreadPool` (Phase 2) + `std::future` | Runs 10 independent pit-strategy simulations in parallel, picks the best |

Files:

```
src/race_control/track_limits.h/.cpp
src/race_control/penalty_enforcer.h/.cpp
src/race_control/weather.h/.cpp
src/strategy/strategy_analyzer.h/.cpp
tests/phase4/test_track_limits.cpp
tests/phase4/test_penalty_enforcer.cpp
tests/phase4/test_weather.cpp
tests/phase4/test_strategy_analyzer.cpp
```

None of these four systems are wired into `TelemetryGenerator` yet — same as
Phase 3's `RaceDirector`/`LapBarrier`/`PitLane`, they're built and proven in
isolation first. That's a deliberate habit worth calling out on its own: it's
much easier to get a concurrency primitive right (and to unit test it
convincingly) in a small class with a dedicated queue than while also
juggling the full simulation loop.

---

## 1. A pre-existing bug I fixed before writing any Phase 4 code

`src/common/types.h` had:

```cpp
enum class PenaltyState {
    NONE,
    PENDINF,   // <-- typo, should be PENDING
    SERVING,
    SERVED
};
```

`PENDINF` was a typo from Phase 1 that nothing had referenced yet — `DriverState`
just stores a `PenaltyState`, it never spells out the enumerator name. Phase 4
is the first code that actually writes `PenaltyState::PENDING` (in the CAS
transition and in every penalty test), so this typo would have surfaced as a
confusing "no member named 'PENDING' in ... 'PenaltyState'" compile error the
moment I typed real code. I grepped for every use of `PENDINF`/`PENDING`
across the repo first, confirmed nothing depended on the misspelling, and
renamed it before writing anything else.

**Lesson:** typos in enum/identifier names don't announce themselves until
something finally references the correctly-spelled version. Worth a quick
grep before building on top of an old header you haven't touched in a while.

---

## 2. `track_limits.h` / `.cpp` — the probabilistic event producer

### Purpose
Every "monitoring cycle," look at all 20 drivers and roll a weighted die for
each one: did they exceed track limits at this instant? If yes, push a
`TRACK_LIMITS` event onto the shared `MpscQueue<RaceControlEvent>`.

### The header

```cpp
class TrackLimitsMonitor {
public:
    explicit TrackLimitsMonitor(MpscQueue<RaceControlEvent>& event_queue);
    void check(const std::vector<DriverState>& states, int current_lap);
private:
    MpscQueue<RaceControlEvent>& events_;
    std::mt19937                 rng_{std::random_device{}()};
    std::uniform_real_distribution<float> coin_{0.0f, 1.0f};
    static constexpr float BASE_RATE = 0.004f;
};
```

- **`explicit TrackLimitsMonitor(...)`** — constructor, takes the *shared*
  output queue by reference (same ownership pattern as Phase 3's
  `TelemetryGenerator`: this class doesn't own the queue, it just writes to
  it).
- **`void check(...)`** — the only real function. Called once per monitoring
  pass with the current standings.
- **`rng_{std::random_device{}()}`** — worth contrasting with Phase 3's fixed
  `rng_{42}`. `TelemetryGenerator`'s tests check *exact trends* (tire wear
  strictly increases), so a fixed seed keeps them deterministic. This class's
  tests only check *statistical* trends (more aggression → more violations
  across 10,000 tries), so a real random seed is more honest and no less
  testable.

### `check()` — one function, walked through

```cpp
void TrackLimitsMonitor::check(const std::vector<DriverState>& states, int current_lap) {
    for (const auto& state : states) {
        if (state.in_pit) continue; // can't violate track limits in pits

        const auto& frame = state.latest_frame;
        float speed_factor = frame.speed_kph / 280.0f;
        float prob = BASE_RATE
                   * state.profile.aggression
                   * (1.0f + frame.tire_wear)
                   * speed_factor;

        if (coin_(rng_) < prob) {
            RaceControlEvent ev;
            ev.type      = RaceControlEvent::Type::TRACK_LIMITS;
            ev.driver_id = state.profile.id;
            ev.lap       = current_lap;
            ev.message   = state.profile.id + " exceeded track limits (S" + ...;
            ev.timestamp = std::chrono::steady_clock::now();
            events_.push(std::move(ev));
        }
    }
}
```

`coin_(rng_)` draws a uniform random float in `[0, 1)`. If it lands below
`prob`, that's a "hit" — the formula's ingredients each make sense on their
own: more aggressive drivers push the limits more (`aggression`), worn tires
are harder to keep on track (`1 + tire_wear`), and going faster gives less
margin for error (`speed_factor`). Multiplying them together means any one
factor being low pulls the whole probability down, which matches intuition:
a cautious driver on fresh tires going slowly essentially never gets flagged.

---

## 3. `penalty_enforcer.h` / `.cpp` — the CAS state machine (the centerpiece)

### The problem it solves, restated

Say a driver has 2 accumulated warnings. If two `TRACK_LIMITS` events for the
"3rd warning" arrive back to back (or if this were ever fed by multiple
producer threads), you don't want to issue two penalties for one crossing of
the threshold. `compare_exchange_strong` (CAS) is an atomic
"if-it's-still-what-I-expect, swap it, and tell me whether that worked, all
as one indivisible hardware step." Two threads can call it on the same atomic
at the same instant; exactly one sees the swap succeed.

### The header

```cpp
class PenaltyEnforcer {
public:
    explicit PenaltyEnforcer(MpscQueue<RaceControlEvent>& event_queue);
    void process_events();
    void driver_entered_pits(const std::string& driver_id);
    void driver_exited_pits(const std::string& driver_id);
    PenaltyState penalty_state(const std::string& driver_id) const;
    int warning_count(const std::string& driver_id) const;
private:
    int driver_index(const std::string& id) const;
    MpscQueue<RaceControlEvent>& events_;
    std::array<std::atomic<int>,          20> warnings_{};
    std::array<std::atomic<PenaltyState>, 20> states_{};
    std::array<std::atomic<int>,          20> extra_pit_time_{};
};
```

State machine per driver (this is worth memorizing — it's the whole point of
the class):

```
NONE --(3rd warning, via CAS)--> PENDING --(enters pits)--> SERVING --(exits pits)--> SERVED
```

- **`std::array<std::atomic<int>, 20> warnings_{}`** — one atomic counter per
  driver, indexed by position in `DRIVERS`. `{}` value-initializes every
  element, which for `std::atomic<int>` (C++20 onward) means each one starts
  at 0 — you can't just `= {0, 0, 0, ...}` an array of atomics because
  `std::atomic` has no copy constructor, but value-initialization doesn't
  need one.
- **`std::array<std::atomic<PenaltyState>, 20> states_{}`** — same idea, one
  atomic enum per driver. `std::atomic<SomeEnum>` works because a plain
  `enum class` is trivially copyable — the same requirement any type needs to
  live inside `std::atomic`.

### Function by function

- **Constructor** — loops over all three arrays and explicitly `.store()`s
  starting values (`NONE`, `0`, `0`). You might wonder why this is needed if
  `{}` already value-initializes them — it's belt-and-suspenders here (the
  member initializers already guarantee it), but being explicit makes the
  starting state obvious to a reader without having to know the value-init
  rule for atomics by heart.

- **`driver_index(id)`** — linear search through the 20-entry `DRIVERS`
  array to translate a driver ID string into an array index. `O(20)`, called
  on every event — fine at this scale, would become a hash map lookup if this
  ever needed to scale past a small fixed roster.

- **`process_events()`** — drains the queue and reacts to `TRACK_LIMITS`
  events. This is where I found a real bug (full story in section 6 below).
  The corrected version snapshots the queue into a local `std::vector` first,
  *then* processes the snapshot:

  ```cpp
  std::vector<RaceControlEvent> batch;
  RaceControlEvent ev;
  while (events_.pop(ev)) batch.push_back(std::move(ev));

  for (auto& e : batch) {
      if (e.type != RaceControlEvent::Type::TRACK_LIMITS) continue;
      int idx = driver_index(e.driver_id);
      if (idx < 0) continue;

      int count = warnings_[idx].fetch_add(1, std::memory_order_relaxed) + 1;
      if (count == 3) {
          PenaltyState expected = PenaltyState::NONE;
          if (states_[idx].compare_exchange_strong(
                  expected, PenaltyState::PENDING,
                  std::memory_order_acq_rel, std::memory_order_acquire))
          {
              // we won the race — issue exactly one penalty
              RaceControlEvent pen; /* ... */ events_.push(std::move(pen));
              extra_pit_time_[idx].store(3, std::memory_order_relaxed);
          }
      }
  }
  ```

  `fetch_add` is itself atomic — no CAS needed for the counter, because we
  don't care about "the exact instant it crossed 3," we only care about the
  return value ("what did it become after I added 1"). The CAS is only
  needed for the NONE→PENDING *transition*, because that's the one-time
  action ("issue a penalty") that must not happen twice.

- **`driver_entered_pits(id)` / `driver_exited_pits(id)`** — each does one
  CAS with a specific `expected` value:

  ```cpp
  PenaltyState expected = PenaltyState::PENDING;
  states_[idx].compare_exchange_strong(expected, PenaltyState::SERVING, ...);
  ```

  If the driver *isn't* currently `PENDING` (e.g. they have no penalty at
  all), the CAS simply fails and nothing happens — the return value is
  ignored on purpose, because "this driver wasn't in the expected state" is
  not an error here, it's just a no-op.

- **`penalty_state(id)` / `warning_count(id)`** — plain atomic loads, the
  read side of the state machine.

---

## 4. `weather.h` / `.cpp` — `std::shared_mutex` (SWMR)

### The problem it solves

A plain `std::mutex` only ever lets one thread in — even two threads that
only want to *read* the same value have to take turns. Weather changes maybe
once every few laps, but could be **read** up to 20 times every tick (once
per driver) if wired into the simulation. `std::shared_mutex` gives two lock
modes:

- `std::unique_lock` — exclusive; blocks everyone (readers and writers). Use
  when writing.
- `std::shared_lock` — shared; many threads can hold it *simultaneously*, as
  long as nobody holds (or is waiting for) the exclusive lock. Use when only
  reading.

### The header

```cpp
class WeatherSystem {
public:
    explicit WeatherSystem(MpscQueue<RaceControlEvent>& event_queue);
    void update(int current_lap);
    WeatherState current() const;
    float grip_factor() const;
private:
    MpscQueue<RaceControlEvent>& events_;
    WeatherState                 state_{WeatherState::DRY};
    mutable std::shared_mutex    mutex_;
    ...
};
```

`mutable std::shared_mutex mutex_` — `current()` and `grip_factor()` are
`const` member functions (callers shouldn't need a non-const reference just
to *read* the weather), but locking a mutex modifies its internal state.
`mutable` is the standard escape hatch for "this member needs to change even
inside a `const` function, because it's bookkeeping, not the object's logical
state."

### Function by function

- **`update(int current_lap)`** — throttles itself (`if (current_lap -
  last_update_lap_ < 5) return;`) so weather can't flip every single tick,
  then rolls a die (`coin_(rng_) > CHANGE_PROBABILITY`) to decide whether to
  change at all. If it does change, it cycles `DRY → DAMP → WET → DRY`, takes
  a brief `shared_lock` just to read the current state for the `switch`, then
  a `unique_lock` to actually write the new state, then pushes a
  `WEATHER_CHANGE` event.

  Worth being honest about one detail here (see retrospective, section 7):
  since the class's own comment states *"written by a single background
  thread,"* the `shared_lock` taken inside `update()` to read `state_` isn't
  actually protecting against anything — `update()` is the only writer, and a
  thread doesn't need a lock to read data only it itself ever writes. The
  `shared_lock`/`unique_lock` split genuinely matters for `current()` and
  `grip_factor()`, which *are* called from other threads.

- **`current() const`** — `std::shared_lock read{mutex_}; return state_;` —
  the whole point of the class. Many threads can call this at once without
  blocking each other.

- **`grip_factor() const`** — same locking, maps `WeatherState` to a
  multiplier (`DRY=1.00`, `DAMP=0.85`, `WET=0.70`) that a later phase would
  multiply into speed/tire calculations.

---

## 5. `strategy_analyzer.h` / `.cpp` — futures and task batching

### The pattern

1. Submit N independent jobs to the `ThreadPool` all at once — each
   `submit()` returns immediately with a `std::future<float>`, a placeholder
   for "the answer, whenever it's ready."
2. Call `.get()` on each future in turn. `.get()` blocks *only* until that
   specific task finishes.
3. Because all N tasks were already running in parallel on the pool's worker
   threads, the total wall-clock wait is roughly "the slowest one," not "the
   sum of all of them."

This is the same shape used in quant systems to price multiple independent
scenarios (different rate curves, different models) at once rather than
serially.

### The header

```cpp
struct StrategyResult {
    int   pit_lap;
    float estimated_race_time_s;
};

class StrategyAnalyzer {
public:
    explicit StrategyAnalyzer(ThreadPool& pool);
    StrategyResult analyze(const DriverState& driver, const TrackProfile& track, int total_laps);
    static constexpr std::array<int, 10> CANDIDATES = {12, 15, 18, 21, 24, 27, 30, 33, 36, 39};
private:
    static float simulate_one_strategy(const DriverProfile&, const CarProfile&,
                                        const TrackProfile&, int pit_lap, int total_laps);
    ThreadPool& pool_;
};
```

`ThreadPool& pool_` — same reference-member pattern as `TelemetryGenerator`'s
queue: the analyzer doesn't own the pool's lifetime, and in the tests
multiple `StrategyAnalyzer` calls (even concurrent ones, from `std::async`)
share the exact same pool instance.

### Function by function

- **`analyze(driver, track, total_laps)`** — the orchestrator:

  ```cpp
  std::vector<std::future<float>> futures;
  for (int pit_lap : CANDIDATES) {
      futures.push_back(pool_.submit(
          simulate_one_strategy, driver.profile, driver.car, track, pit_lap, total_laps));
  }
  StrategyResult best{CANDIDATES[0], std::numeric_limits<float>::max()};
  for (std::size_t i = 0; i < CANDIDATES.size(); ++i) {
      float t = futures[i].get();
      if (t < best.estimated_race_time_s) best = {CANDIDATES[i], t};
  }
  return best;
  ```

  All 10 submits happen in the first loop (non-blocking — they just enqueue
  work). Only the second loop blocks, and only one task at a time, in
  submission order — but since they're all already running concurrently on
  the pool's worker threads, `futures[i].get()` for an already-finished task
  returns instantly.

- **`simulate_one_strategy(profile, car, track, pit_lap, total_laps)`** —
  `static`, and every parameter is passed **by value**, not by reference.
  This matters: this function runs on a worker thread, potentially well
  after `analyze()`'s local variables could have gone out of scope on the
  calling thread. Passing by value means each task carries its own
  independent copy — there is no reference back to memory another thread
  might be mutating or destroying. It's a small, simplified lap-by-lap loop:
  add a fixed pit-lane time penalty on the chosen `pit_lap`, accumulate tire
  wear and fuel penalties into lap time, and return the total.

---

## 6. The real bug I hit: an enforcer that ate its own output

Writing `test_penalty_enforcer.cpp`'s `ConcurrentThirdWarningExactlyOnePenalty`
test, I got:

```
Expected equality of these values:
  penalty_count
    Which is: 0
  1
```

Zero penalties found, even though the CAS logic itself is correct. Here's
what was actually happening, step by step:

1. `process_events()` starts draining the queue: `while (events_.pop(ev)) { ... }`.
2. It pops the event that pushes the warning count to exactly 3. The CAS
   succeeds, and inside that same loop iteration it does
   `events_.push(std::move(pen))` to announce the `PENALTY_ISSUED` event —
   **onto the exact same queue it is currently draining.**
3. The `while` loop hasn't exited yet (there were more `TRACK_LIMITS` events
   still queued from the other 3 threads). It keeps popping — and a few
   iterations later, it pops the `PENALTY_ISSUED` event it just pushed
   *itself*.
4. That event's type isn't `TRACK_LIMITS`, so the loop's very first line —
   `if (ev.type != RaceControlEvent::Type::TRACK_LIMITS) continue;` —
   silently discards it.

The enforcer consumed and threw away its own announcement before the test
(or, in a real system, a UI) ever got a chance to see it. Not a data race —
a straightforward logic bug that only shows up because input and output share
one queue and draining and processing happened in the same pass.

**The fix:** snapshot the queue into a local `std::vector<RaceControlEvent>`
first, *then* iterate the snapshot. Anything `process_events()` pushes while
reacting to the snapshot lands in the (now-empty) live queue, safely out of
reach of the loop that's currently running:

```cpp
std::vector<RaceControlEvent> batch;
RaceControlEvent ev;
while (events_.pop(ev)) batch.push_back(std::move(ev));

for (auto& e : batch) { /* ... same logic, referencing e instead of ev ... */ }
```

**Lesson for myself:** whenever a function both *reads from* and *writes to*
the same queue/collection in one pass, ask "could something this function
produces be mistaken for something it should consume?" If yes, decouple the
read phase from the write phase.

---

## 7. Design decisions — what I'd defend, and what I'd fix

### Things I think are solid

- **CAS for the one-time NONE→PENDING transition, `fetch_add` for the
  counter.** Using the cheaper primitive (`fetch_add`) where correctness
  doesn't require CAS, and reserving CAS for the one spot where "exactly one
  winner" actually matters, shows the tool was chosen for the job rather than
  reached for by default.
- **`shared_mutex` only where it earns its keep.** `current()` and
  `grip_factor()` are the actual multi-reader hot path; that's where the
  shared/unique split pays off.
- **`simulate_one_strategy` takes everything by value.** Correct and
  deliberate for code that runs on a different thread than its caller — no
  dangling references, no shared mutable state between concurrent scenario
  runs.
- **Found and fixed a real bug by running the tests, not by trusting the
  design.** The self-consumption bug (section 6) wasn't something a code
  review would obviously catch — it took an actual concurrent test run
  producing a wrong number to surface it.
- **Caught the `PENDINF` typo before it could cause a confusing error deep
  inside real logic**, by grepping first.

### Things I'd flag as weaknesses if an interviewer pushed on them

- **`PenaltyEnforcer::process_events()` drains and discards *any* non-
  `TRACK_LIMITS` event from whatever queue it's given**, not just its own.
  In today's tests that's harmless because each test builds a fresh, private
  `MpscQueue` just for that one system. But if a later phase wires
  `TrackLimitsMonitor`, `PenaltyEnforcer`, and `WeatherSystem` onto **one
  shared** race-control event bus (which `RaceControlEvent`'s design clearly
  anticipates — it has a `Type` field covering all of them), this same class
  would silently swallow `WEATHER_CHANGE`/`PIT_ENTRY`/etc. events meant for a
  UI, because it drains the *entire* queue and only re-emits what it
  personally produces. The honest fix: either give the enforcer its own
  dedicated inbound queue (separate from the shared outbound/display queue),
  or have it push unrecognized events back out rather than discarding them.
- **`driver_index()` is an O(20) linear scan per event.** Completely fine at
  20 drivers; if this pattern got reused somewhere hotter or with a bigger
  roster, a `std::unordered_map<std::string, int>` built once in the
  constructor would be the natural upgrade.
- **The `20` in `std::array<std::atomic<int>, 20>` is a repeated magic
  number** — it also appears as `DRIVERS.size()` in `season_data.h`. Nothing
  enforces they stay in sync; a `static constexpr` shared constant (or
  `DRIVERS.size()` used directly, if it can be made available at that point)
  would remove the duplication.
- **`WeatherSystem::update()`'s `shared_lock` around reading `state_` isn't
  protecting against anything**, given the class's own stated contract
  ("written by a single background thread"). It's not wrong — shared locks
  are safe to take even when uncontended — but it's a lock that isn't doing
  real work, which is worth being able to say out loud rather than presenting
  it as necessary.
- **`TrackLimitsMonitor`, `PenaltyEnforcer`, and `WeatherSystem` are not
  actually wired into the live simulation loop yet** — same caveat as
  Phase 3's `RaceDirector`/`LapBarrier`/`PitLane`. They're correct and tested
  in isolation, but "where does this get called from in the real race?" is
  still an open question for a later phase.

---

## 8. Quiz yourself — likely interview questions

**Q: Why CAS instead of just checking-then-setting the state?**
A: Check-then-set ("if state == NONE, set state = PENDING" as two separate
steps) has a race window between the check and the set — two threads could
both pass the check before either writes. CAS makes "compare and swap" one
indivisible hardware operation, so only one thread can ever win when several
attempt the same transition simultaneously.

**Q: Why does the warning counter use `fetch_add` instead of CAS?**
A: Because incrementing doesn't need "exactly one winner" semantics — every
increment is valid and should count. CAS is for transitions that must happen
at most once; `fetch_add` is for accumulation where every caller's
contribution is legitimate.

**Q: What's the difference between `std::shared_lock` and
`std::unique_lock` on a `std::shared_mutex`?**
A: `unique_lock` is exclusive — only one holder, blocks everyone else.
`shared_lock` allows multiple simultaneous holders, as long as no thread
holds (or is waiting for) the unique lock. Use unique for writes, shared for
reads.

**Q: Walk me through the bug you found in `PenaltyEnforcer`.**
A: `process_events()` both read `TRACK_LIMITS` events from and wrote
`PENALTY_ISSUED` events to the same queue, in a single draining `while`
loop. A penalty event pushed mid-loop got popped by that same loop moments
later, recognized as "not a TRACK_LIMITS event," and discarded — so the
penalty announcement never reached anyone. Fixed by snapshotting the queue
into a `std::vector` before processing, so anything produced during
processing is left for the next call instead of being immediately
re-consumed.

**Q: Why does `simulate_one_strategy` take its parameters by value instead
of by `const&`, unlike most other functions in this codebase?**
A: It runs on a worker thread pulled from a thread pool, invoked
asynchronously relative to the caller. A reference could outlive (or be
outlived by) the caller's stack frame depending on scheduling, so each task
needs its own independent copy of everything it touches — passing by value
guarantees that.

**Q: If you wired these four systems onto one shared event queue in a later
phase, what would you need to change?**
A: `PenaltyEnforcer::process_events()` would need to stop discarding
unrecognized event types — right now it silently eats anything that isn't
`TRACK_LIMITS` from whatever queue it's handed, which is fine when it owns a
private queue but would destroy other systems' events on a shared bus.

---

## 9. Building and testing this phase

```bash
cmake --build build
ctest --test-dir build -R phase4 --output-on-failure

# ThreadSanitizer (needs a reconfigure since new source files were added)
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan --target phase4_tests
./build-tsan/tests/phase4_tests
```

Result: all 12 phase4 tests pass, both normally and under TSan (0 warnings),
and the full suite (phase1 + phase2 + phase3 + phase4) still passes — no
regressions.