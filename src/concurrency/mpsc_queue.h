#pragma once

#include <memory>
#include <atomic>

// Multi-Producer Single-Consumer unbounded queue.
// Used for: race control events (track limits, weather, safety car) → UI panel.

template<typename T>
class MpscQueue {
    struct Node {
        T data {};
        std::atomic<Node*> next {nullptr};
    };
    std::atomic<Node*> head_;  // head_ is the insertion point; producers race to update it
    Node* tail_;  // tail_ is the consumption point; only the consumer ever reads/writes it.  No atomic needed — single-threaded access.

public:
    MpscQueue() {
        Node* sentinel = new Node{};
        head_.store(sentinel, std::memory_order_relaxed);
        tail_ = sentinel;
    }

    ~MpscQueue() {
        // drain any remaining items then delete the sentinel
        T ignored;
        while (pop(ignored)) {}
        delete tail_; // tail is now the sentinel
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // Called by ANY thread (multiple producers safe).
    void push(T value) {
        Node* new_node = new Node{std::move(value)};

        // atomically swap head_ with new_node
        // prev_head is the node that was the head before the push
        // acq_rel: acquire to see prior pushes and release to publish current push
        Node* prev_head = head_.exchange(new_node, std::memory_order_acq_rel);

        // link prev_head to new node
        // release: the consumer's aquire load on next will see this link
        prev_head->next.store(new_node, std::memory_order_release);
    }

    // Called ONLY by the single consumer thread.
    // Returns false if the queue is empty or a push is mid-flight.
    bool pop(T& out) {
        Node* next = tail_->next.load(std::memory_order_acquire);
        if (!next) return false;  // empty or push is between exchange and store

        out = std::move(next->data);
        Node* old_tail = tail_;
        tail_ = next;  // advance: next becomes the new sentinel
        delete old_tail;
        return true;
    }

    bool empty() const {
        return tail_->next.load(std::memory_order_acquire) == nullptr;
    }



};