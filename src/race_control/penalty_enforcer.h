#pragma once

#include "common/types.h"
#include "concurrency/mpsc_queue.h"
#include <array>
#include <atomic>
#include <string>

// PenaltyEnforcer consumes TRACK_LIMITS events and manages per-driver
// penalty state via atomic compare-and-exchange (CAS).
//
// State machine per driver:
//   NONE -> PENDING  (on 3rd warning, via CAS)
//   PENDING -> SERVING (when driver enters pits)
//   SERVING -> SERVED  (when pit stop completes)
//
// Why CAS for the NONE->PENDING transition?
//   If track limits events arrive from multiple threads simultaneously
//   (possible if you add more monitoring threads later), two threads might
//   both see warnings_count[i] == 3 and both try to issue a penalty.
//   CAS ensures exactly one succeeds:
//     expected = NONE; if (state.CAS(expected, PENDING)) { issue penalty }
//   The losing CAS sees expected != NONE and does nothing.
//
// The warning counters use fetch_add, which is inherently atomic — no CAS
// needed there, because we don't care about "the exact instant it crossed
// 3," only "add 1 and tell me the new total."

class PenaltyEnforcer {
public:
    explicit PenaltyEnforcer(MpscQueue<RaceControlEvent>& event_queue);

    // Drain the MPSC queue and process all pending TRACK_LIMITS events.
    // Call this from any single consumer thread.
    void process_events();

    // Called by the simulator when a driver enters/exits the pits.
    void driver_entered_pits(const std::string& driver_id);
    void driver_exited_pits(const std::string& driver_id);

    // Current penalty state for a driver.
    PenaltyState penalty_state(const std::string& driver_id) const;

    // Number of accumulated warnings.
    int warning_count(const std::string& driver_id) const;

private:
    int driver_index(const std::string& id) const;

    MpscQueue<RaceControlEvent>& events_;

    // One entry per driver (indexed by DRIVERS order).
    std::array<std::atomic<int>,          20> warnings_{};
    std::array<std::atomic<PenaltyState>, 20> states_{};
    std::array<std::atomic<int>,          20> extra_pit_time_{}; // ticks added
};