# Lesson 3 — The Simulation Engine (threading primitives)

My own reference for Phase 3: what every file does, function by function, plus
an honest retrospective on what's good and what's not, so I can explain this
project to someone else (or defend it in an interview) without hand-waving.

---

## 0. The big picture

Phase 1 gave us data (`TelemetryFrame`, `DriverState`, ...). Phase 2 gave us
pipes (`SpscQueue`, `MpscQueue`, `ThreadPool`). Phase 3 gives us the thing
that actually generates data: `TelemetryGenerator` — a background thread that
pretends to be 20 F1 cars, ticking 50 times a second, pushing one
`TelemetryFrame` per car per tick into an `SpscQueue`.

Alongside it, three small standalone classes each wrap one C++20
synchronization primitive (`std::latch`, `std::barrier`,
`std::counting_semaphore`) as an isolated, unit-tested example — they are
**not wired into the simulation yet** (more on that in the retrospective).

Files:

```
src/simulation/telemetry_generator.h    (declaration)
src/simulation/telemetry_generator.cpp  (implementation — the real logic)
src/simulation/race_director.h          (std::latch wrapper)
src/simulation/lap_barrier.h            (std::barrier wrapper)
src/simulation/pit_lane.h               (std::counting_semaphore wrapper)
tests/phase3/test_telemetry_generator.cpp
tests/phase3/test_latch.cpp
tests/phase3/test_barrier.cpp
tests/phase3/test_semaphore.cpp
```

---

## 1. `telemetry_generator.h` — the class's public shape

This file only *declares* things; the logic lives in the `.cpp`. Reading a
header first is the fastest way to understand what a class is for before
diving into how it works.

```cpp
inline constexpr float TIME_SCALE     = 120.0f;
inline constexpr float TICK_S         = 0.02f;
inline constexpr float SIM_S_PER_TICK = TIME_SCALE * TICK_S;
inline constexpr float TIRE_WEAR_BASE = 0.00130f;
inline constexpr int   PIT_STOP_TICKS = 11;
inline constexpr int   TOTAL_LAPS     = 50;
```
Named constants instead of magic numbers scattered through the logic. `inline`
is required here because these live in a header included from multiple `.cpp`
files — without `inline`, each translation unit would define its own copy of
the variable and the linker would complain about duplicate symbols.

```cpp
using OnTickCallback = std::function<void(const std::vector<DriverState>&)>;
```
A type alias for "a function that takes the current standings and returns
nothing." This is how the generator can notify *someone else* (later: the
leaderboard) every tick, without the generator needing to know anything about
what a leaderboard is. That's the whole point of a callback: it decouples the
producer from whoever consumes its events.

### The public functions, one at a time

- **`explicit TelemetryGenerator(SpscQueue<TelemetryFrame, 2048>& queue, const TrackProfile& track = DEFAULT_TRACK)`**
  Constructor. Takes the queue *by reference* — the generator does not own
  the queue's lifetime, `main()` does. `explicit` blocks the compiler from
  silently converting some other type into a `TelemetryGenerator` behind your
  back in a function call; always add it to single/defaulted-argument
  constructors unless you specifically want implicit conversions.

- **`void set_on_tick(OnTickCallback cb)`**
  Lets external code (tests, later the leaderboard) register a callback that
  fires once per tick with the current standings.

- **`void start()`**
  Launches the background thread.

- **`void stop()`**
  Asks the thread to stop and waits for it to actually finish.

- **`bool is_running() const`**
  `{ return thread_.joinable(); }` — a `jthread` is "joinable" if it's
  currently running a thread of execution (hasn't been joined/detached/moved
  from/default-constructed).

- **`std::vector<DriverState> standings() const`**
  Returns a **copy** of the current state of all 20 drivers. Safe-by-copy: the
  caller gets their own independent vector, not a reference into memory the
  background thread might still be writing to.

- **`int race_lap() const` / `bool race_finished() const`**
  Cheap read-only getters for the two pieces of global race state.

### The private members

