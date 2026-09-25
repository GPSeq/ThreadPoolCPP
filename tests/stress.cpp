#include <threadpool/thread_pool.hpp>
#include <atomic>
#include <barrier>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    using namespace threadpool;
    // Race producers against both shutdown modes. Each accepted future must settle
    // exactly once; each task's side effect must match its result/cancellation.
    for (int round = 0; round < 60; ++round) {
        Options options; options.threads = 1 + round % 8; options.queue_capacity = 32;
        options.log_level.clear();
        ThreadPool pool(options);
        std::atomic<unsigned> executed = 0, attempts = 0;
        std::barrier start(5);
        std::vector<std::future<void>> futures[4];
        std::vector<std::jthread> producers;
        for (int p = 0; p < 4; ++p) producers.emplace_back([&, p] {
            start.arrive_and_wait();
            for (int i = 0; i < 1000; ++i) {
                if (auto f = pool.try_submit([&] { executed.fetch_add(1, std::memory_order_relaxed); }))
                    futures[p].push_back(std::move(*f));
                ++attempts;
            }
        });
        start.arrive_and_wait();
        while (attempts.load() < 100) std::this_thread::yield();
        pool.shutdown(round % 2 ? ShutdownMode::drain : ShutdownMode::cancel);
        for (auto& p : producers) p.join();
        unsigned completed = 0, cancelled = 0;
        for (auto& list : futures) for (auto& f : list) {
            try { f.get(); ++completed; } catch (const task_cancelled&) { ++cancelled; }
        }
        const auto s = pool.statistics();
        if (completed != executed || completed != s.completed || cancelled != s.cancelled ||
            s.accepted != completed + cancelled || s.accepted + s.rejected != 4000 ||
            s.active || s.queued || s.cancelling || s.failed) {
            std::cerr << "Accounting/exactly-once failure in round " << round << '\n'; return 1;
        }
    }
    std::cout << "PASS 60 rounds of concurrent admission/shutdown (240000 attempts)\n";
}
