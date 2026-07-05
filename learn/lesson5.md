# Lesson 5 — The Shared State Layer

My own reference for Phase 5: what every file does, function by function, the
one subtle correctness gap I found by reasoning (not by a failing test), and
an honest retrospective — written so I can explain this to someone else (or
defend it in an interview) without hand-waving.

---

## 0. The big picture

Phases 3 and 4 built things that run *alongside* each other. Phase 5 builds
the two data structures that would sit *between* the simulation (the writer)
and a future UI (the reader) — the bridge every earlier phase has been
setting up for. Two files, both header-only (no `.cpp` needed — everything
here is small enough to live entirely as inline member functions):

```
src/common/leaderboard.h   — SWMR-protected sorted standings
src/common/race_state.h    — hand-picked atomic memory orders for race flags
tests/phase5/test_leaderboard.cpp
tests/phase5/test_race_state.cpp
```

This phase is the densest in *concepts* despite being the smallest in file
count — the plan doc that guided it says outright that this is "the hardest
concepts to get right and the most likely to come up in quant firm
interviews," and I agree after actually building it.

---

## 1. `leaderboard.h` — SWMR standings, one step up from Phase 4's weather

### Purpose
Hold the current sorted race standings (20 `DriverState`s) so a writer thread
(the simulation) and reader threads (a future UI, status queries) can share
them safely — same `std::shared_mutex` idea as Phase 4's `WeatherSystem`, but
protecting a `vector` of structs instead of one enum.

### The class

```cpp
class Leaderboard {
public:
    void update(std::vector<DriverState> sorted_standings) {
        std::unique_lock lock{mutex_};
        standings_ = std::move(sorted_standings);
    }
    std::vector<DriverState> snapshot() const {
        std::shared_lock lock{mutex_};
        return standings_;
    }
    std::size_t size() const { std::shared_lock lock{mutex_}; return standings_.size(); }
    DriverState at_position(int pos) const { ... }
    std::string fastest_lap_holder() const { ... }
private:
    std::vector<DriverState>  standings_;
    mutable std::shared_mutex mutex_;
};
```

Function by function:

