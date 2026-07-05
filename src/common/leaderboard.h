#pragma once

#include "common/types.h"
#include <shared_mutex>
#include <vector>
#include <limits>
#include <string>

// Leaderboard holds the sorted race standings.
//
// Access pattern:
//   Writer: TelemetryGenerator (1 thread) — updates once per lap
//   Readers: a future UI render loop + any status queries
//
// std::shared_mutex (SWMR — Single Writer Multiple Readers):
//   unique_lock  -> exclusive: blocks ALL other readers and writers
//   shared_lock  -> shared:    multiple readers proceed concurrently
//
// Why not std::mutex? A plain mutex would serialise every snapshot()
// call even when two readers hit it simultaneously. shared_mutex lets
// all readers proceed in parallel — only the writer blocks everyone.
//
// Performance note: shared_mutex has higher overhead than a plain mutex
// when there is actually contention. In our case: reads would be frequent
// (~10Hz UI) but short (copy 20 structs), and writes rare (~1/lap).
// shared_mutex pays off here.
//
// The `mutable` keyword: allows snapshot() to be `const` (logically
// read-only) while still locking the mutex (which modifies internal
// lock state). Without mutable, a const method cannot lock a mutex.

class Leaderboard {
public:
    // Called by the simulation thread — takes exclusive lock.
    void update(std::vector<DriverState> sorted_standings) {
        std::unique_lock lock{mutex_};
        standings_ = std::move(sorted_standings);
    }

    // Called by any reader — takes shared lock.
    // Returns a full copy so the caller doesn't need to hold the lock.
    std::vector<DriverState> snapshot() const {
        std::shared_lock lock{mutex_};
        return standings_;
    }

    // Returns the number of drivers currently tracked.
    std::size_t size() const {
        std::shared_lock lock{mutex_};
        return standings_.size();
    }

    // Returns the driver at a given position (1-indexed).
    // Returns a default-constructed DriverState if position is out of range.
    DriverState at_position(int pos) const {
        std::shared_lock lock{mutex_};
        for (const auto& s : standings_) {
            if (s.position == pos) return s;
        }
        return {};
    }

    // Returns the best lap holder's driver id (empty string if none set yet).
    std::string fastest_lap_holder() const {
        std::shared_lock lock{mutex_};
        float best = std::numeric_limits<float>::max();
        std::string holder;
        for (const auto& s : standings_) {
            if (s.best_lap_ms > 0.0f && s.best_lap_ms < best) {
                best   = s.best_lap_ms;
                holder = s.profile.id;
            }
        }
        return holder;
    }

private:
    std::vector<DriverState>  standings_;
    mutable std::shared_mutex mutex_;
};