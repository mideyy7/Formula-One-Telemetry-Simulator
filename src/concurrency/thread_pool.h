#pragma once

#include <vector>
#include <deque>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <stdexcept>


// Fixed-size thread pool with a shared FIFO task queue.
// Used for: strategy analysis, weather updates, race-control processing.

class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit a callable. Returns a future for the result.
    // The callable is executed on one of the worker threads.
    // Throws std::runtime_error if called after the pool is stopped.
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    std::size_t thread_count() const { return workers_.size(); }

private:
    std::vector<std::thread>          workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    bool                              stop_{false};

    void worker_loop();
};

// ─── Template method must live in the header ──────────────────────────────────

template <typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>
{
    using ReturnType = std::invoke_result_t<F, Args...>;

    // packaged_task wraps the callable and gives us a future.
    // We heap-allocate it (shared_ptr) so the lambda below can capture it
    // without copying (packaged_task is not copyable).
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [f = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
            return f(std::forward<Args>(args)...);
        }
    );

    std::future<ReturnType> result = task->get_future();

    {
        std::unique_lock lock{mutex_};
        if (stop_) {
            throw std::runtime_error("submit() called on a stopped ThreadPool");
        }
        // Wrap the packaged_task in a void() lambda for the task queue.
        tasks_.emplace_back([task]() { (*task)(); });
    }

    cv_.notify_one(); // wake one sleeping worker
    return result;
}