# PitWall — Implementation Plan

**Goal:** Master C++20 concurrency by building a real-time F1 race telemetry engine from scratch.  
**UI:** FTXUI terminal dashboard (multi-panel, live-updating)...Functional Terminal (X) User Interface 
**Testing:** GoogleTest unit tests gate every phase. Do not move to the next phase until all tests pass.

---

## Directory Structure

```
pitwall/
├── CMakeLists.txt
├── IMPLEMENTATION_PLAN.md
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
│   │   └── sync_primitives.hpp
│   ├── simulation/
│   │   ├── telemetry_generator.hpp
│   │   ├── telemetry_generator.cpp
│   │   ├── race_director.hpp
│   │   └── pit_lane.hpp
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
    ├── phase1/
    │   └── test_types.cpp
    ├── phase2/
    │   ├── test_spsc_queue.cpp
    │   ├── test_mpsc_queue.cpp
    │   └── test_thread_pool.cpp
    ├── phase3/
    │   ├── test_telemetry_generator.cpp
    │   ├── test_latch.cpp
    │   ├── test_barrier.cpp
    │   └── test_semaphore.cpp
    ├── phase4/
    │   ├── test_track_limits.cpp
    │   ├── test_penalty_enforcer.cpp
    │   ├── test_weather.cpp
    │   └── test_strategy_analyzer.cpp
    ├── phase5/
    │   ├── test_leaderboard.cpp
    │   └── test_race_state.cpp
    └── phase6/
        └── test_dashboard_integration.cpp
```

---

## Concurrency Concepts Map

| Phase | Concept | C++ Feature | Where it appears |
|-------|---------|-------------|-----------------|
| 2 | Lock-free SPSC queue | `std::atomic`, `memory_order_acquire/release` | Telemetry pipeline |
| 2 | Lock-free MPSC queue | `compare_exchange_weak`, CAS loop | Race control event bus |
| 2 | Thread pool | `std::mutex`, `std::condition_variable` | Strategy, weather |
| 3 | Modern threading | `std::jthread`, `std::stop_token` | All threads |
| 3 | One-shot barrier | `std::latch` | Race start sync |
| 3 | Reusable barrier | `std::barrier` | End-of-lap sync |
| 3 | Resource limiting | `std::counting_semaphore` | Pit lane capacity |
| 4 | Atomic state machine | `compare_exchange_strong` | Penalty enforcer |
| 4 | Atomic broadcast | `std::atomic<bool>` with `memory_order_release` | Safety car flag |
| 5 | Single-writer multiple-reader | `std::shared_mutex` | Leaderboard state |
| 5 | Explicit memory ordering | `memory_order_acquire/release/seq_cst` | Race flags |

---

## Phase 1 — Foundation

**Goal:** Project scaffold, data types, and season data. No concurrency yet.

### Step 1 — CMake project scaffold

Create `CMakeLists.txt` at the project root.

```cmake
cmake_minimum_required(VERSION 3.20)
project(pitwall CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# Fetch FTXUI
include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG        v5.0.0
)
FetchContent_MakeAvailable(ftxui)

# Fetch GoogleTest
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest
  GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

add_subdirectory(src)
add_subdirectory(tests)
```

**Verify:** `cmake -B build && cmake --build build` compiles with no errors.

---

### Step 2 — Core types (`src/common/types.hpp`)

Define all structs used throughout the project.

```cpp
// Key structs to define:
struct TelemetryFrame {
    std::string driver_id;
    int         lap;
    int         sector;        // 1, 2, or 3
    float       speed_kph;
    float       throttle;      // 0.0 - 1.0
    float       brake;         // 0.0 - 1.0
    float       tire_wear;     // 0.0 (new) - 1.0 (destroyed)
    float       fuel_kg;
    bool        drs_active;
    bool        in_pit;
    float       gap_to_leader; // seconds
    std::chrono::milliseconds lap_time;
};

struct DriverProfile {
    std::string id;
    std::string name;
    std::string team;
    std::string color;         // ANSI color code
    float       aggression;    // 0.0 - 1.0
    float       consistency;   // 0.0 - 1.0
    float       tire_mgmt;     // 0.0 - 1.0
    float       risk_tolerance;// 0.0 - 1.0
};

struct CarProfile {
    std::string team;
    float       engine_power;  // 0.0 - 1.0
    float       aero_efficiency;
    float       cooling;
    float       reliability;
};

struct TrackProfile {
    std::string name;
    float       length_km;
    int         sectors;
    float       tire_deg_factor;
    float       fuel_consumption; // kg/km
};

enum class PenaltyState { NONE, PENDING, SERVING, SERVED };

struct RaceControlEvent {
    enum class Type {
        TRACK_LIMITS, PENALTY_ISSUED, PIT_ENTRY, PIT_EXIT,
        WEATHER_CHANGE, SAFETY_CAR, FASTEST_LAP, RADIO_MESSAGE
    };
    Type        type;
    std::string driver_id;
    int         lap;
    std::string message;
    std::chrono::steady_clock::time_point timestamp;
};

struct DriverState {
    DriverProfile       profile;
    CarProfile          car;
    TelemetryFrame      latest_frame;
    PenaltyState        penalty_state{PenaltyState::NONE};
    int                 penalty_warnings{0};
    int                 position{0};
    int                 pit_stops{0};
    float               best_lap_time{0.0f};
};
```

