#include "race_control/penalty_enforcer.h"
#include "common/season_data.h"
#include <algorithm>
#include <chrono>
#include <vector>

PenaltyEnforcer::PenaltyEnforcer(MpscQueue<RaceControlEvent>& event_queue)
    : events_(event_queue)
{
    // Initialise all penalty states to NONE.
    for (auto& s : states_) s.store(PenaltyState::NONE, std::memory_order_relaxed);
    for (auto& w : warnings_) w.store(0, std::memory_order_relaxed);
    for (auto& t : extra_pit_time_) t.store(0, std::memory_order_relaxed);
}

int PenaltyEnforcer::driver_index(const std::string& id) const {
    for (int i = 0; i < static_cast<int>(DRIVERS.size()); ++i) {
        if (DRIVERS[i].id == id) return i;
    }
    return -1;
}

void PenaltyEnforcer::process_events() {
    // Snapshot everything currently queued BEFORE processing any of it.
    // Why: processing a TRACK_LIMITS event can itself push a new
    // PENALTY_ISSUED event onto this same queue. If we drained with a
    // single `while (events_.pop(ev))` loop, that freshly-pushed event
    // would be picked up by the very same loop a few iterations later,
    // seen as "not TRACK_LIMITS", and silently discarded — the enforcer
    // would eat its own announcement before anyone else (a UI, a test)
    // gets to see it. Snapshotting first means anything we push during
    // processing is left in the queue for the NEXT call/consumer instead.
    std::vector<RaceControlEvent> batch;
    RaceControlEvent ev;
    while (events_.pop(ev)) batch.push_back(std::move(ev));

    for (auto& e : batch) {
        if (e.type != RaceControlEvent::Type::TRACK_LIMITS) continue;

        int idx = driver_index(e.driver_id);
        if (idx < 0) continue;

        int count = warnings_[idx].fetch_add(1, std::memory_order_relaxed) + 1;

        if (count == 3) {
            // CAS: only ONE thread can win this transition.
            // expected gets overwritten with the actual value if CAS fails —
            // that's fine here since we don't reuse it afterwards.
            PenaltyState expected = PenaltyState::NONE;
            if (states_[idx].compare_exchange_strong(
                    expected, PenaltyState::PENDING,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                // We won. Push a PENALTY_ISSUED event.
                RaceControlEvent pen;
                pen.type      = RaceControlEvent::Type::PENALTY_ISSUED;
                pen.driver_id = e.driver_id;
                pen.lap       = e.lap;
                pen.message   = e.driver_id + " - 5 second penalty (track limits)";
                pen.timestamp = std::chrono::steady_clock::now();
                events_.push(std::move(pen));

                // Add 5 seconds worth of pit stop ticks (~13 ticks at 2.4s/tick).
                extra_pit_time_[idx].store(3, std::memory_order_relaxed);
            }
            // If CAS failed: another thread already issued the penalty. Do nothing.
        }
    }
}

void PenaltyEnforcer::driver_entered_pits(const std::string& driver_id) {
    int idx = driver_index(driver_id);
    if (idx < 0) return;

    PenaltyState expected = PenaltyState::PENDING;
    states_[idx].compare_exchange_strong(
        expected, PenaltyState::SERVING,
        std::memory_order_acq_rel,
        std::memory_order_relaxed);
}

void PenaltyEnforcer::driver_exited_pits(const std::string& driver_id) {
    int idx = driver_index(driver_id);
    if (idx < 0) return;

    PenaltyState expected = PenaltyState::SERVING;
    states_[idx].compare_exchange_strong(
        expected, PenaltyState::SERVED,
        std::memory_order_acq_rel,
        std::memory_order_relaxed);
}

PenaltyState PenaltyEnforcer::penalty_state(const std::string& driver_id) const {
    int idx = driver_index(driver_id);
    if (idx < 0) return PenaltyState::NONE;
    return states_[idx].load(std::memory_order_acquire);
}

int PenaltyEnforcer::warning_count(const std::string& driver_id) const {
    int idx = driver_index(driver_id);
    if (idx < 0) return 0;
    return warnings_[idx].load(std::memory_order_relaxed);
}