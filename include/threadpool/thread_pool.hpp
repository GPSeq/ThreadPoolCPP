#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

/** @file thread_pool.hpp
 * @brief C++20 task pool with futures, bounded admission and cooperative stop.
 */
namespace threadpool {

/// Higher priorities are dequeued first; equal priorities are FIFO.
enum class Priority { high, normal, low };
/// Drain executes queued work; cancel discards queued work and requests cooperative stop.
enum class ShutdownMode { drain, cancel };
/// A task was discarded before execution by cancel shutdown.
class task_cancelled : public std::runtime_error {
public:
    task_cancelled() : std::runtime_error("task cancelled before execution") {}
};
/// Submission raced with, or followed, shutdown.
class pool_closed : public std::runtime_error {
public:
    pool_closed() : std::runtime_error("thread pool is closed") {}
};
/// A bounded queue has no available slot.
class queue_full : public std::runtime_error {
public:
    queue_full() : std::runtime_error("thread pool queue is full") {}
};

/// Pool construction options. Logging is enabled by default; empty log_level disables it.
struct Options {
    std::size_t threads = 0; ///< Zero uses hardware_concurrency, with a fallback of one.
    std::size_t queue_capacity = 0; ///< Zero is unbounded; running tasks do not consume slots.
    std::string log_name = "ThreadPoolCPP"; ///< ManuelLogger application name.
    std::string log_level = "info"; ///< Logger level; empty disables all logging.
    std::string log_file; ///< Optional file; upstream logger truncates it at construction.
};

/// Consistent snapshot. accepted == queued + active + completed + cancelled + cancelling.
struct Statistics {
    std::size_t accepted = 0;
    std::size_t completed = 0; ///< Executed tasks, including failures.
    std::size_t failed = 0; ///< Subset of completed whose execution/result delivery threw.
    std::size_t cancelled = 0;
    std::size_t rejected = 0; ///< Closed/full admission attempts (not argument-construction failures).
    std::size_t queued = 0;
    std::size_t active = 0;
    std::size_t cancelling = 0; ///< Discarded tasks whose futures/captures are being settled.
};

namespace detail {
struct Task {
    virtual ~Task() = default;
    virtual bool run() noexcept = 0;
    virtual void cancel() noexcept = 0;
};
template<class R, class F> struct FutureTask final : Task {
    F function;
    std::promise<R> promise;
    explicit FutureTask(F&& f) : function(std::move(f)) {}
    bool run() noexcept override {
        try {
            if constexpr (std::is_void_v<R>) {
                std::invoke(std::move(function));
                promise.set_value();
            } else {
                promise.set_value(std::invoke(std::move(function)));
            }
            return true;
        } catch (...) {
            promise.set_exception(std::current_exception());
            return false;
        }
    }
    void cancel() noexcept override {
        try { throw task_cancelled{}; }
        catch (...) { promise.set_exception(std::current_exception()); }
    }
};
template<class F, class... Args>
auto bind_once(F&& f, Args&&... args) {
    return [fn = std::decay_t<F>(std::forward<F>(f)),
            values = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]() mutable
            -> std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...> {
        return std::apply([&fn](auto&&... xs) -> decltype(auto) {
            return std::invoke(std::move(fn), std::forward<decltype(xs)>(xs)...);
        }, std::move(values));
    };
}
} // namespace detail

/** @brief Fixed-size, thread-safe executor.
 *
 * Nonblocking admission avoids producer deadlocks on bounded queues. Member calls may
 * run concurrently, except destruction, which requires exclusive ownership. Destruction
 * drains and joins. Destroying the pool on its own worker terminates the process.
 * Call await() instead of future.get() for dependencies inside this pool's tasks.
 */
class ThreadPool {
public:
    explicit ThreadPool(Options options = {});
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Submit normal-priority work. Arguments are decay-copied/moved; use std::ref to borrow.
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args) {
        return submit_with_priority(Priority::normal, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /// Submit at a selected priority. Throws pool_closed or queue_full on rejection.
    template<class F, class... Args>
    auto submit_with_priority(Priority priority, F&& f, Args&&... args) {
        auto result = enqueue_bound(priority, detail::bind_once(std::forward<F>(f), std::forward<Args>(args)...), false);
        return std::move(*result);
    }

    /// Nonblocking admission; nullopt means closed/full. Other construction errors propagate.
    template<class F, class... Args>
    auto try_submit(F&& f, Args&&... args) {
        return enqueue_bound(Priority::normal, detail::bind_once(std::forward<F>(f), std::forward<Args>(args)...), true);
    }

    /// Invoke f(pool_stop_token, args...). Only cancel shutdown requests this token.
    template<class F, class... Args>
    auto submit_stoppable(F&& f, Args&&... args) {
        return submit(std::forward<F>(f), stop_token(), std::forward<Args>(args)...);
    }

    /// Wait/get a future; on this pool's worker execute queued work while waiting.
    /// This permits acyclic nested tasks, but introduces reentrancy. Do not hold task locks.
    template<class T>
    decltype(auto) await(std::future<T>& future) {
        if (!future.valid()) throw std::future_error(std::future_errc::no_state);
        if (is_worker_thread()) {
            while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::deferred) break;
                if (!help_one()) future.wait_for(std::chrono::milliseconds(1));
            }
        }
        return future.get();
    }

    /// Prevent dequeueing. Already-running tasks continue. Throws after shutdown starts.
    void pause();
    /// Allow dequeueing again. Harmless after shutdown.
    void resume();
    /// Wait for queued/running/cancelling work to settle. Throws on this pool's workers.
    /// A snapshot barrier only: concurrent producers can enqueue immediately afterwards.
    void wait_idle();
    /// Timed idle wait; same restrictions as wait_idle().
    bool wait_idle_for(std::chrono::milliseconds timeout);
    /// Close admission and initiate shutdown without joining; safe to call from a task.
    /// Cancel can escalate drain; drain never reverses cancel. Stop callbacks run inline.
    void request_shutdown(ShutdownMode mode = ShutdownMode::drain);
    /// Initiate shutdown and join. Concurrent calls are safe; throws on this pool's workers.
    void shutdown(ShutdownMode mode = ShutdownMode::drain);
    [[nodiscard]] Statistics statistics() const;
    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] bool is_worker_thread() const noexcept;
    [[nodiscard]] std::stop_token stop_token() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool enqueue(Priority priority, std::unique_ptr<detail::Task> task, bool allow_rejection);
    bool help_one();
    template<class F>
    auto enqueue_bound(Priority priority, F&& function, bool allow_rejection) {
        using R = std::invoke_result_t<F>;
        auto task = std::make_unique<detail::FutureTask<R, F>>(std::forward<F>(function));
        auto future = task->promise.get_future();
        if (!enqueue(priority, std::move(task), allow_rejection)) return std::optional<std::future<R>>{};
        return std::optional<std::future<R>>{std::move(future)};
    }
};
} // namespace threadpool