---

### Step 3 — Season data (`src/common/season_data.hpp`)

Hardcode the 2025 F1 grid. 20 drivers across 10 teams with realistic personality traits. Includes a default `TrackProfile` for a generic circuit.

**Verify:** Include the header in `main.cpp` and print all driver names to stdout.

---

### Phase 1 Tests (`tests/phase1/test_types.cpp`)

**Testing tool:** GoogleTest  
**Run:** `cmake --build build && ./build/tests/phase1/phase1_tests`

```
What to test:
- TelemetryFrame default-initializes to safe values (no garbage floats)
- DriverProfile fields are within valid ranges after construction
- RaceControlEvent timestamp is set correctly on creation
- DriverState starts with position=0, pit_stops=0, penalty=NONE
- All 20 drivers in season_data have unique IDs
- All 10 teams in season_data have unique names
- No driver profile has aggression or consistency outside [0.0, 1.0]
```

**Gate:** All Phase 1 tests green before starting Phase 2.

---

## Phase 2 — Concurrency Primitives

**Goal:** Build the three reusable concurrency data structures that the entire project sits on. These are the most important files in the codebase. Take your time here.

---

### Step 4 — Lock-free SPSC queue (`src/concurrency/spsc_queue.hpp`)

**Concept:** `std::atomic`, `memory_order_acquire`, `memory_order_release`, false sharing prevention.

Single-producer single-consumer bounded ring buffer. Used for the telemetry pipeline (generator thread → display thread). The key insight is that with exactly one producer and one consumer, you only need two atomic indices — no locks required.

```cpp
template <typename T, std::size_t Capacity>
class SpscQueue {
    // head_ and tail_ on SEPARATE cache lines to prevent false sharing.
    // False sharing: two threads writing to different variables on the same
    // 64-byte cache line causes the CPU to bounce the line between cores,
    // destroying performance. alignas(64) forces each onto its own line.

    alignas(64) std::atomic<std::size_t> head_{0}; // written by consumer
    alignas(64) std::atomic<std::size_t> tail_{0}; // written by producer
    std::array<T, Capacity> buffer_;

public:
    // Producer calls this. Returns false if queue is full.
    bool push(const T& item) {
        auto tail = tail_.load(memory_order_relaxed);
        auto next = (tail + 1) % Capacity;
        // Acquire: see all writes the consumer made to head_ before
        // it incremented it (i.e., the slot is now free to write).
        if (next == head_.load(memory_order_acquire)) return false; // full
        buffer_[tail] = item;
        // Release: make the written item visible to the consumer
        // before we update tail_.
        tail_.store(next, memory_order_release);
        return true;
    }

    // Consumer calls this. Returns false if queue is empty.
    bool pop(T& item) {
        auto head = head_.load(memory_order_relaxed);
        // Acquire: see the item the producer wrote before updating tail_.
        if (head == tail_.load(memory_order_acquire)) return false; // empty
        item = buffer_[head];
        head_.store((head + 1) % Capacity, memory_order_release);
        return true;
    }
};
```

**Why these memory orders matter:**  
- `memory_order_release` on `store`: all writes before the store are visible to any thread that subsequently does an `acquire` load of the same atomic.  
- `memory_order_acquire` on `load`: all writes that happened before the matching `release` store are now visible to this thread.  
- Together they form a happens-before relationship: producer's item write happens-before consumer's item read.

---

### Step 5 — Lock-free MPSC queue (`src/concurrency/mpsc_queue.hpp`)

**Concept:** `compare_exchange_weak`, CAS (compare-and-swap) loop, ABA problem awareness.

Multiple producers (track limits, weather, safety car) push race-control events. One consumer (race control panel) drains them. Uses a lock-free linked list where the head pointer is updated with CAS.

