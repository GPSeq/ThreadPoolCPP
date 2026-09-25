#include <threadpool/thread_pool.hpp>
int main() {
    threadpool::Options options; options.threads = 2; options.log_level.clear();
    threadpool::ThreadPool pool(options);
    return pool.submit([] { return 42; }).get() == 42 ? 0 : 1;
}
