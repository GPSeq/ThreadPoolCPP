#include <threadpool/thread_pool.hpp>
#include <iostream>
#include <latch>
#include <thread>

int main() {
    threadpool::Options options; options.threads = 1;
    threadpool::ThreadPool pool(options);
    std::latch started(1);
    auto running = pool.submit_stoppable([&](std::stop_token token) {
        started.count_down();
        while (!token.stop_requested()) std::this_thread::yield();
        return "cooperatively stopped";
    });
    started.wait();
    auto queued = pool.submit([] { return "never started"; });
    pool.shutdown(threadpool::ShutdownMode::cancel);
    std::cout << running.get() << '\n';
    try { std::cout << queued.get() << '\n'; }
    catch (const threadpool::task_cancelled& error) { std::cout << error.what() << '\n'; }
}