```cpp
template <typename T>
class MpscQueue {
    struct Node {
        T value;
        std::atomic<Node*> next{nullptr};
    };

    // stub_ is a sentinel node; head_ always points to the last node pushed.
    // This design allows producers to push without a lock by atomically
    // swapping head_ and linking the previous head as next.
    std::atomic<Node*> head_;
    Node* tail_; // only accessed by the single consumer — no atomic needed

public:
    void push(T value);  // called by any thread
    bool pop(T& out);    // called only by the consumer thread
};
```

**ABA problem note:** ABA happens when a pointer is read as A, changes to B, then back to A before your CAS fires — making the CAS succeed incorrectly. The MPSC linked-list design avoids ABA because nodes are never reused (each push allocates a new node). Document this in a comment.

---

### Step 6 — Thread pool (`src/concurrency/thread_pool.hpp`)

**Concept:** `std::mutex`, `std::condition_variable`, RAII thread ownership, graceful shutdown.

Fixed number of worker threads sharing a task queue. Workers block on a condition variable when idle, wake when a task is submitted. Destructor joins all threads cleanly.

```cpp
class ThreadPool {
    std::vector<std::thread>          workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    bool                              stop_{false};

public:
    explicit ThreadPool(std::size_t num_threads);
    ~ThreadPool(); // signals stop, notifies all, joins all

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;
};
```

Key points to implement:
- `submit()` wraps the callable in a `std::packaged_task`, pushes to the queue, and notifies one worker.
- Workers loop: lock → check stop+empty → wait if empty → pop task → unlock → execute.
- Destructor sets `stop_ = true`, calls `cv_.notify_all()`, then joins every thread.

---

### Phase 2 Tests (`tests/phase2/`)

**Run:** `./build/tests/phase2/phase2_tests`

#### SPSC Queue tests (`test_spsc_queue.cpp`)
```
- push() returns true on empty queue, false when full
- pop() returns false on empty queue, true after push
- Single-threaded: push N items, pop N items, verify order preserved (FIFO)
- Concurrent stress test: producer thread pushes 100,000 items;
  consumer thread pops all; verify zero items lost, correct order
- Verify no data race: run under ThreadSanitizer (TSan)
  Build flag: -fsanitize=thread
- False sharing test: measure throughput with and without alignas(64);
  document the difference
```

#### MPSC Queue tests (`test_mpsc_queue.cpp`)
```
- Single producer, single consumer: push 1000 events, pop all, verify none lost
- Multi-producer stress test: 4 producer threads each push 10,000 events;
  single consumer pops all; verify total count = 40,000
- pop() returns false on empty queue
- Events are popped in push order per producer (not globally ordered — document why)
- Run under TSan: zero data races
```

#### Thread pool tests (`test_thread_pool.cpp`)
```
- submit() returns a valid std::future
- future.get() returns the correct result of the submitted callable
- Submit 1000 tasks; verify all complete
- Submit tasks that increment a shared atomic counter; verify final count correct
- Destructor test: pool goes out of scope with pending tasks; verify clean shutdown
  (no hang, no crash, no leaked threads)
- Exception propagation: submitted task throws; future.get() rethrows it
```

**Gate:** All Phase 2 tests green, zero TSan warnings before starting Phase 3.

---

## Phase 3 — Simulation Engine

**Goal:** Build the producer side of the system using C++20 threading primitives.

---

### Step 7 — Telemetry generator (`src/simulation/telemetry_generator.hpp/.cpp`)

**Concept:** `std::jthread`, `std::stop_token` — modern RAII threading.

Runs at 50 Hz (sleeps 20 ms per tick). Maintains 20 `DriverState` objects. Each tick it computes speed, tire wear, fuel burn, DRS eligibility, and lap progression, then pushes a `TelemetryFrame` per driver into the SPSC queue.

`std::jthread` vs `std::thread`:
- `jthread` automatically joins on destruction — no manual `join()` or detach needed.
- `stop_token` is a cooperative cancellation mechanism. The thread checks `stop_token.stop_requested()` each iteration. The owner calls `jthread::request_stop()` to signal shutdown.

```cpp
class TelemetryGenerator {
    SpscQueue<TelemetryFrame, 1024>& queue_;
    std::vector<DriverState>         states_;
    std::jthread                     thread_;

public:
    void start() {
        thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested()) {
                tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    // Destructor: thread_ goes out of scope → jthread joins automatically
};
```

---

### Step 8 — Race start latch (`src/simulation/race_director.hpp`)

**Concept:** `std::latch` — one-shot countdown synchronization.

