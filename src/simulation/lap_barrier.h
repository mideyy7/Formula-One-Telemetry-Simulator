#pragma once

#include <barrier>
#include <functional>

// LapBarrier wraps std::barrier for end-of-lap synchronisation.
//
// std::barrier: counts down each "phase", runs a completion function once
// everyone has arrived, then RESETS and can be used again next phase.
// That auto-reset is the key difference from std::latch (one-shot only).
//
// Usage pattern (if drivers ran on separate threads):
//   LapBarrier barrier{20, [&]{ sort_standings(); }};
//   // In each driver thread, at lap end:
//   barrier.arrive_and_wait(); // blocks until all 20 have arrived
//   // Completion function runs ONCE, then all 20 threads release together.
//
// NOTE: In PitWall's current design, all 20 drivers are updated in a loop
// on a single producer thread (see TelemetryGenerator::tick()), so nothing
// actually needs to block on a barrier yet. This class exists as a working,
// tested example of the primitive — if per-driver work is ever split across
// its own thread, this is the synchronisation piece to reach for.
//
// std::barrier requires its completion function to never throw. Apple's
// libc++ can't compile std::function<void() noexcept> (the "this type must
// not throw" spelled directly into the type), so we store a normal
// std::function<void()> and wrap the actual call in a lambda marked
// noexcept below — same guarantee, different spelling.

class LapBarrier {
public:
    explicit LapBarrier(std::ptrdiff_t count,
                         std::function<void()> on_completion = {})
        : barrier_{count,
            [cb = std::move(on_completion)]() noexcept {
                if (cb) cb();
            }}
    {}

    // Called by each thread at the lap boundary. Blocks until everyone
    // registered with the barrier has also called this.
    void arrive_and_wait() { barrier_.arrive_and_wait(); }

    // Non-blocking arrival: registers this thread as arrived but returns
    // immediately instead of waiting for the others.
    [[nodiscard]] auto arrive() { return barrier_.arrive(); }

private:
    std::barrier<std::function<void()>> barrier_;
};