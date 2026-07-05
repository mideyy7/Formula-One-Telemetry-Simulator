#pragma once

#include <semaphore>
#include <chrono>

// PitLane limits how many cars can be serviced simultaneously.
//
// std::counting_semaphore<N>: an internal counter starting at N.
//   acquire() decrements it — blocks the calling thread if it's already 0.
//   release() increments it — wakes up one thread that was blocked in acquire().
//
// A std::mutex is really just a counting_semaphore<1> in disguise: it has
// exactly one "slot". Use a mutex when you have ONE exclusive resource
// (like a single shared data structure). Use a counting semaphore when you
// have N interchangeable resources (like N pit boxes) and any thread can
// use any one of them.
//
// This models the real F1 rule that limits how many cars can be actively
// serviced in the pit lane at once; we simplify to a global cap of 2.

class PitLane {
public:
    // Enter the pit lane. Blocks if both slots are already occupied.
    void enter() { capacity_.acquire(); }

    // Try to enter, giving up after `timeout`. Returns true if a slot was
    // acquired, false if the timeout elapsed first.
    bool try_enter(std::chrono::milliseconds timeout) {
        return capacity_.try_acquire_for(timeout);
    }

    // Leave the pit lane, freeing one slot for another car.
    void exit() { capacity_.release(); }

private:
    // The template parameter <2> is a compile-time MAXIMUM value.
    // The {2} in the initializer is the runtime STARTING count.
    std::counting_semaphore<2> capacity_{2};
};