A `std::latch` counts down to zero exactly once. Use it to ensure all threads (telemetry generator, race control, UI) are fully initialized before the simulation begins. Each thread calls `latch.count_down()` when ready, then `latch.wait()` to block until all others are ready.

```cpp
// In race_director.hpp
std::latch race_start_latch{4}; // 4 threads must signal ready

// Each thread, before entering its main loop:
race_start_latch.arrive_and_wait(); // count_down() + wait() atomically
```

Unlike `std::barrier`, a latch cannot be reused. It is a one-shot signal.

---

### Step 9 — Lap barrier (`src/simulation/lap_barrier.hpp`)

**Concept:** `std::barrier` with completion function — reusable synchronization point.

At the end of each simulated lap, all driver update threads (if you parallelize per-driver updates) must reach the barrier before positions are recalculated. The barrier's completion callback runs the sort exactly once, guaranteed, after all threads have arrived.

```cpp
// Completion function runs once per phase, on the thread that triggers it
auto on_lap_complete = []() noexcept {
    recalculate_positions(); // runs exactly once, no race possible
};

std::barrier lap_sync{20, on_lap_complete}; // 20 drivers

// Each driver's update thread, at lap boundary:
lap_sync.arrive_and_wait();
```

Unlike latch, `std::barrier` resets after each phase completion and can be used for every lap.

---

### Step 10 — Pit lane semaphore (`src/simulation/pit_lane.hpp`)

**Concept:** `std::counting_semaphore` — resource limiting.

Real F1 pit lanes can only service one car per team. Simplify to a global maximum of 2 cars in the pit lane simultaneously. A driver trying to pit must acquire the semaphore. They release it when they exit.

```cpp
// counting_semaphore<N> where N is the maximum concurrent resource holders
std::counting_semaphore<2> pit_lane_capacity{2};

// Driver enters pit:
pit_lane_capacity.acquire(); // blocks if 2 cars already in pits
// ... service the car ...
pit_lane_capacity.release(); // signal that slot is free
```

---

### Phase 3 Tests (`tests/phase3/`)

**Run:** `./build/tests/phase3/phase3_tests`

#### Telemetry generator tests (`test_telemetry_generator.cpp`)
```
- Start generator, let it run for 200ms (~10 ticks at 50Hz), stop it
- Verify at least 10 frames were pushed per driver into the SPSC queue
- Verify tire_wear increases monotonically each lap
- Verify fuel_kg decreases monotonically each tick
- Verify speed_kph is within [0, 370] kph (plausible F1 range)
- Stop via stop_token: generator stops within 30ms of request_stop()
- jthread destructor test: generator goes out of scope; verify thread joins
  cleanly with no hang (use a timeout in the test)
```

#### Latch tests (`test_latch.cpp`)
```
- 4 threads count down a latch{4}; all must exit arrive_and_wait() together
- Measure max spread between thread release times (should be < 1ms)
- Verify latch cannot be reused: calling arrive_and_wait() again after
  it reaches zero returns immediately (already open)
- Single-thread degenerate case: latch{1} — arrive_and_wait() returns immediately
```

#### Barrier tests (`test_barrier.cpp`)
```
- 5 threads use a barrier{5}; run 10 phases; completion callback fires exactly 10 times
- Completion callback receives no arguments and modifies an atomic counter;
  verify counter == 10 after all phases
- Threads that arrive early block until the last thread arrives
- Timing test: measure that all threads release from each phase within 1ms of each other
```

#### Semaphore tests (`test_semaphore.cpp`)
```
- counting_semaphore<2>: 2 threads acquire immediately; 3rd blocks
- 3rd thread unblocks when one of the first two releases
- Stress test: 10 threads compete for semaphore<3>; verify at most 3 hold it
  simultaneously (use an atomic counter inside the guarded region)
- try_acquire() returns false when at capacity, true when slot available
- Run under TSan: zero data races
```

**Gate:** All Phase 3 tests green, zero TSan warnings before starting Phase 4.

---

## Phase 4 — Race Control Systems

**Goal:** Build the event-driven systems that run concurrently with the simulation.

---

### Step 11 — Track limits monitor (`src/race_control/track_limits.hpp/.cpp`)

**Concept:** MPSC queue as event bus, thread pool task scheduling.

Submitted to the thread pool as a periodic task (every 500ms simulated time). Probabilistically detects violations at sector boundaries based on driver aggression, speed, and tire wear. Pushes `RaceControlEvent{TRACK_LIMITS}` into the MPSC queue.

```
Violation probability = base_rate × aggression × (1 + tire_wear) × speed_factor
```

