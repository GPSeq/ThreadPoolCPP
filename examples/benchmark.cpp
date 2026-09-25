#include <threadpool/thread_pool.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

// A reproducible workload, not a claim that any one executor is universally fastest.
int main(int argc, char** argv) {
    try {
        const auto count = argc > 1 ? std::stoull(argv[1]) : 100000ULL;
        const auto threads = argc > 2 ? std::stoull(argv[2]) : 0ULL;
        if (!count || count > 10000000 || threads > 1024)
            throw std::invalid_argument("use 1..10000000 tasks and 0..1024 threads");
        threadpool::Options options; options.threads = static_cast<std::size_t>(threads); options.log_level.clear();
        threadpool::ThreadPool pool(options);
        std::vector<std::future<std::uint64_t>> futures; futures.reserve(static_cast<std::size_t>(count));
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < count; ++i) futures.push_back(pool.submit([i] {
            auto x = i;
            for (int n = 0; n < 100; ++n) x = x * 6364136223846793005ULL + 1;
            return x;
        }));
        std::uint64_t checksum = 0;
        for (auto& f : futures) checksum += f.get();
        pool.wait_idle();
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "threads,tasks,seconds,tasks_per_second,checksum\n" << pool.thread_count() << ','
                  << count << ',' << seconds << ',' << static_cast<double>(count) / seconds << ',' << checksum << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
