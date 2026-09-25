#include <threadpool/thread_pool.hpp>
#include <manuel_logger.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace threadpool {
struct ThreadPool::Impl {
    using Queue = std::list<std::unique_ptr<detail::Task>>;
    mutable std::mutex mutex;
    std::mutex join_mutex;
    std::condition_variable ready, idle;
    std::array<Queue, 3> queues;
    std::vector<std::thread> workers;
    std::stop_source stop;
    Statistics stats;
    std::size_t capacity, count;
    bool accepting = true, paused = false;
    std::unique_ptr<manuel::ManuelLogger> logger;
    static thread_local Impl* current;

    explicit Impl(const Options& options)
        : capacity(options.queue_capacity),
          count(options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency())) {
        if (!options.log_level.empty()) {
            logger = std::make_unique<manuel::ManuelLogger>(options.log_name, options.log_level, options.log_file);
        }
    }
    void log(const char* message, bool error = false) noexcept {
        try {
            if (logger) {
                if (error) logger->error(message);
                else logger->info(message);
            }
        } catch (...) { /* Diagnostics must never change task/shutdown semantics. */ }
    }
    bool is_idle() const { return stats.queued == 0 && stats.active == 0 && stats.cancelling == 0; }
    // Requires mutex; priority is strict and ordering is FIFO within a queue.
    std::unique_ptr<detail::Task> take() {
        for (auto& queue : queues) {
            if (!queue.empty()) {
                auto task = std::move(queue.front());
                queue.pop_front();
                --stats.queued;
                ++stats.active;
                return task;
            }
        }
        return {};
    }
    void execute(std::unique_ptr<detail::Task> task) noexcept {
        const bool success = task->run();
        task.reset(); // User captures are destroyed outside mutex, before idle is observable.
        if (!success) log("Task failed; exception stored in its future", true);
        {
            std::lock_guard lock(mutex);
            --stats.active;
            ++stats.completed;
            if (!success) ++stats.failed;
            if (is_idle()) idle.notify_all();
        }
    }
    void worker() noexcept {
        current = this;
        for (;;) {
            std::unique_ptr<detail::Task> task;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] { return !accepting || (!paused && stats.queued != 0); });
                if (!accepting && stats.queued == 0) break;
                task = take();
            }
            execute(std::move(task));
        }
        current = nullptr;
    }
};
thread_local ThreadPool::Impl* ThreadPool::Impl::current = nullptr;

ThreadPool::ThreadPool(Options options) : impl_(std::make_unique<Impl>(options)) {
    try {
        impl_->workers.reserve(impl_->count);
        for (std::size_t i = 0; i < impl_->count; ++i) {
            impl_->workers.emplace_back([this] { impl_->worker(); });
        }
    } catch (...) {
        request_shutdown();
        for (auto& worker : impl_->workers) worker.join();
        throw;
    }
    impl_->log("Thread pool started");
}
ThreadPool::~ThreadPool() {
    if (is_worker_thread()) std::terminate();
    shutdown();
}

bool ThreadPool::enqueue(Priority priority, std::unique_ptr<detail::Task> task, bool allow_rejection) {
    const auto index = static_cast<std::size_t>(priority);
    if (index >= impl_->queues.size()) throw std::invalid_argument("invalid task priority");
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->accepting) {
            ++impl_->stats.rejected;
            if (allow_rejection) return false;
            throw pool_closed{};
        }
        if (impl_->capacity && impl_->stats.queued >= impl_->capacity) {
            ++impl_->stats.rejected;
            if (allow_rejection) return false;
            throw queue_full{};
        }
        impl_->queues[index].push_back(std::move(task));
        ++impl_->stats.queued;
        ++impl_->stats.accepted;
    }
    impl_->ready.notify_one();
    return true;
}

bool ThreadPool::help_one() {
    std::unique_ptr<detail::Task> task;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->paused || impl_->stats.queued == 0) return false;
        task = impl_->take();
    }
    impl_->execute(std::move(task));
    return true;
}
void ThreadPool::pause() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->accepting) throw pool_closed{};
    impl_->paused = true;
}
void ThreadPool::resume() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->paused = false;
    }
    impl_->ready.notify_all();
}
void ThreadPool::wait_idle() {
    if (is_worker_thread()) throw std::logic_error("worker cannot wait for its own pool to become idle");
    std::unique_lock lock(impl_->mutex);
    impl_->idle.wait(lock, [this] { return impl_->is_idle(); });
}
bool ThreadPool::wait_idle_for(std::chrono::milliseconds timeout) {
    if (is_worker_thread()) throw std::logic_error("worker cannot wait for its own pool to become idle");
    std::unique_lock lock(impl_->mutex);
    return impl_->idle.wait_for(lock, timeout, [this] { return impl_->is_idle(); });
}
void ThreadPool::request_shutdown(ShutdownMode mode) {
    if (mode != ShutdownMode::drain && mode != ShutdownMode::cancel)
        throw std::invalid_argument("invalid shutdown mode");
    Impl::Queue discarded;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->accepting = false;
        impl_->paused = false;
        if (mode == ShutdownMode::cancel) {
            for (auto& queue : impl_->queues) discarded.splice(discarded.end(), queue);
            impl_->stats.cancelling += impl_->stats.queued;
            impl_->stats.queued = 0;
        }
    }
    impl_->ready.notify_all();
    // Complete discarded futures before invoking arbitrary stop callbacks.
    const auto cancelled = discarded.size();
    for (auto& task : discarded) task->cancel();
    discarded.clear();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stats.cancelling -= cancelled;
        impl_->stats.cancelled += cancelled;
        if (impl_->is_idle()) impl_->idle.notify_all();
    }
    if (mode == ShutdownMode::cancel) impl_->stop.request_stop();
}
void ThreadPool::shutdown(ShutdownMode mode) {
    if (is_worker_thread()) throw std::logic_error("worker cannot join its own pool; use request_shutdown");
    request_shutdown(mode);
    std::lock_guard join_lock(impl_->join_mutex);
    bool joined = false;
    for (auto& worker : impl_->workers) {
        if (worker.joinable()) {
            worker.join();
            joined = true;
        }
    }
    wait_idle();
    if (joined) impl_->log("Thread pool stopped");
}
Statistics ThreadPool::statistics() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}
std::size_t ThreadPool::thread_count() const noexcept { return impl_->count; }
bool ThreadPool::is_worker_thread() const noexcept { return Impl::current == impl_.get(); }
std::stop_token ThreadPool::stop_token() const noexcept { return impl_->stop.get_token(); }
} // namespace threadpool
