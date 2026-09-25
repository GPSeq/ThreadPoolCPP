# Design and concurrency contract

## Architecture

A compiled C++20 library keeps logger dependencies out of public headers. The public
header owns typed task wrappers; each holds a promise and a decay-captured invocation.
A virtual task interface provides move-only type erasure without requiring C++23
`std::move_only_function`. Each accepted task has one owner and one execution path.

Workers share three FIFO linked queues behind one mutex. A condition variable sleeps
idle workers. Queue admission, dequeue, pause, closure, and statistics transitions
are serialized by this mutex. User callables, capture destruction, stop callbacks,
and logger calls execute outside it. A separate mutex serializes thread joins.

The linked queues let cancellation splice all waiting tasks into a local list
without allocating and without destroying user captures while holding the queue
mutex. Cancellation settles futures, releases captures, and updates statistics
before requesting stop. This order allows stop callbacks to inspect the futures
cancelled by that request. Concurrent requests may have separate in-flight cleanup.

## Lifecycle and linearization

1. Construction initializes state/logging and starts the requested workers. If
   starting a worker fails, already-created workers are awakened and joined before
   the constructor rethrows. No partially constructed pool escapes.
2. Submission linearizes when the task is appended under the queue mutex. Closure
   and bounded-capacity checks use the same mutex. A racing submission is either
   accepted exactly once or rejected; it cannot disappear between those states.
3. Dequeue transfers unique ownership to one worker and counts an active frame.
   There is no preemption once dequeued. A promise publishes the result/exception,
   then capture destruction and accounting finish the frame.
4. Shutdown closes admission under the same mutex and unpauses workers. Drain leaves
   the queues intact. Cancel removes them and requests the shared stop source.
5. Workers exit after closure when no queued work remains. Joining waits for their
   active frames; idle waiting also covers cancellation cleanup on other callers.

Futures provide result synchronization. Queue state and counters are always accessed
under the mutex. Worker identification uses a thread-local implementation pointer;
thread count and logger ownership remain immutable after construction. No detached
threads outlive the pool. C++ object lifetime rules still apply: concurrent destruction
and member access are unsupported even though ordinary member calls are synchronized.

## Deliberate limits

This pool does not promise hard deadlines, lock freedom, fairness across priorities,
forced task interruption, automatic resizing, work stealing, individual task handles,
continuations, timers, coroutines, or a standard executors/senders implementation.
No API can make arbitrary cyclic dependencies safe. Fixed counts avoid the ambiguous
lifetime/ordering behavior of resizing while tasks are running.

Central queues favor auditability and priority semantics but contend under heavy
multi-producer tiny-task workloads. Every submitted task allocates a task wrapper,
future shared state, and queue node. Batch small operations inside a task when
scheduling overhead dominates useful work. A specialized work-stealing executor may
be more appropriate for fine-grained recursive parallelism; an asynchronous I/O
runtime may be more appropriate for many mostly-waiting connections.

`await` makes nested acyclic work possible through reentrant execution; it does not
provide structured concurrency or a general task dependency graph. Ordinary future
waits and application mutexes remain the application's responsibility. Capture
destructors and stop callbacks must not perform blocking pool lifecycle operations.

## Dependency policy

The requested cpp_manual_logger is vendored with its MIT license and exact
commit recorded in `third_party/NOTICE.md`. Its upstream build references a missing
package template, so only the header is integrated. spdlog is resolved from an installed
CMake package or fetched at a full commit SHA; its bundled fmt is the default. Installed
spdlog variants using external fmt propagate their own dependency through CMake.
One documented timestamp compatibility patch supports fmt 12, which removed the
logger's `fmt::localtime` helper. Dependency updates should refresh notices and run the complete CI matrix.