- **`update(sorted_standings)`** — takes the vector **by value**, then
  `std::move`s it into the member under an exclusive (`unique_lock`) write
  lock. Taking it by value lets the caller either copy in (if they still
  need their own copy) or move in (if they don't) — the function itself
  doesn't have to guess, it just always moves *its own* parameter, which is
  cheap either way.
- **`snapshot() const`** — the main read path. Takes a `shared_lock` (many
  readers can hold this simultaneously) and returns a full **copy** of the
  vector. Same idiom as Phase 3's `TelemetryGenerator::standings()` and
  Phase 4's `WeatherSystem::current()`: never hand out a reference to
  memory another thread might still be writing, hand out an independent
  copy instead.
- **`size() const`** — trivial shared-lock read.
- **`at_position(pos) const`** — linear scan under a shared lock; returns a
  default-constructed `DriverState{}` (position `0`) if nothing matches,
  which is how the test tells "found" from "not found" without a
  `std::optional`.
- **`fastest_lap_holder() const`** — scans all 20 drivers under a shared
  lock, tracks the minimum `best_lap_ms` seen, and returns that driver's id
  (or an empty string if nobody has set a lap time yet).

**A real gap I found and fixed before writing this file:** `fastest_lap_holder()`
uses `std::numeric_limits<float>::max()`, which needs `#include <limits>`.
The original design only listed `"common/types.h"`, `<shared_mutex>`, and
`<vector>` as includes. I test-compiled that exact combination first — it
happened to compile anyway, because `<limits>` gets pulled in transitively
through something `<shared_mutex>` includes on this toolchain. That's not
guaranteed by the C++ standard, though — a different standard library
(or a future version of this one) could drop that transitive include and
silently break the build. I added `#include <limits>` explicitly rather than
depend on an implementation detail — the general rule is "include what you
use," not "include what happens to already be there."

---

## 2. `race_state.h` — memory ordering, made explicit everywhere

### Why this file is different from everything before it

Every previous atomic in this project (Phase 3's stop tokens, Phase 4's CAS
state machine) used specific memory orders, but this file is the one that
stops and documents, field by field, *why* each choice is correct — because
picking the wrong (too-weak) order is a real, production-relevant class of
bug, and picking the safe-but-slow order (`seq_cst`) everywhere is a real,
production-relevant performance cost. The header comment itself is worth
re-reading slowly:

```
memory_order_relaxed — no synchronisation; just atomicity.
memory_order_release — on a STORE: publish everything written before it.
memory_order_acquire — on a LOAD: see everything published by a matching release.
memory_order_acq_rel — on a read-modify-write: both at once.
memory_order_seq_cst — total global order across ALL atomics; safest, priciest.
```

### Field by field

- **`race_active` (bool)** — `start_race()`/`end_race()` store with
  `release`; `is_active()` loads with `acquire`. Reasoning: when the race
  starts, there's presumably other setup state written just before flipping
  this flag, and any thread that observes `race_active == true` via an
  acquire-load should also see that setup — release/acquire is exactly the
  pairing that guarantees that.

- **`current_lap` (int)** — `set_lap()`/`get_lap()` both use `relaxed`.
  Reasoning: this is a display-only counter. If a reader sees lap 41 instead
  of 42 for one frame, nothing breaks — there's no *other* data whose
  visibility depends on seeing the exact right lap number at the exact right
  moment. This is the textbook case for `relaxed`: correctness doesn't
  depend on ordering, only on the read/write itself being atomic (so you
  never see a half-written garbage int).

- **`safety_car` (bool)** — same release/acquire pairing as `race_active`,
  described as "atomic broadcast: one writer, many readers, no locks." This
  is the same shape as Phase 4's weather flag, but done with a raw atomic
  instead of a `shared_mutex` — appropriate here because it's a single bool,
  not a whole struct; a mutex would be overkill for one word.

- **`fastest_lap_holder` (int) + `fastest_lap_ms` (float)** — the
  interesting one. `try_claim_fastest_lap()`:

  ```cpp
  bool try_claim_fastest_lap(int driver_index, float lap_ms) {
      float current_best = fastest_lap_ms.load(std::memory_order_acquire);
      while (lap_ms < current_best) {
          if (fastest_lap_ms.compare_exchange_weak(
                  current_best, lap_ms,
                  std::memory_order_acq_rel,
                  std::memory_order_acquire))
          {
              fastest_lap_holder.store(driver_index, std::memory_order_release);
              return true;
          }
          // current_best was overwritten with the actual value; loop and recheck.
      }
      return false;
  }
  ```

  This is the classic **CAS retry loop**: load the current best, and while
  our candidate is still better than what we last saw, try to swap it in.
  If another thread beat us to it between our load and our CAS,
  `compare_exchange_weak` fails *and* overwrites `current_best` with
  whatever the real current value now is — so the `while` condition
  re-checks against fresh data instead of stale data. If our lap is no
  longer the best by that point, the loop exits naturally and we return
  `false`. This is why the parameter is named `current_best` and reused
  across iterations rather than re-declared.

  **Why `compare_exchange_weak` here, but `compare_exchange_strong` in
  Phase 4's `PenaltyEnforcer`?** `_weak` is allowed to fail "spuriously" —
  return false even when the comparison *would* have succeeded — which is
  cheaper on some hardware (notably ARM's load-linked/store-conditional
  instructions) but only safe to use **inside a retry loop**, since a
  spurious failure just means "try again." `PenaltyEnforcer`'s state
  transitions each only attempt the swap **once** (no loop) — a spurious
  failure there would incorrectly report "someone else already issued the
  penalty" when actually nobody had. `_strong` never fails spuriously, which
  is the correct (if very slightly more expensive) choice for a single-shot
  attempt.

### A real correctness gap I found by reasoning through it (not by a failing test)

`fastest_lap_holder` and `fastest_lap_ms` are two **separate** atomics that
together represent one logical fact ("who currently holds the fastest lap,
and what is it"). `try_claim_fastest_lap()` updates them in two separate
steps: first the CAS on `fastest_lap_ms` succeeds, *then* (a moment later)
`fastest_lap_holder.store(...)` runs. Between those two lines, there is a
real window where another thread calling `get_fastest_lap_holder()` and
`get_fastest_lap_ms()` could read the **new** time paired with the **old**
holder — a driver who doesn't actually own that lap time, however briefly.

This is not a data race — ThreadSanitizer wouldn't (and didn't) flag
anything, because every individual load and store is still correctly
atomic and ordered. It's a **logical consistency gap across two related
atomics that aren't updated as a single transaction**. TSan proves the
absence of data races; it does not prove the absence of this kind of
cross-variable inconsistency. I didn't hit this via a failing test — the
window is narrow enough that reproducing it reliably in a unit test would be
its own small project — I found it by reading the two-step update and asking
"what could a concurrent reader observe *between* these two lines?"

The real fix, if this mattered for correctness rather than just a UI
display value: pack both fields into one atomic (e.g. a small struct holding
`{int holder; float ms;}` wrapped in `std::atomic<T>` if it's trivially
copyable and small enough to be lock-free, or protect both with one mutex,
or use a single 64-bit atomic with the two 32-bit halves bit-packed
manually). For a value that only ever feeds a "fastest lap" badge in a UI,
this project's current behavior — a very occasional one-frame flash of a
mismatched holder/time pair — is a reasonable, deliberate trade-off, not an
oversight I'd necessarily "fix" without knowing how much it actually
mattered to the product.

---

## 3. The global singleton: `extern RaceState g_race_state;`

The header declares (but, in this repository, does not yet define anywhere
outside the test file):

```cpp
extern RaceState g_race_state;
```

`extern` tells every translation unit that includes this header "this
variable exists somewhere; don't allocate storage for it here, just link
against whichever `.cpp` actually defines it." Exactly one `.cpp` in the
final program must provide the matching definition (`RaceState g_race_state;`
with no `extern`), or the linker fails with an "undefined symbol" error.

**Deliberate choice: I did not add that definition to `main.cpp`.** Same
reasoning as Phase 3's `RaceDirector`/`LapBarrier`/`PitLane` and Phase 4's
three race-control systems: this is a correct, fully-tested, **not yet
wired into the live simulation** component. `tests/phase5/test_race_state.cpp`
provides its own definition (`RaceState g_race_state;` at file scope) because
test binaries don't link `main.cpp` — they build their own executable out of
the test file plus the shared library — so each test binary needs its own
copy of anything declared `extern` if that binary is ever going to
odr-use it.

---

## 4. Design decisions — what I'd defend, and what I'd fix

### Things I think are solid

- **Every atomic operation in `race_state.h` has an explicit, individually
  justified memory order**, rather than leaving everything at the default
  `seq_cst` or, worse, guessing. This is genuinely rare even in real
  production code, and it's exactly the kind of detail that separates
  "I used atomics" from "I understand what atomics guarantee."
- **The weakest-correct-order principle is followed consistently**: `relaxed`
  for the display-only lap counter (no other data depends on its ordering),
  `release`/`acquire` for flags that gate visibility of other state, `acq_rel`
  for the one read-modify-write. Nothing is over-synchronized "just in case."
- **`compare_exchange_weak` in a retry loop vs. `compare_exchange_strong` for
  a single-shot check** (contrasting `race_state.h` against Phase 4's
  `penalty_enforcer.cpp`) — the right primitive was picked for each shape of
  problem, not the same one reused everywhere out of habit.
- **Consistent "return a copy, never a reference" idiom** across
  `Leaderboard::snapshot()`, Phase 4's `WeatherSystem::current()`, and Phase
  3's `TelemetryGenerator::standings()` — the same safe pattern reused
  deliberately rather than reinvented per class.
- **Storing `fastest_lap_holder` as an `int` index rather than a
  `std::string`.** `std::string` can't live inside `std::atomic` at all (it's
  not trivially copyable — it may own a heap allocation), so this wasn't
  just a style choice, it was close to the only option that keeps this a
  genuine lock-free atomic instead of requiring a mutex.

