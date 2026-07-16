#pragma once

#include "common/types.h"
#include "common/season_data.h"
#include "concurrency/spsc_queue.h"
#include "simulation/lap_barrier.h"
#include "simulation/pit_lane.h"
#include "simulation/race_director.h"
#include <array>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>
#include <random>
#include <functional>

// Simulation constants
inline constexpr float TIME_SCALE     = 120.0f; // sim runs 120x faster than real time
inline constexpr float TICK_S         = 0.02f;  // 20ms per real tick (50Hz)
inline constexpr float SIM_S_PER_TICK = TIME_SCALE * TICK_S; // 2.4 sim-seconds per tick
inline constexpr float TIRE_WEAR_BASE = 0.00130f; // calibrated: tires last ~30 laps
inline constexpr int   PIT_STOP_TICKS = 11;       // 25 sim-seconds / 2.4 ~= 11 ticks
inline constexpr int   TOTAL_LAPS     = 50;

// Callback type: called after every tick with the updated states.
// Used later by the leaderboard to snapshot current standings.
using OnTickCallback = std::function<void(const std::vector<DriverState>&)>;

// Generates fake telemetry for 20 drivers at 50Hz on a background thread.
// This is the "producer" in the producer/consumer pipeline built in Phase 2.
class TelemetryGenerator {
public:
    explicit TelemetryGenerator(
        SpscQueue<TelemetryFrame, 2048>& queue,
        const TrackProfile& track = DEFAULT_TRACK
    );

    // Register a callback invoked after every tick (runs on whichever thread
    // the lap barrier's completion function executes on — see on_tick_complete()).
    void set_on_tick(OnTickCallback cb) { on_tick_ = std::move(cb); }

    // Optional: if set, the generator's pacer thread rendezvous with this
    // gate before entering its tick loop. Lets the caller capture the race
    // start timestamp at the instant ticking actually begins, not the
    // instant start() was called.
    void set_start_gate(RaceDirector* gate) { start_gate_ = gate; }

    // Applies pre-computed pit-lap plans, keyed by driver id. Call after
    // construction (states_ already seeded) but before start().
    void apply_pit_plan(const std::unordered_map<std::string, int>& plans);

    // Start the producer thread (plus the chunk-worker threads).
    void start();

    // Ask all threads to stop, then block until they actually have (join).
    void stop();

    bool is_running() const { return thread_.joinable(); }

    // Returns a copy of the current standings — safe to call from any thread
    // because it copies the vector rather than handing out a reference.
    std::vector<DriverState> standings() const;

    int race_lap() const { return race_lap_; }
    bool race_finished() const { return race_finished_; }

private:
    void run();  // pacer thread entry point
    void chunk_worker(std::size_t worker_idx, std::size_t first, std::size_t last);
    void process_chunk(std::size_t worker_idx, std::size_t first, std::size_t last);
    void on_tick_complete(); // lap barrier completion — push, recalc, callback
    void update_positions_and_drs();
    void handle_pit(DriverState& state);
    bool should_pit(const DriverState& state) const;
    static float max_speed(const CarProfile& car);

    SpscQueue<TelemetryFrame, 2048>& queue_;
    TrackProfile                     track_;
    std::vector<DriverState>         states_;
    std::jthread                     thread_;
    OnTickCallback                   on_tick_;

    // One RNG + distribution per chunk worker — NOT shared. Before Phase S,
    // a single rng_/dist_ pair was safe because one thread ticked all 20
    // drivers sequentially. With 4 chunk-worker threads now calling
    // dist_(rng_) concurrently, a shared engine is a real data race (caught
    // by ThreadSanitizer: mersenne_twister_engine::operator() torn reads).
    // Each worker gets its own, seeded distinctly so the 4 streams aren't
    // identical.
    std::array<std::mt19937, 4>                    rngs_{
        std::mt19937{42}, std::mt19937{43}, std::mt19937{44}, std::mt19937{45}
    };
    std::array<std::normal_distribution<float>, 4> dists_{};

    int  race_lap_      {1};
    bool race_finished_ {false};

    // Phase S: parallel tick + pit-lane + race-start wiring
    LapBarrier tick_barrier_;                  // 4 chunk workers + pacer thread
    PitLane    pit_lane_;
    std::array<std::jthread, 4> chunk_workers_;
    RaceDirector* start_gate_ {nullptr};

    // Independent, manually-verified happens-before edge for states_ itself.
    // Apple's experimental libc++ std::barrier does not reliably give
    // ThreadSanitizer a visible happens-before between an arriving thread's
    // pre-arrival writes and the completion function's reads of them — a
    // minimal repro (N threads writing disjoint array slots, one completion
    // function summing all of them) reproduces the same race with no
    // project code involved. tick_barrier_ is still used for pacing/
    // counting ("wait until all 5 have arrived"), but the actual data
    // handoff from chunk workers to on_tick_complete() goes through this
    // release/acquire counter instead, which TSan verifies unambiguously.
    std::atomic<int> workers_done_ {0};

    // External stop request, written by stop() (possibly mid-phase, from a
    // thread not participating in the barrier at all).
    std::atomic<bool> shutdown_requested_ {false};

    // The actual "stop after this phase" decision, written exactly once per
    // phase — inside on_tick_complete() — and read (as a plain, non-atomic
    // bool) by all 5 threads right after they resume from
    // tick_barrier_.arrive_and_wait(). This indirection matters: reading
    // shutdown_requested_ directly on each of the 5 threads is NOT enough,
    // even with correct release/acquire pairing. Release/acquire only
    // guarantees ordering between a store and the specific load that reads
    // it — it does NOT guarantee 5 independent threads, resuming at
    // slightly different real times, all observe the same value of a store
    // that races with their reads. If stop() lands its write in the window
    // while the 5 threads are resuming, some could see true and others
    // still see false — the ones that see false loop back into another
    // phase, while the ones that saw true have already exited, permanently
    // deadlocking the barrier on the phase that will now never collect all
    // 5 arrivals. on_tick_complete() runs exactly once per phase and is
    // guaranteed (by std::barrier's own synchronization) to happen before
    // ANY of the 5 threads resume for that phase, so a plain write there is
    // guaranteed visible, identically, to whichever thread reads it next —
    // no race is possible.
    bool phase_should_stop_ {false};
};