---

### Step 12 — Penalty enforcer (`src/race_control/penalty_enforcer.hpp/.cpp`)

**Concept:** Atomic state machine with `compare_exchange_strong`.

Consumes events from the MPSC queue. Tracks warning counts per driver in a `std::array<std::atomic<int>, 20>`. On the 3rd warning, transitions penalty state from `NONE → PENDING` using CAS to prevent double-penalties from concurrent updates.

```cpp
// Atomic state transition: only one thread can move the state
PenaltyState expected = PenaltyState::NONE;
if (penalty_state.compare_exchange_strong(expected, PenaltyState::PENDING)) {
    // We won the CAS race — we issue the penalty
    push_event(RaceControlEvent{PENALTY_ISSUED, driver_id, ...});
}
// If CAS failed, another thread already issued the penalty — do nothing
```

---

### Step 13 — Weather system (`src/race_control/weather.hpp/.cpp`)

**Concept:** Thread pool periodic task, `shared_mutex` write path.

Submitted to the thread pool every 10 simulated laps. Randomly transitions track state: `DRY → DAMP → WET → DRY`. Writes to the shared `TrackProfile` under a `unique_lock<shared_mutex>`. All driver threads read it under a `shared_lock<shared_mutex>`.

---

### Step 14 — Strategy analyzer (`src/strategy/strategy_analyzer.hpp/.cpp`)

**Concept:** Thread pool task batching, collecting `std::future` results.

Before the race and after each pit stop, submits 10 lightweight race simulations to the thread pool — one per candidate pit lap (laps 12, 15, 18, 21, 24, 27, 30, 33, 36, 39). Collects results via `std::future<float>` (estimated finish time). Returns the pit lap with minimum estimated time.

```cpp
std::vector<std::future<float>> futures;
for (int pit_lap : candidates) {
    futures.push_back(pool_.submit(simulate_race_with_pit, driver, pit_lap, track));
}
// Wait for all results
float best_time = std::numeric_limits<float>::max();
int   best_lap  = candidates[0];
for (int i = 0; i < futures.size(); ++i) {
    float t = futures[i].get(); // blocks until this scenario is done
    if (t < best_time) { best_time = t; best_lap = candidates[i]; }
}
```

---

### Phase 4 Tests (`tests/phase4/`)

**Run:** `./build/tests/phase4/phase4_tests`

#### Track limits tests (`test_track_limits.cpp`)
```
- High-aggression driver (1.0) triggers more violations than low-aggression (0.1)
  over 10,000 simulated sector crossings — chi-squared test on counts
- Events are pushed to the MPSC queue (verify queue size increases)
- Monitor runs as a thread pool task: submit it, verify it completes without hang
- Violations have valid driver_id, lap, and sector fields
```

#### Penalty enforcer tests (`test_penalty_enforcer.cpp`)
```
- 3 TRACK_LIMITS events for same driver → PENALTY_ISSUED event generated
- 2 events → no penalty issued
- Concurrent test: 4 threads each push a 3rd warning for same driver simultaneously;
  verify exactly 1 penalty issued (not 4) — tests CAS correctness
- State machine: NONE → PENDING → SERVING → SERVED in correct order
- Different drivers accumulate warnings independently
```

#### Weather tests (`test_weather.cpp`)
```
- Weather transitions follow valid sequence: DRY/DAMP/WET only
- shared_mutex test: 8 reader threads + 1 writer thread run concurrently;
  readers always see a consistent (non-torn) WeatherState
- Writer holds lock for < 1ms (verify with timing)
- Run under TSan: zero data races
```

#### Strategy analyzer tests (`test_strategy_analyzer.cpp`)
```
- Analyze() returns a pit lap within the candidate list
- All 10 futures complete (no hang, no deadlock)
- Deterministic scenario: known tire deg rate → expected optimal lap is verifiable
- Thread pool is not exhausted: submit 5 concurrent analyses;
  all complete without deadlock
- Exception in one simulation scenario: other futures still resolve
```

**Gate:** All Phase 4 tests green, zero TSan warnings before starting Phase 5.

---

## Phase 5 — Shared State Layer

**Goal:** Define the shared data that the simulation writes and the UI reads concurrently.

---

### Step 15 — Leaderboard state (`src/common/leaderboard.hpp`)

**Concept:** `std::shared_mutex` — single-writer, multiple-reader (SWMR).

The leaderboard is the most-read data structure in the system. The UI renders it at 10 Hz. The simulation updates it once per lap (once per second at 120x speed). A `shared_mutex` lets many readers proceed concurrently while ensuring the writer has exclusive access.

