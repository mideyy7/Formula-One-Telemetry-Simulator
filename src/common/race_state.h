#pragma once

#include <atomic>
#include <string>
#include <limits>

// RaceState holds global flags that multiple threads read and write.
//
// Every load/store has an explicit memory_order annotation.
// The comments explain WHY that order is sufficient.
//
// ─── Memory order quick reference ────────────────────────────────────────────
//
// memory_order_relaxed  — no synchronisation; just atomicity. Safe only when
//                         the value itself is the only thing that matters
//                         (e.g. a monotonically increasing counter where you
//                         don't care about the order of other writes).
//
// memory_order_release  — on a STORE: all writes before this store are
//                         visible to any thread that does an acquire-load
//                         of the SAME atomic. Think: "I'm done writing;
//                         publish everything I've done."
//
// memory_order_acquire  — on a LOAD: if I see the value written by a
//                         release-store, I also see all writes that
//                         happened before that store. Think: "I'm reading;
//                         show me everything the writer wrote before this."
//
// memory_order_acq_rel  — on a READ-MODIFY-WRITE (e.g. exchange, CAS):
//                         acts as both acquire and release simultaneously.
//
// memory_order_seq_cst  — total global order: the most expensive, rarely
//                         needed. Only use when you have multiple atomics
//                         and threads must agree on the ORDER of all stores.
//
// ─────────────────────────────────────────────────────────────────────────────

struct RaceState {
    // ── race_active ───────────────────────────────────────────────────────────
    // Written ONCE at start (true) and ONCE at end (false).
    // All threads read it every tick to decide whether to exit.
    //
    // Store: release — ensures all simulation setup writes are visible to
    //   threads that subsequently load with acquire.
    // Load:  acquire — threads see the store and all writes before it.
    std::atomic<bool> race_active{false};

    void start_race() { race_active.store(true,  std::memory_order_release); }
    void end_race()   { race_active.store(false, std::memory_order_release); }
    bool is_active()  { return race_active.load(std::memory_order_acquire);  }

    // ── current_lap ──────────────────────────────────────────────────────────
    // Written by TelemetryGenerator. Read by a future UI for the lap counter.
    //
    // relaxed load/store is fine here: the lap number is informational.
    // If the UI displays lap N-1 for one frame instead of N, no harm done.
    // We do NOT need the lap update to be ordered relative to other writes.
    std::atomic<int> current_lap{1};

    void set_lap(int lap)  { current_lap.store(lap, std::memory_order_relaxed); }
    int  get_lap()   const { return current_lap.load(std::memory_order_relaxed); }

    // ── safety_car ───────────────────────────────────────────────────────────
    // Written by a race director. Read by all 20 driver threads each tick.
    //
    // release store: ensures readers also see any other state updated before
    //   the safety car flag was set.
    // acquire load: readers see the flag AND all preceding writes.
    //
    // This is an "atomic broadcast" — one writer, many readers, no locks.
    std::atomic<bool> safety_car{false};

    void deploy_safety_car()  { safety_car.store(true,  std::memory_order_release); }
    void retract_safety_car() { safety_car.store(false, std::memory_order_release); }
    bool is_safety_car() const { return safety_car.load(std::memory_order_acquire); }

    // ── fastest_lap_holder ───────────────────────────────────────────────────
    // Updated whenever a driver sets a new fastest lap.
    // Multiple driver threads can simultaneously try to claim fastest lap —
    // CAS ensures exactly one wins.
    //
    // We store an index into DRIVERS (0-19) rather than a string to keep
    // the atomic simple. -1 means "no fastest lap set yet".
    //
    // acq_rel on success: we've written the holder AND all our prior writes
    //   are visible. acquire on failure: we see the current winner's time.
    std::atomic<int>   fastest_lap_holder{-1};
    std::atomic<float> fastest_lap_ms{std::numeric_limits<float>::max()};

    // Returns true if this driver set a new fastest lap.
    bool try_claim_fastest_lap(int driver_index, float lap_ms) {
        float current_best = fastest_lap_ms.load(std::memory_order_acquire);
        while (lap_ms < current_best) {
            if (fastest_lap_ms.compare_exchange_weak(
                    current_best, lap_ms,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                // We updated the time; now update the holder.
                fastest_lap_holder.store(driver_index, std::memory_order_release);
                return true;
            }
            // current_best was overwritten by the failed CAS with the
            // now-current value. Loop and check again against that.
        }
        return false;
    }

    int  get_fastest_lap_holder() const {
        return fastest_lap_holder.load(std::memory_order_acquire);
    }
    float get_fastest_lap_ms() const {
        return fastest_lap_ms.load(std::memory_order_acquire);
    }
};

// Global singleton — declared here. Not yet defined/wired into main.cpp;
// see lesson5.md for why that wiring is deliberately deferred, same as
// Phase 3/4's not-yet-integrated components. A production translation unit
// that wants this must provide exactly one definition:
//   RaceState g_race_state;
// `extern` here just tells the compiler "this exists somewhere, don't
// allocate storage for it in every file that includes this header."
extern RaceState g_race_state;