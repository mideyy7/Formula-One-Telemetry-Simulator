#pragma once

#include <atomic>
#include <array>
#include <cstddef>

template<typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2, "Capacity must be at least 2");

    // padding prevents false sharing btw head_ and tail_
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};

    std::array<T, Capacity> buffer_{};

public:
    SpscQueue() = default;
    
    // Not copyabble because of atomics
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;


    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    // approximate size
    std::size_t size_approx() const {
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto head = head_.load(std::memory_order_acquire);
        return (tail - head + Capacity) % Capacity;
    }

    // Called by Producer and returns false if queue is full( caller should spin or drop)
    bool push(T& item) {
        // relaxed: read tail_ here only, no ordering needed on index
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = (tail + 1) % Capacity;

        // aquire: synchronise with the consumer's release store on head_.
        if (next == head_.load(std::memory_order_acquire)) return false;

        buffer_[tail] = item;

        // release: make the item write visible to the consumer tail_ is updated. 
        // Without release here, the compiler or CPU could reorder the item store after the tail_ store, letting the consumer read uninitialised data.
        tail_.store(next, std::memory_order_release);
        return true;

    }

    // push() overload for semantics
    bool push(T&& item) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = (tail_ + 1) % Capacity;
        if (next == head_.load(std::memory_order_acquire)) return false;
        buffer_[tail_] = std::move(item);
        tail_.store(next, std::memory_order_release);
        return true;
    }


    // Consumer calls this and returns false if queue is empty
    bool pop(T& out) {
        const auto head = head_.load(std::memory_order_acquire);
        const auto next = (head_ + 1) % Capacity;

        // aquire: synchronise with the producer's release store on tail_
        // ensure the item is fully written before read
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = buffer_[head];
        
        // release: tell the producer that the slot is free
        head_.store(next, std::memory_order_acquire);
        return true; 
    }
};