```cpp
class Leaderboard {
    std::vector<DriverState> standings_; // sorted by position
    mutable std::shared_mutex mutex_;

public:
    // Called by producer — exclusive lock, blocks all readers
    void update(std::vector<DriverState> sorted_states) {
        std::unique_lock lock{mutex_};
        standings_ = std::move(sorted_states);
    }

    // Called by UI thread — shared lock, allows concurrent reads
    std::vector<DriverState> snapshot() const {
        std::shared_lock lock{mutex_};
        return standings_; // copy under lock
    }
};
```

`mutable` on the mutex allows `snapshot()` to be `const` — logically read-only to callers.

---

### Step 16 — Race state atomics (`src/common/race_state.hpp`)

**Concept:** `std::atomic<T>` with explicit memory ordering and documented rationale.

Global race flags that multiple threads read and write. Every annotation has a comment explaining why that order is sufficient (or why stronger ordering is needed).

```cpp
struct RaceState {
    std::atomic<bool>        race_active{false};
    std::atomic<bool>        safety_car{false};
    std::atomic<int>         current_lap{0};
    std::atomic<std::string*> fastest_lap_holder{nullptr}; // pointer swap

    // Safety car broadcast:
    // Producer writes: safety_car.store(true, memory_order_release)
    // All driver threads read: safety_car.load(memory_order_acquire)
    // Release/acquire pair ensures drivers see the flag before reacting.
    // seq_cst not needed here — only one writer, no ordering relative
    // to other atomics required.

    // Fastest lap: CAS swap
    bool try_set_fastest_lap(std::string* expected, std::string* new_holder) {
        return fastest_lap_holder.compare_exchange_strong(
            expected, new_holder,
            std::memory_order_acq_rel,  // success: full acquire+release
            std::memory_order_acquire   // failure: acquire to read current
        );
    }
};
```

---

### Phase 5 Tests (`tests/phase5/`)

**Run:** `./build/tests/phase5/phase5_tests`

#### Leaderboard tests (`test_leaderboard.cpp`)
```
- update() followed by snapshot() returns the same data
- Concurrent stress: 1 writer thread updates every 10ms;
  8 reader threads call snapshot() as fast as possible for 2 seconds;
  verify readers never see a partially-written state (all 20 drivers present)
- No deadlock: writer and readers never starve each other
  (verify by timing: max reader wait < 5ms)
- Run under TSan: zero data races
- SWMR property: measure reader throughput with shared_mutex vs plain mutex;
  document the difference
```

#### Race state tests (`test_race_state.cpp`)
```
- safety_car flag: producer sets it; 10 consumer threads read it;
  all see true within 1ms (tests release/acquire visibility)
- current_lap increments atomically: 1 writer increments 1000 times;
  final value == 1000 (no lost updates)
- fastest_lap CAS: 5 threads simultaneously try to claim fastest lap;
  exactly 1 succeeds; no double-claim
- Memory ordering test: use a second atomic to verify happens-before
  (pattern: producer writes data, then sets flag with release;
   consumer acquires flag, then reads data; verify data is correct)
- Run under TSan: zero data races
```

**Gate:** All Phase 5 tests green, zero TSan warnings before starting Phase 6.

---

## Phase 6 — FTXUI Dashboard

**Goal:** Build the terminal UI that renders all shared state in real time.

---

### Step 17 — FTXUI skeleton (`src/ui/dashboard.hpp/.cpp`)

Add FTXUI via `FetchContent`. Create a `ScreenInteractive` with three panels arranged in a layout:
- Top row: leaderboard (left 50%) + driver telemetry (right 50%)
- Bottom: race control feed (full width)

Confirm it renders a static placeholder and exits cleanly on `q`.

```cpp
// Minimal skeleton
auto screen = ftxui::ScreenInteractive::Fullscreen();
auto renderer = ftxui::Renderer([&] {
    return ftxui::vbox({
        ftxui::hbox({
            leaderboard_panel(),  // left half
            ftxui::separator(),
            telemetry_panel(),    // right half
        }) | ftxui::flex,
        ftxui::separator(),
        race_control_panel(),    // bottom strip
    });
});
screen.Loop(renderer);
```

---

### Step 18 — Leaderboard panel (`src/ui/leaderboard_panel.hpp`)

Calls `leaderboard.snapshot()` (shared lock) on every render frame. Renders:
- Position number with gold/silver/bronze coloring for P1/P2/P3
- Driver code and team color
- Lap number
- Gap to leader in seconds (or LEADER for P1)
- Tire wear bar using FTXUI `gauge()`
- Status badges: IN PITS, PENALTY, DRS, FL (fastest lap holder)
- Arrow key navigation selects a driver for the telemetry panel

