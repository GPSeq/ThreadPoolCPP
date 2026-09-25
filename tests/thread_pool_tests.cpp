#include <threadpool/thread_pool.hpp>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <sstream>
#include <thread>
#include <vector>

using namespace threadpool;
using namespace std::chrono_literals;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("check failed: " #expr); } while (false)
template<class E, class F> void throws(F&& f) {
    bool caught = false;
    try { f(); } catch (const E&) { caught = true; }
    CHECK(caught);
}
Options quiet(std::size_t n = 2, std::size_t capacity = 0) {
    Options options; options.threads = n; options.queue_capacity = capacity; options.log_level.clear(); return options;
}
void invariant(const Statistics& s) {
    CHECK(s.accepted == s.queued + s.active + s.completed + s.cancelled + s.cancelling);
    CHECK(s.failed <= s.completed);
}
void values_and_arguments() {
    ThreadPool pool(quiet());
    CHECK(pool.submit([] { return 42; }).get() == 42);
    pool.submit([] {}).get();
    auto value = std::make_unique<int>(12);
    CHECK(*pool.submit([](std::unique_ptr<int> p) { return p; }, std::move(value)).get() == 12);
    CHECK(pool.submit([p = std::make_unique<int>(7)] { return *p; }).get() == 7);
    int original = 3;
    pool.submit([](int& x) { x = 9; }, std::ref(original)).get();
    CHECK(original == 9);
    CHECK(&pool.submit([&]() -> int& { return original; }).get() == &original);
    struct Object { int x = 11; int add(int y) const { return x + y; } } object;
    CHECK(pool.submit(&Object::add, &object, 5).get() == 16);
    auto bad = pool.submit([]() -> int { throw std::runtime_error("sentinel"); });
    throws<std::runtime_error>([&] { bad.get(); });
    auto nonstandard = pool.submit([] { throw 17; });
    throws<int>([&] { nonstandard.get(); });
    CHECK(pool.submit([] { return true; }).get());
    pool.wait_idle();
    CHECK(pool.statistics().failed == 2);
    invariant(pool.statistics());
}
void priority_capacity_pause() {
    ThreadPool pool(quiet(1, 4));
    pool.pause();
    std::vector<int> order;
    auto low = pool.submit_with_priority(Priority::low, [&] { order.push_back(4); });
    auto normal = pool.submit([&] { order.push_back(3); });
    auto high1 = pool.submit_with_priority(Priority::high, [&] { order.push_back(1); });
    auto high2 = pool.submit_with_priority(Priority::high, [&] { order.push_back(2); });
    throws<queue_full>([&] { pool.submit([] {}); });
    CHECK(!pool.try_submit([] {}));
    CHECK(!pool.wait_idle_for(1ms));
    CHECK(pool.statistics().queued == 4);
    pool.resume(); pool.wait_idle();
    CHECK((order == std::vector<int>{1, 2, 3, 4}));
    CHECK(pool.statistics().rejected == 2);
    CHECK(pool.try_submit([] { return 5; })->get() == 5);
}
void drain_and_close() {
    std::atomic<int> finished = 0;
    std::vector<std::future<void>> futures;
    {
        ThreadPool pool(quiet()); pool.pause();
        for (int i = 0; i < 100; ++i) futures.push_back(pool.submit([&] { ++finished; }));
        pool.shutdown();
        CHECK(finished == 100);
        CHECK(!pool.stop_token().stop_requested());
        throws<pool_closed>([&] { pool.submit([] {}); });
        throws<pool_closed>([&] { pool.pause(); });
        CHECK(!pool.try_submit([] {}));
        pool.resume(); pool.shutdown();
        invariant(pool.statistics());
    }
    for (auto& f : futures) f.get();
    { ThreadPool pool(quiet()); pool.submit([&] { ++finished; }); }
    CHECK(finished == 101);
}
void cancellation() {
    ThreadPool pool(quiet(1));
    std::latch started(1);
    auto running = pool.submit_stoppable([&](std::stop_token token) {
        started.count_down();
        while (!token.stop_requested()) std::this_thread::yield();
        return 7;
    });
    started.wait();
    auto queued = pool.submit([] { return 99; });
    auto queued2 = pool.submit([] {});
    pool.shutdown(ShutdownMode::cancel);
    CHECK(running.get() == 7);
    throws<task_cancelled>([&] { queued.get(); });
    throws<task_cancelled>([&] { queued2.get(); });
    CHECK(pool.statistics().cancelled == 2);
    CHECK(pool.statistics().completed == 1);
    invariant(pool.statistics());
}
void escalation_and_concurrent_join() {
    ThreadPool pool(quiet(1));
    std::latch started(1);
    auto running = pool.submit_stoppable([&](std::stop_token token) {
        started.count_down();
        while (!token.stop_requested()) std::this_thread::yield();
    });
    started.wait();
    auto pending = pool.submit([] {});
    pool.request_shutdown();
    std::jthread a([&] { pool.shutdown(); });
    std::jthread b([&] { pool.shutdown(ShutdownMode::cancel); });
    a.join(); b.join(); running.get();
    throws<task_cancelled>([&] { pending.get(); });
    CHECK(pool.stop_token().stop_requested());
}
void nested_and_worker_guards() {
    ThreadPool pool(quiet(1));
    CHECK(!pool.is_worker_thread());
    auto outer = pool.submit([&] {
        CHECK(pool.is_worker_thread());
        throws<std::logic_error>([&] { pool.wait_idle(); });
        throws<std::logic_error>([&] { pool.wait_idle_for(1ms); });
        throws<std::logic_error>([&] { pool.shutdown(); });
        auto inner = pool.submit([&] {
            auto leaf = pool.submit([] { return 40; });
            return pool.await(leaf) + 1;
        });
        return pool.await(inner) + 1;
    });
    CHECK(outer.get() == 42);
    auto deferred = pool.submit([&] {
        auto f = std::async(std::launch::deferred, [] { return 3; });
        return pool.await(f);
    });
    CHECK(deferred.get() == 3);
    std::future<void> invalid;
    throws<std::future_error>([&] { pool.await(invalid); });
    pool.submit([&] { pool.request_shutdown(); }).get();
    pool.shutdown();
}
void saturated_nested_work() {
    ThreadPool pool(quiet(4));
    std::barrier parents_started(4);
    std::vector<std::future<int>> parents;
    for (int i = 0; i < 4; ++i) parents.push_back(pool.submit([&] {
        parents_started.arrive_and_wait();
        auto child = pool.submit([] { return 42; });
        return pool.await(child);
    }));
    for (auto& parent : parents) CHECK(parent.get() == 42);
    auto failing_parent = pool.submit([&] {
        auto child = pool.submit([]() -> int { throw std::runtime_error("child failure"); });
        return pool.await(child);
    });
    throws<std::runtime_error>([&] { failing_parent.get(); });
    pool.wait_idle();
    CHECK(pool.wait_idle_for(0ms));
    CHECK(pool.statistics().completed == 10);
    CHECK(pool.statistics().failed == 2);
    invariant(pool.statistics());
}
void callback_and_capture_lifetimes() {
    ThreadPool pool(quiet(1)); pool.pause();
    struct Capture {
        ThreadPool* pool;
        std::atomic<bool>* destroyed;
        ~Capture() { invariant(pool->statistics()); destroyed->store(true); }
    };
    std::atomic<bool> destroyed = false;
    auto capture = std::make_shared<Capture>(); capture->pool = &pool; capture->destroyed = &destroyed;
    auto f = pool.submit([capture] {}); capture.reset();
    std::atomic<bool> callback = false;
    std::stop_callback stop(pool.stop_token(), [&] {
        invariant(pool.statistics());
        CHECK(f.wait_for(0ms) == std::future_status::ready);
        pool.request_shutdown();
        callback = true;
    });
    pool.shutdown(ShutdownMode::cancel);
    CHECK(callback); CHECK(destroyed);
    throws<task_cancelled>([&] { f.get(); });
}
void capacity_excludes_running() {
    ThreadPool pool(quiet(1, 1));
    std::latch started(1), release(1);
    auto running = pool.submit([&] { started.count_down(); release.wait(); });
    started.wait();
    auto queued = pool.try_submit([] {});
    const bool admitted = queued.has_value();
    const bool full = !pool.try_submit([] {}).has_value();
    release.count_down();
    CHECK(admitted); CHECK(full);
    running.get(); queued->get();
}
void logging() {
    auto options = quiet(); options.log_level = "debug";
    const auto path = std::filesystem::temp_directory_path() /
        ("threadpool-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    options.log_file = path.string();
    { ThreadPool pool(options); auto f = pool.submit([] { throw 1; }); throws<int>([&] { f.get(); }); }
    std::ifstream input(path); std::stringstream data; data << input.rdbuf(); input.close();
    std::filesystem::remove(path);
    CHECK(data.str().find("Thread pool started") != std::string::npos);
    CHECK(data.str().find("Task failed") != std::string::npos);
    CHECK(data.str().find("Thread pool stopped") != std::string::npos);
}
void invalid_options_and_defaults() {
    auto options = quiet(); options.log_level = "invalid";
    throws<std::invalid_argument>([&] { ThreadPool pool(options); });
    ThreadPool pool(quiet(0)); CHECK(pool.thread_count() >= 1);
    throws<std::invalid_argument>([&] { pool.submit_with_priority(static_cast<Priority>(99), [] {}); });
    throws<std::invalid_argument>([&] { pool.request_shutdown(static_cast<ShutdownMode>(99)); });
    CHECK(pool.submit([] { return 1; }).get() == 1);
}
int main() {
    const std::pair<const char*, void(*)()> tests[] = {
        {"values and arguments", values_and_arguments}, {"priority capacity pause", priority_capacity_pause},
        {"drain and close", drain_and_close}, {"cancellation", cancellation},
        {"escalation concurrent join", escalation_and_concurrent_join}, {"nested worker guards", nested_and_worker_guards},
        {"saturated nested work", saturated_nested_work},
        {"callbacks capture lifetime", callback_and_capture_lifetimes}, {"capacity excludes running", capacity_excludes_running},
        {"logging", logging}, {"invalid options defaults", invalid_options_and_defaults}
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; return 1; }
        catch (...) { std::cerr << "FAIL " << name << ": unknown exception\n"; return 1; }
    }
}
