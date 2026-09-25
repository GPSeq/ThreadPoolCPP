#include <threadpool/thread_pool.hpp>
#include <iostream>
#include <memory>

int main() {
    threadpool::Options options; options.threads = 4; options.queue_capacity = 1024;
    threadpool::ThreadPool pool(options);
    auto value = pool.submit([](std::unique_ptr<int> n) { return *n * 2; }, std::make_unique<int>(21));
    auto nested = pool.submit([&pool] {
        auto child = pool.submit([] { return 7; });
        return pool.await(child); // Makes progress even when every worker has a child.
    });
    std::cout << value.get() << ", " << nested.get() << '\n';
    pool.shutdown();
}