### Things I'd flag as weaknesses if an interviewer pushed on them

- **The `fastest_lap_holder`/`fastest_lap_ms` split-update window**, covered
  in detail above — two atomics representing one fact, updated in two
  separate non-atomic-as-a-pair steps.
- **`fastest_lap_holder` is "just an int"** with no type-level connection to
  `DRIVERS` — nothing stops passing an out-of-range index, and reading it
  requires the caller to already know it's meant to index into `DRIVERS`
  elsewhere. A small wrapper type or a bounds-checked accessor would make the
  contract explicit instead of implicit-by-convention.
- **`Leaderboard::at_position()` is an O(n) linear scan under a shared lock**
  — completely fine at 20 drivers (same as Phase 4's `driver_index()`), but
  the same "would need an index/map at real scale" caveat applies.
- **Global mutable singleton (`g_race_state`).** Globals are easy — no need
  to thread a reference through every function that might care about race
  state — but they're also a classic testability and coupling smell: any
  code that touches `g_race_state` has a hidden dependency invisible in its
  function signature, and you can't easily run two independent race
  simulations in the same process (there's only one global). For a
  single-race, single-binary simulator this is a reasonable trade-off; for
  anything bigger it's the first thing I'd refactor toward dependency
  injection.
- **None of Phase 5's structures are wired into the live simulation loop
  yet** — same honest caveat as every prior phase's new components. They're
  correct and tested in isolation; "who actually calls `Leaderboard::update()`
  every lap" is still an open question for the phase that does the final
  integration.

---

## 5. Quiz yourself — likely interview questions

**Q: Why `relaxed` for the lap counter but `release`/`acquire` for the
safety car flag?**
A: The lap counter is purely informational — no other data's visibility
depends on exactly when a reader sees the new lap number, so atomicity alone
(no reordering guarantees) is sufficient. The safety car flag is a signal
that something else changed alongside it (or is expected to gate other
reasoning) — release/acquire guarantees that whatever the writer did before
setting the flag is visible to anyone who observes the flag becoming true.

**Q: What's the actual difference between `compare_exchange_weak` and
`compare_exchange_strong`?**
A: `_weak` may fail even when the comparison would have succeeded (a
"spurious failure"), which is cheaper on architectures using
load-linked/store-conditional instructions (like ARM). That's only safe to
use inside a retry loop, where a spurious failure just means "try again."
`_strong` never fails spuriously — required when you only get one attempt
and a false negative would be wrong, not just wasteful.

**Q: Why can't `std::string` be stored in a lock-free `std::atomic`?**
A: `std::atomic<T>` requires `T` to be trivially copyable (and small enough,
in practice, to be lock-free on real hardware). `std::string` typically owns
a heap allocation and has a non-trivial copy constructor/destructor, so it
fails that requirement — `std::atomic<std::string>` would need internal
locking to work at all, defeating the point.

**Q: Walk me through a subtle bug (not a data race) you found in this code.**
A: `fastest_lap_holder` and `fastest_lap_ms` are two separate atomics
representing one logical fact, updated in two separate steps inside
`try_claim_fastest_lap()`. A concurrent reader can observe the new time
paired with the old holder in the brief window between the two stores.
ThreadSanitizer doesn't catch this because every individual access is still
correctly atomic and ordered — it's a cross-variable consistency gap, not a
race on either variable alone.

**Q: Why not just use `memory_order_seq_cst` everywhere and not worry about
it?**
A: `seq_cst` is the safest and the default if you specify nothing, but it's
also the most expensive — it forces a full memory fence (e.g. `MFENCE` on
x86, and something costlier on ARM) on every access. In a 50Hz loop touching
20 drivers' worth of atomics per tick, that overhead is real. Production
low-latency systems use the weakest memory order that is still provably
correct for each specific variable, and document why — which is the whole
point of this file.

**Q: Why does `Leaderboard::update()` take its parameter by value instead of
`const&`?**
A: It always needs to end up moved into the member (`standings_ =
std::move(sorted_standings)`), regardless of whether the caller passes an
lvalue or an rvalue. Taking by value lets the compiler pick the cheapest
path for the caller (move if they pass a temporary, one copy if they pass a
named vector they still need afterward) while the function itself always
just moves its own local parameter — no overload duplication needed.

---

## 6. Building and testing this phase

```bash
cmake --build build
ctest --test-dir build -R phase5 --output-on-failure

# ThreadSanitizer — especially important for the SWMR and atomic tests
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan --target phase5_tests
./build-tsan/tests/phase5_tests
```

Result: all 9 phase5 tests passed on the first run, both normally and under
TSan (0 warnings) — no bugs needed fixing this phase (unlike Phase 4's
self-consumption bug). The full suite (phase1 through phase5) still passes
with no regressions.