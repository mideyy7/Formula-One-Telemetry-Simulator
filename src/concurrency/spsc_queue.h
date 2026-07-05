#pragma once

#include <atomic>
#include <array>
#include <cstddef>

// Single-Producer Single-Consumer bounded ring buffer.
// Used for: telemetry generator → display consumer pipeline.
//
// Cache optimization: each side caches the other's index.
//   Producer writes tail_ and caches head_ in head_cache_.
//   Consumer writes head_ and caches tail_ in tail_cache_.
//
// In steady-state (queue neither full nor empty), the producer never
// needs to read head_ across the cache-line boundary, and the consumer
// never needs to read tail_. Cross-thread atomic traffic drops from
// 2 per round-trip to near-zero.

template<typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2 (enables bitmask instead of modulo)");

    static constexpr std::size_t MASK = Capacity - 1;

    // ── Producer-owned cache line ──────────────────────────────────────────
    // Producer writes tail_ and reads head_cache_ (stale copy of consumer's head_).
    // Colocating them prevents the producer from touching the consumer's cache line
    // on the common path (queue not full).
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_{0};

    // ── Consumer-owned cache line ──────────────────────────────────────────
    // Consumer writes head_ and reads tail_cache_ (stale copy of producer's tail_).
    alignas(64) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_{0};

    std::array<T, Capacity> buffer_{};

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    bool empty() const {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    std::size_t size_approx() const {
        const auto t = tail_.load(std::memory_order_relaxed);
        const auto h = head_.load(std::memory_order_relaxed);
        return (t - h) & MASK;
    }

    // Called by producer only.
    bool push(const T& item) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = (tail + 1) & MASK;

        // Fast path: head_cache_ says there is space — no cross-thread read.
        // Slow path: only when next == head_cache_ do we refresh from the real head_.
        if (next == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (next == head_cache_) return false; // truly full
        }

        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool push(T&& item) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = (tail + 1) & MASK;
        if (next == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (next == head_cache_) return false;
        }
        buffer_[tail] = std::move(item);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Called by consumer only.
    bool pop(T& out) {
        const auto head = head_.load(std::memory_order_relaxed);

        // Fast path: tail_cache_ says there is data — no cross-thread read.
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false; // truly empty
        }

        out = buffer_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }
};