# Testing and contributing

## Local validation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The standalone suite uses explicit checks, not `assert`, so Release builds retain
all validation. Coordination uses latches/barriers; priority tests pause the pool
before enqueueing. CTest deadlines turn deadlocks into failed jobs. Stress tests
race four producers against drain/cancel across 60 rounds and 1–8 workers, validating
240,000 admission attempts and every returned future.

| Area | Checks |
| --- | --- |
| Task values | Value, void, reference, member functions, move-only arguments/captures, borrowed references |
| Failures | Standard/nonstandard exceptions, worker recovery, failure counters |
| Scheduling | Strict priorities, FIFO with one worker, pause/resume, bounded admission |
| Lifecycle | Destructor drain, paused drain, repeat/concurrent shutdown, drain-to-cancel escalation |
| Cancellation | Discarded futures, running stop-aware tasks, callbacks, capture destruction outside lock |
| Nested work | Single-worker descendants, saturated multiworker descendants, nested failures, deferred futures, worker wait/join guards |
| Observability | Accounting invariants, rejection counts, logger file contents |
| Stress | Concurrent admission versus shutdown; accepted/rejected and executed/cancelled partition |
| Packaging | Install then build/run an independent `find_package` consumer in CI |

Thread creation failure cleanup is reviewed in code; deterministic OS-resource
exhaustion and allocator-failure injection are not part of this suite. Self-destruction
termination is a documented precondition failure rather than an ordinary task test.
Testing and sanitizers increase confidence but cannot prove absence of every race.

## Sanitizers

Use separate build directories; do not combine ThreadSanitizer with AddressSanitizer.

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DTHREADPOOL_SANITIZER=address
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DTHREADPOOL_SANITIZER=thread
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

`undefined` is also supported on GCC/Clang. CI tests GCC and Clang on Linux,
AppleClang with the default Xcode 26 SDK on `macos-26`, MSVC on Windows,
Debug/Release configurations, and separate
Linux sanitizer jobs. Instrumented builds are for verification, not benchmarking.
A sanitizer startup failure caused by the host runtime is not a passing race check.

The macOS jobs select AppleClang through `xcrun` for both the library and installed
consumer and print the toolchain versions. The former `macos-15` default SDK lacked
the required stop-token types. Every library configure now compiles and links a
probe using stop tokens, callbacks and `std::jthread`; an unsupported library fails
early with a toolchain diagnostic instead of failing during the main build.

## Benchmarking

```sh
./build/threadpool_benchmark 100000 4
```

The benchmark emits CSV: threads, tasks, seconds, tasks per second, checksum. It
includes submission, future delivery, and completion; excludes pool construction;
and disables logging. Each task performs 100 unsigned arithmetic iterations.
Run an optimized build, repeat trials, and record CPU, compiler, flags, power mode,
and competing load. This is a diagnostic harness, not evidence of being universally
fastest. Compare representative application workloads and queue bounds before tuning.

## Contribution checklist

Document changes to ownership, ordering, cancellation or shutdown guarantees. Add
focused behavioral tests and use synchronization primitives to expose the relevant
interleaving rather than depending on sleeps. Run the normal suite, appropriate
sanitizers, documentation build, and installed-package check. Avoid changing the
vendored logger without recording a deliberate upstream/version update.