---

### Step 19 — Driver telemetry panel (`src/ui/telemetry_panel.hpp`)

Shows the latest `TelemetryFrame` for the currently selected driver. Updates each render frame by popping from the SPSC queue (consumer side). Renders:
- Speed with FTXUI `gauge()` (0–370 kph scale)
- Throttle percentage bar (green)
- Brake percentage bar (red)
- Tire wear bar (green → yellow → red based on percentage)
- Fuel load bar with kg label
- DRS status (bold green if active)
- Current lap / sector

---

### Step 20 — Race control feed (`src/ui/race_control_panel.hpp`)

Drains the MPSC queue on each render frame (up to 5 events per frame to avoid starving the render loop). Maintains a ring buffer of the last 15 events. Renders as a scrolling list with icons:
- `[TL]` Track limits warning
- `[PEN]` Penalty issued
- `[PIT]` Pit entry/exit
- `[WX]` Weather change
- `[SC]` Safety car
- `[FL]` Fastest lap
- `[RAD]` Radio message

---

### Step 21 — Wire simulation to UI

Connect all components:
1. `RaceDirector` owns the `std::latch` — waits for UI to signal ready before starting simulation.
2. UI render loop reads `Leaderboard::snapshot()` under shared lock.
3. UI telemetry panel pops from SPSC queue's consumer end.
4. UI race control panel drains MPSC queue.
5. UI reads `RaceState` atomics for safety car flag, current lap display.
6. `q` key sets `race_active = false` (release store) → all `jthread`s see it via acquire load → stop cleanly.

---

### Phase 6 Tests (`tests/phase6/test_dashboard_integration.cpp`)

**Note:** FTXUI UI components are hard to unit-test in isolation. Focus on integration and the data pipeline, not pixel-level rendering.

```
- Integration test: start full simulation (generator + race control + strategy);
  let it run for 500ms; verify SPSC queue has been populated and drained
- Leaderboard panel data test: snapshot() returns 20 drivers in valid position order
  (positions 1-20, no duplicates) after 1 simulated lap
- Race control feed test: after 2 seconds of simulation, MPSC queue has received
  at least 1 event of each type (TRACK_LIMITS, PIT_ENTRY)
- Shutdown test: send stop signal; verify all jthreads join within 500ms;
  no zombie threads, no memory leaks (run under AddressSanitizer)
- TSan full-system run: run simulation for 5 seconds; zero data races reported
```

**Gate:** All Phase 6 tests green, zero TSan/ASan warnings before starting Phase 7.

---

## Phase 7 — Features and Polish

**Goal:** Add the remaining F1 features, each motivated by a concurrency concept.

---

### Step 22 — Safety car

**Concept:** Atomic broadcast with `memory_order_release/acquire`.

Race director deploys safety car when incident probability fires. Sets `race_state.safety_car.store(true, memory_order_release)`. All 20 driver update threads read `safety_car.load(memory_order_acquire)` each tick and cap speed to 80 kph if true. Teaches atomic broadcast without any locks — the release/acquire pair is the only synchronization needed.

---

### Step 23 — Radio messages feed

Drivers send pit-wall radio messages at key moments (push warning, tire concern, overtake). Pushed into a second `MpscQueue<std::string>`. Displayed in a scrolling ticker in the race control panel below the event feed. Teaches MPSC queue reuse and queue draining rate limiting.

---

### Step 24 — Race end and results screen

When the leader completes all laps:
1. `race_active.store(false, memory_order_release)`.
2. All `jthread`s see it via `memory_order_acquire` load and exit their loops.
3. `jthread` destructors join automatically.
4. FTXUI transitions to a full-screen results table: finishing order, fastest lap holder, total pit stops per driver, safety car laps.
5. Press any key to exit.

---

### Step 25 — Final CMake and README

- Add CMake install targets.
- Add `AddressSanitizer` and `ThreadSanitizer` build presets to `CMakePresets.json`.
- Write a README section for each concurrency concept used: what it is, where it is in the code, why it was chosen over alternatives, and what interview question it answers.

---

## Testing Setup Reference

### How to run tests

```bash
# Build everything including tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run all tests
ctest --test-dir build --output-on-failure

# Run a specific phase
./build/tests/phase2/phase2_tests
./build/tests/phase3/phase3_tests

# Run with ThreadSanitizer (catches data races)
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan
./build-tsan/tests/phase2/phase2_tests

# Run with AddressSanitizer (catches memory errors and leaks)
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -g"
cmake --build build-asan
./build-asan/tests/phase6/phase6_tests
```