```cpp
void run(std::stop_token st);
void tick();
void update_positions_and_drs();
void handle_pit(DriverState& state);
bool should_pit(const DriverState& state) const;
static float max_speed(const CarProfile& car);
```
Each "what happens every 20ms" concern gets its own named function instead of
one 150-line function. `max_speed` is `static` because it doesn't touch any
instance data (`states_`, `queue_`, etc.) — it's a pure function of a
`CarProfile`, so there's no reason to require a `TelemetryGenerator` object to
call it.

```cpp
SpscQueue<TelemetryFrame, 2048>& queue_;
TrackProfile                     track_;
std::vector<DriverState>         states_;
std::jthread                     thread_;
OnTickCallback                   on_tick_;
std::mt19937                     rng_{42};
std::normal_distribution<float>  dist_{0.0f, 1.0f};
int  race_lap_      {1};
bool race_finished_ {false};
```
`rng_{42}` — a **fixed seed**. This is deliberate: it makes the simulation
reproducible run-to-run (same "random" driver variation every time), which
makes tests deterministic instead of randomly flaky.

---

## 2. `telemetry_generator.cpp` — the actual logic, function by function

### Constructor

Purpose: build the initial `DriverState` for all 20 drivers.

```cpp
for (std::size_t i = 0; i < DRIVERS.size(); ++i) {
    DriverState s;
    s.profile  = DRIVERS[i];
    s.car      = car_for_driver(DRIVERS[i]);
    s.position = static_cast<int>(i + 1);
    s.distance_in_lap = static_cast<float>(DRIVERS.size() - i - 1) * 0.010f;
    s.latest_frame.driver_id = DRIVERS[i].id;
    s.latest_frame.lap       = 1;
    s.latest_frame.fuel_kg   = 100.0f;
    s.lap_start              = std::chrono::steady_clock::now();
    states_.push_back(std::move(s));
}
```
The `distance_in_lap` line staggers cars along the grid — P1 (`i=0`) starts
furthest ahead, last place (`i=19`) starts furthest back, 10 metres (`0.010`
km) apart — so on tick 1 they aren't all mathematically on top of each other.
`std::move(s)` avoids copying the whole struct into the vector a second time;
it transfers `s`'s internals directly into the new vector slot.

### `start()` / `stop()`

```cpp
void TelemetryGenerator::start() {
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}
void TelemetryGenerator::stop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}
```
`start()` launches a lambda that captures `this` (so it can call member
function `run`) and forwards the `stop_token` the `jthread` machinery hands
it. `stop()` is the cooperative-cancellation pattern: flip the flag, then
block until the thread notices and returns.

### `standings()`

```cpp
std::vector<DriverState> TelemetryGenerator::standings() const {
    return states_;
}
```
One line, but the important part is what it *doesn't* do: no lock. Discussed
in the retrospective below — this is a known simplification.

### `run()` — the thread's entire life

```cpp
void TelemetryGenerator::run(std::stop_token st) {
    while (!st.stop_requested() && !race_finished_) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
```
A tight loop: simulate one tick, sleep 20ms (50Hz), repeat, until either
someone calls `stop()` or the race naturally finishes.

### `tick()` — one simulated 20ms step for all 20 cars

This is the heart of the file. Walking through the per-car body:

1. **Pit check** — if the car is currently in the pits, delegate to
   `handle_pit()` and skip the rest of the physics for this car this tick.
2. **Speed** — starts from `max_speed(car)`, then multiplies in penalties for
   fuel weight and tire wear, adds Gaussian random "driver variation" scaled
   by `(1 - consistency)`, adds a DRS bonus if active, then clamps to a
   plausible range with `std::clamp`.
3. **Throttle/brake** — a simplified proxy: throttle scales with speed;
   brake only kicks in below 150 kph.
4. **Distance/sector** — converts `speed_kph` into km travelled this tick
   using `SIM_S_PER_TICK`, accumulates it, and works out which of the 3
   track sectors the car is currently in.
