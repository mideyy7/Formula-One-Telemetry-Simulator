#include "concurrency/thread_pool.h"

ThreadPool::ThreadPool(std::size_t num_threads) {
    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock lock{mutex_};
        stop_ = true;
    }
    cv_.notify_all(); // wake every sleeping worker so they can exit
    for (auto& t : workers_) {
        t.join();
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock lock{mutex_};
            // Wait until there's a task OR the pool is stopping.
            cv_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });

            if (stop_ && tasks_.empty()) {
                return; // clean exit
            }

            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        // Execute the task OUTSIDE the lock so other workers can dequeue.
        task();
    }
}