### Test file template

```cpp
// tests/phase2/test_spsc_queue.cpp
#include <gtest/gtest.h>
#include "concurrency/spsc_queue.hpp"

TEST(SpscQueueTest, PushPopSingleThread) {
    SpscQueue<int, 16> q;
    EXPECT_TRUE(q.push(42));
    int val;
    EXPECT_TRUE(q.pop(val));
    EXPECT_EQ(val, 42);
}

TEST(SpscQueueTest, FullQueueReturnsFalse) {
    SpscQueue<int, 4> q; // capacity 4
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_FALSE(q.push(4)); // full
}

TEST(SpscQueueTest, ConcurrentStressTest) {
    SpscQueue<int, 1024> q;
    constexpr int N = 100'000;
    std::atomic<int> sum_produced{0}, sum_consumed{0};

    std::jthread producer([&](std::stop_token st) {
        for (int i = 0; i < N; ++i) {
            while (!q.push(i)) {} // spin until space
            sum_produced.fetch_add(i, std::memory_order_relaxed);
        }
    });

    std::jthread consumer([&](std::stop_token st) {
        int received = 0;
        int val;
        while (received < N) {
            if (q.pop(val)) {
                sum_consumed.fetch_add(val, std::memory_order_relaxed);
                ++received;
            }
        }
    });

    // jthreads join on destruction
    producer.join();
    consumer.join();

    EXPECT_EQ(sum_produced.load(), sum_consumed.load());
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

### `tests/CMakeLists.txt` template

```cmake
enable_testing()

function(add_phase_tests phase_name sources)
    add_executable(${phase_name}_tests ${sources})
    target_link_libraries(${phase_name}_tests
        PRIVATE pitwall_lib GTest::gtest GTest::gtest_main
    )
    add_test(NAME ${phase_name} COMMAND ${phase_name}_tests)
endfunction()

add_phase_tests(phase1 phase1/test_types.cpp)
add_phase_tests(phase2
    phase2/test_spsc_queue.cpp
    phase2/test_mpsc_queue.cpp
    phase2/test_thread_pool.cpp
)
add_phase_tests(phase3
    phase3/test_telemetry_generator.cpp
    phase3/test_latch.cpp
    phase3/test_barrier.cpp
    phase3/test_semaphore.cpp
)
add_phase_tests(phase4
    phase4/test_track_limits.cpp
    phase4/test_penalty_enforcer.cpp
    phase4/test_weather.cpp
    phase4/test_strategy_analyzer.cpp
)
add_phase_tests(phase5
    phase5/test_leaderboard.cpp
    phase5/test_race_state.cpp
)
add_phase_tests(phase6 phase6/test_dashboard_integration.cpp)
```

---

## Progress Tracker

- [ ] Phase 1 — Foundation (types, season data, CMake)
- [ ] Phase 2 — Concurrency Primitives (SPSC, MPSC, thread pool)
- [ ] Phase 3 — Simulation Engine (jthread, latch, barrier, semaphore)
- [ ] Phase 4 — Race Control Systems (track limits, penalties, weather, strategy)
- [ ] Phase 5 — Shared State Layer (shared_mutex, atomics)
- [ ] Phase 6 — FTXUI Dashboard (panels, wiring, integration)
- [ ] Phase 7 — Features and Polish (safety car, radio, race end)

---

## Interview Cheat Sheet

| Topic | Where in PitWall | Question it answers |
|-------|-----------------|---------------------|
| Lock-free SPSC | `concurrency/spsc_queue.hpp` | "Implement a lock-free ring buffer" |
| Memory ordering | `spsc_queue.hpp`, `race_state.hpp` | "What is acquire/release semantics?" |
| False sharing | `spsc_queue.hpp` (alignas(64)) | "What is false sharing and how do you fix it?" |
| CAS loop | `concurrency/mpsc_queue.hpp` | "Explain compare-and-swap" |
| ABA problem | `mpsc_queue.hpp` comments | "What is the ABA problem?" |
| Thread pool | `concurrency/thread_pool.hpp` | "Design a thread pool" |
| SWMR | `common/leaderboard.hpp` | "When would you use shared_mutex?" |
| Graceful shutdown | All `jthread` usage | "How do you safely stop threads?" |
| Atomic broadcast | `race_state.hpp` safety car | "How do you signal many threads without a lock?" |
| Semaphore | `simulation/pit_lane.hpp` | "What is a semaphore? When is it better than a mutex?" |