5. **Tire wear** — accumulates based on aggression, track deg factor, and
   current speed.
6. **Fuel burn** — subtracts `fuel_consumption * distance` from the tank.
7. **Lap completion** — if accumulated distance passes the track length:
   roll over into the next lap, record the lap time, decide whether to pit
   (`should_pit`), and bump the global `race_lap_`/`race_finished_` if this
   is the leader crossing the line past `TOTAL_LAPS`.
8. **Push** — `queue_.push(frame)` sends this car's frame to whoever is
   consuming the queue. The return value (`false` = queue was full) is
   currently ignored — see retrospective.

After every car is updated: `update_positions_and_drs()` re-sorts the field,
then `on_tick_(states_)` fires the registered callback, if any, with the
freshly updated standings.

### `handle_pit()`

```cpp
void TelemetryGenerator::handle_pit(DriverState& state) {
    auto& frame = state.latest_frame;
    state.pit_timer_ticks--;
    frame.in_pit    = true;
    frame.speed_kph = 60.0f; // pit lane speed limit
    frame.throttle  = 0.2f;
    frame.brake     = 0.1f;

    if (state.pit_timer_ticks <= 0) {
        state.in_pit    = false;
        frame.in_pit    = false;
        frame.tire_wear = 0.02f;
        frame.fuel_kg   = 100.0f;
        state.pit_stops++;
    }
}
```
While `pit_timer_ticks` counts down, the car is frozen at pit-lane speed. Once
it reaches zero: tires and fuel reset, and the pit-stop counter increments.

### `should_pit()`

```cpp
bool TelemetryGenerator::should_pit(const DriverState& state) const {
    if (state.has_completed_pit) return false; // one stop only for now
    if (state.latest_frame.lap < 5) return false; // no early pits
    float threshold = 0.60f + state.profile.tire_mgmt * 0.25f;
    return state.latest_frame.tire_wear >= threshold;
}
```
A driver pits once tire wear crosses a threshold that's personalized by how
gently they treat tires (`tire_mgmt`). `has_completed_pit` caps every driver
at exactly one stop for now — a deliberate simplification (real F1 strategy
allows multiple stops).

### `update_positions_and_drs()`

Two passes:
1. Build an index array `idx`, sort it by total race distance
   (`lap * track_length + distance_in_lap`) using `std::stable_sort`
   (stable, so two cars at *exactly* equal distance keep their prior relative
   order instead of swapping every tick, which would look jittery).
2. Walk the sorted order assigning `position` and computing `gap_to_leader`
   for everyone behind P1, then a second small loop flags `drs_active` for
   anyone within 1 second of the car directly ahead (P1 never gets DRS).

### `max_speed()`

```cpp
float TelemetryGenerator::max_speed(const CarProfile& car) {
    return 280.0f + car.engine_power * 90.0f;
}
```
A pure function — same input always gives the same output, no side effects —
which is exactly why it's `static`: it doesn't need a `TelemetryGenerator`
instance at all.

---

## 3. `race_director.h` — `std::latch` wrapper

```cpp
class RaceDirector {
public:
    explicit RaceDirector(std::ptrdiff_t thread_count) : latch_{thread_count} {}
    void signal_ready()   { latch_.count_down(); }
    void wait_for_start() { latch_.wait(); }
    void arrive_and_wait(){ latch_.arrive_and_wait(); }
private:
    std::latch latch_;
};
```
- `signal_ready()` — one thread says "I'm ready," decrementing the internal
  counter by one.
- `wait_for_start()` — blocks until the counter reaches zero (i.e. everyone
  has called `signal_ready()`).
- `arrive_and_wait()` — the common case, does both in one call: decrement,
  then block until zero.

This is a one-shot gate: once the count reaches 0 it never resets. Any later
call to `wait_for_start()` returns immediately.

---

## 4. `lap_barrier.h` — `std::barrier` wrapper

