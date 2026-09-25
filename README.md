# ThreadPoolCPP

[![Tests](https://github.com/GPSeq/ThreadPoolCPP/actions/workflows/tests.yml/badge.svg?branch=main)](https://github.com/GPSeq/ThreadPoolCPP/actions/workflows/tests.yml)
[![Docs](https://github.com/GPSeq/ThreadPoolCPP/actions/workflows/docs.yml/badge.svg?branch=main)](https://github.com/GPSeq/ThreadPoolCPP/actions/workflows/docs.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/build-CMake-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![Doxygen](https://img.shields.io/badge/docs-Doxygen-2C4AA8)](https://www.doxygen.nl/)

A portable C++20 thread pool for applications that need clear task ownership,
predictable shutdown, and observable failures. Built for Linux, macOS and Windows.
Logging uses [cpp_manual_logger](https://github.com/lutfia95/cpp_manual_logger).

- Futures carry values, references, `void`, and exceptions; tasks and arguments may be move-only.
- Fixed worker count, optional bounded queue, throwing or optional-returning admission.
- Three priorities with FIFO dequeue order within each priority, plus pause/resume.
- Graceful drain, queued-task cancellation, and cooperative stop for running work.
- Worker-aware `await` supports acyclic nested tasks, including single-worker pools.
- Consistent statistics, timed idle waits, RAII cleanup, and concurrent shutdown callers.
- Correctness/stress tests, sanitizer CI, installable CMake package, and Doxygen guides.

No pool is best for every workload. This implementation favors explicit guarantees
and maintainable synchronization. It is not a real-time scheduler or asynchronous
I/O runtime. See the [design and limits](docs/design.md) before choosing it for tiny
tasks or dependency-heavy workloads.

## Quick start

Requires CMake 3.24+, a compiler and standard library supporting C++20 stop tokens,
and threads. CI targets GCC 13, Clang 18, AppleClang, and Visual Studio 2022.
CMake uses installed fmt-based spdlog >= 1.15.3 or fetches a pinned version on first configure.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/threadpool_basic
```

For Visual Studio, add `--config Release` to build and `-C Release` to CTest;
examples are under `build/Release/`.

```cpp
#include <threadpool/thread_pool.hpp>
#include <iostream>

int main() {
    threadpool::Options options;
    options.threads = 4;
    options.queue_capacity = 1024;
    threadpool::ThreadPool pool(options);

    auto answer = pool.submit([](int x) { return x * 2; }, 21);
    std::cout << answer.get() << '\n';
} // Drains accepted tasks and joins every worker.
```

## Integrate

Use `add_subdirectory(ThreadPoolCPP)` and link `ThreadPoolCPP::threadpool`, or install:

```sh
cmake --install build --prefix "$PWD/install"
```

```cmake
find_package(ThreadPoolCPP 1 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE ThreadPoolCPP::threadpool)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix. The package requires spdlog;
the default fetched dependency is installed alongside it. If using a system spdlog,
consumers must also be able to find that package. For offline builds install spdlog
and set `THREADPOOL_FETCH_SPDLOG=OFF`, or supply a local spdlog source tree through
`FETCHCONTENT_SOURCE_DIR_SPDLOG`.

## Documentation

- [Usage guide](docs/guide.md): admission, futures, priorities, cancellation, logging.
- [Design and concurrency contract](docs/design.md): lifecycle, invariants, boundaries.
- [Testing and contributing](docs/testing.md): test coverage, sanitizers, benchmarking.
- [Published API documentation](https://GPSeq.github.io/ThreadPoolCPP/) (available after the Docs workflow deploys).

With Doxygen installed:

```sh
cmake -S . -B build
cmake --build build --target docs
```

Open `build/docs/html/index.html`. The Docs workflow builds documentation for pull
requests and deploys `main` to GitHub Pages. Set the repository's **Settings → Pages
→ Source** to **GitHub Actions** to enable deployment.

## License

MIT. See [LICENSE](LICENSE) and [third-party notices](third_party/NOTICE.md).