```cpp
class LapBarrier {
public:
    explicit LapBarrier(std::ptrdiff_t count, std::function<void()> on_completion = {})
        : barrier_{count,
            [cb = std::move(on_completion)]() noexcept { if (cb) cb(); }}
    {}
    void arrive_and_wait() { barrier_.arrive_and_wait(); }
    [[nodiscard]] auto arrive() { return barrier_.arrive(); }
private:
    std::barrier<std::function<void()>> barrier_;
};
```
- The constructor stores your callback (`on_completion`) inside a `noexcept`
  lambda before handing it to `std::barrier`. `std::barrier` requires its
  completion function to be non-throwing; wrapping it this way satisfies that
  without needing the (broken-on-this-toolchain) `std::function<void() noexcept>`
  spelling. See "gotchas" below.
- `arrive_and_wait()` — blocks the calling thread until every participant for
  this phase has also called it; once the last one arrives, the completion
  function runs once, then the barrier silently resets for the next phase.
- `arrive()` — a non-blocking variant: register that this thread arrived, but
  don't wait around. Returns an "arrival token" (marked `[[nodiscard]]`
  because ignoring it is very likely a mistake — you'd lose the only handle
  you have on this thread's arrival).

Difference from `RaceDirector`/`latch`: this one is designed to be called
again and again (once per lap), not just once.

---

## 5. `pit_lane.h` — `std::counting_semaphore` wrapper

```cpp
class PitLane {
public:
    void enter() { capacity_.acquire(); }
    bool try_enter(std::chrono::milliseconds timeout) {
        return capacity_.try_acquire_for(timeout);
    }
    void exit() { capacity_.release(); }
private:
    std::counting_semaphore<2> capacity_{2};
};
```
- `enter()` — takes one of 2 "seats"; blocks if both are taken.
- `try_enter(timeout)` — same, but gives up and returns `false` after
  `timeout` instead of blocking forever.
- `exit()` — gives a seat back, waking up one thread that was blocked in
  `enter()`, if any.

`<2>` is the compile-time maximum (a template parameter — baked in at compile
time), `{2}` is the runtime starting count. They happen to match here, but
they're conceptually different things: you could have
`std::counting_semaphore<10> sem{2}` — max capacity 10, but only 2 available
right now.

---

## 6. What each test file actually proves

- **`test_telemetry_generator.cpp`** (6 tests) — the generator produces
  frames at all; speeds stay in a believable range; tire wear trends upward
  over time; fuel trends downward; `stop()` returns quickly (proves the
  cooperative-cancellation loop actually notices the flag promptly); and all
  20 drivers get frames, not just some.
- **`test_latch.cpp`** (3 tests) — 4 threads released within 2ms of each
  other (proves the "everyone waits, then everyone goes" behaviour); a latch
  that's already open lets a late `wait_for_start()` return instantly (proves
  the one-shot/no-reset behaviour); degenerate single-thread case.
- **`test_barrier.cpp`** (2 tests) — the completion function runs exactly
  once per phase across 10 phases (proves it resets correctly each time);
  no thread proceeds past `arrive_and_wait()` until every participant for
  that phase has arrived (proves it's actually a barrier, not a no-op).
- **`test_semaphore.cpp`** (2 tests) — with 10 threads hammering a
  capacity-2 semaphore, at most 2 are ever inside at once (proves the cap is
  enforced); a third caller to `try_enter` with a short timeout returns
  `false` when both seats are already taken (proves the timing-out variant
  actually times out instead of blocking forever).

All 4 files link into **one** `phase3_tests` executable (see
`tests/CMakeLists.txt`'s `add_phase_tests`), and rely on `GTest::gtest_main`
to supply `main()` — none of the test files define their own, which matters:
if two files in the same executable both defined `int main()`, that's a
duplicate-symbol link error.

---

## 7. Build system changes

- `CMakeLists.txt` (root): added `-fexperimental-library` for Clang only —
  needed to unlock `std::jthread`/`std::stop_token` on Apple's libc++.
- `src/CMakeLists.txt`: added `simulation/telemetry_generator.cpp` to the
  `rcz_lib` static library's source list, so it actually gets compiled and
  linked into both the main executable and the test executables.
- `tests/CMakeLists.txt`: uncommented the `add_phase_tests(phase3 ...)`
  block, registering the 4 new test files as the `phase3_tests` target and
  the `phase3` ctest name.

---

## 8. Real gotchas I hit while building this

These are worth remembering because they came from actually test-compiling
things on this machine, not from copying working code — they're the kind of
detail that's very believable and interesting in an interview.

1. **`std::jthread`/`std::stop_token` fail to compile out of the box on
   Apple Clang.** Error: `no member named 'jthread' in namespace 'std'`.
   Cause: Apple's libc++ ships the implementation but gates it behind
   `-fexperimental-library` because they don't consider it stable yet. Fixed
   by adding that flag (Clang-only, via `CMAKE_CXX_COMPILER_ID MATCHES "Clang"`).
2. **`std::barrier<std::function<void() noexcept>>` fails to compile.**
   Error: `implicit instantiation of undefined template 'std::function<void () noexcept>'`.
   `std::function` on this library isn't specialized for noexcept function
   signatures. Fixed by storing a plain `std::function<void()>` and
   satisfying the "must not throw" requirement with a `noexcept` lambda
   wrapper instead of encoding it in the type.
3. **Template instantiation mismatch.** A test tried to construct a
   `TelemetryGenerator` with a `SpscQueue<TelemetryFrame, 4096>`, but the
   constructor only accepts `SpscQueue<TelemetryFrame, 2048>&`. Compile error
   (not a runtime bug!) because `SpscQueue<T, 2048>` and `SpscQueue<T, 4096>`
   are two unrelated types generated from the same template — like `int` vs
   `double`. Fixed by matching the capacity.

Lesson for myself: when unsure whether a standard-library feature actually
works on this toolchain, write a 5-line throwaway file and compile it first,
rather than debugging a mystery error buried inside 200 real lines of logic.

---

## 9. Design decisions — what I'd defend, and what I'd fix

### Things I think are solid

- **Cooperative cancellation via `stop_token` instead of a hand-rolled
  atomic bool.** Standard, well-understood, and the destructor safety net
  (`jthread` auto-joins) removes a whole class of "forgot to join" bugs.
- **`standings()` returns a copy, not a reference.** Simple, and it avoids
  handing external code a reference into memory a background thread might
  still write to.
- **Callback-based tick notification (`OnTickCallback`).** The generator has
  zero knowledge of what a leaderboard or UI even is — whoever wants tick
  updates registers a callback. Easy to add a second listener later without
  touching this class.
- **Fixed RNG seed (`rng_{42}`).** Makes "random" driver variation
  reproducible, so tests that check trends (tire wear increasing, fuel
  decreasing) aren't flaky by chance.
- **Named constants at the top of the header** (`TIME_SCALE`, `TICK_S`,
  `TIRE_WEAR_BASE`, ...) instead of magic numbers buried in `tick()`. Easy to
  retune the simulation's pace without hunting through logic.
- **Small, isolated, individually-tested wrapper classes for each sync
  primitive** (`RaceDirector`, `LapBarrier`, `PitLane`) rather than
  learning/debugging `std::latch` for the first time inside the much bigger
  `TelemetryGenerator`. Easier to reason about, easier to test, easier to
  explain one concept at a time.

### Things I'd flag as weaknesses if an interviewer pushed on them

- **`standings() const { return states_; }` is technically a data race** if
  called from another thread while the generator is running. Copying a
  `std::vector` while another thread concurrently mutates it (resizing, in
  particular, could reallocate) is undefined behaviour by the strict C++
  memory model, even though it "worked" in practice in these tests (which
  only call `standings()` after `stop()` has already joined the thread — so
  there's no *actual* concurrent access exercised yet). The honest fix:
  either wrap `states_` access in a small `std::mutex`, or maintain a
  separately-published atomic snapshot (e.g.
  `std::atomic<std::shared_ptr<const std::vector<DriverState>>>`) that the
  generator swaps once per tick, so readers never touch the live vector.
- **Dropped frames are silent.** `queue_.push(frame)`'s `bool` return value
  (false = queue was full, frame dropped) is discarded. There's no counter or
  log, so if the consumer ever falls behind, frames vanish with zero
  visibility. A `dropped_frame_count_` atomic counter, exposed via a getter,
  would be a small, cheap fix.
- **`RaceDirector`, `LapBarrier`, and `PitLane` aren't actually used by the
  simulation yet.** They're correct, tested, and ready — but `tick()` still
  updates all 20 cars sequentially in a single loop on one thread. This is
  an honest, deliberate simplification for this phase (I said so directly in
  `lap_barrier.h`'s comments), but if asked "where do you use a barrier in
  this project," today's honest answer is "not yet — it's built and tested,
  waiting for the phase where per-driver work gets split across threads."
- **Magic numbers still exist in a few spots** — e.g. `60.0f` pit lane speed
  in `handle_pit()`, `0.60f + tire_mgmt * 0.25f` in `should_pit()`. Smaller
  than the header constants, but could still be pulled into named constants
  for discoverability/tuning.
- **Timing-based tests are inherently a little fragile.** Several tests
  `sleep_for(200ms)` and assume "~10 ticks happened." On a heavily loaded CI
  machine this margin could theoretically be tight. A more deterministic
  design would let a test drive `tick()` directly N times without a real
  background thread and real sleeps — something to consider revisiting if
  these tests ever get flaky in practice.
- **One-stop-only pit strategy (`has_completed_pit`) is a real simplification**,
  not a bug — worth being upfront that real F1 strategy allows multiple
  stops, and this project models a simplified single-stop strategy for now.

---

## 10. Quiz yourself — likely interview questions

**Q: Why `jthread` instead of `thread`?**
A: Auto-joins in its destructor (no `std::terminate` from a forgotten
`join()`), and it carries a `std::stop_token` for built-in cooperative
cancellation instead of a hand-rolled atomic flag.

**Q: Can `request_stop()` forcibly kill a thread?**
A: No — it only flips a flag. The thread's own code must check
`stop_requested()` and choose to return. C++ has no safe way to force-kill a
running thread.

**Q: Difference between `std::latch` and `std::barrier`?**
A: Both block N threads until all N arrive. A latch is one-shot — it can't
be reused once it opens. A barrier auto-resets after releasing everyone, so
it can be reused phase after phase, and can run a completion function once
per phase.

**Q: When would you reach for a counting semaphore instead of a mutex?**
A: When you have N interchangeable resource instances, not one exclusive
resource — a mutex is really just a `counting_semaphore<1>`.

**Q: Is `TelemetryGenerator::standings()` actually thread-safe?**
A: Only in the narrow sense that it returns a copy rather than a reference.
If called concurrently with the generator thread actively mutating
`states_`, it's technically a data race under the C++ memory model. The
current tests never exercise that concurrent path (they call `standings()`
after `stop()`), so it "works" today but would need a mutex or an atomic
snapshot to be genuinely safe under real concurrent use.

**Q: Why is `queue_` a reference member instead of a value or pointer?**
A: The generator doesn't own the queue's lifetime — `main` does, and
producer/consumer need to share the exact same instance. A reference
documents "used, not owned, never null," which a raw pointer wouldn't
guarantee.

**Q: What would you change first if you kept working on this?**
A: Make `standings()` genuinely thread-safe (mutex or atomic snapshot), and
add visibility into dropped frames instead of silently discarding them on a
full queue.

---

## 11. Building and testing this phase

```bash
cmake --build build
ctest --test-dir build -R phase3 --output-on-failure

# ThreadSanitizer catches data races normal test runs can miss
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan --target phase3_tests
./build-tsan/tests/phase3_tests
```

Result: all 13 phase3 tests pass, both normally and under TSan (0 warnings),
and the full suite (phase1 + phase2 + phase3 = all tests in the project)
still passes after these changes — no regressions.