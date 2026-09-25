# Usage guide

## Choose the worker count and queue size

`Options::threads = 0` selects `hardware_concurrency()` with a fallback of one.
Set an explicit count for reproducibility. CPU-bound jobs often benefit from a count
near the CPU resources actually available to the application; the automatic choice
does not discover container quotas. Blocking jobs occupy workers until they return.

The default queue is unbounded. Set `queue_capacity` to bound **waiting** tasks;
running tasks and nested execution frames do not consume queue slots. A bounded
queue is not a total process-memory limit: producers construct task payloads before
admission, and callers retain futures/results. `submit` throws `queue_full` when
full and `pool_closed` when shutdown has begun. `try_submit` returns `std::nullopt`
for either condition. Both are nonblocking. Allocation, argument construction and
invalid-priority errors still propagate. Rejection may consume moved arguments.

```cpp
if (auto task = pool.try_submit([] { return 10; })) {
    use(task->get());
} else {
    // Apply an application-specific retry, shedding, or upstream backpressure policy.
}
```

Avoid unbounded busy retries. In particular, a worker that spins submitting to a full
queue can prevent progress. Queue capacity is shared across all three priorities.

## Ownership, futures and exceptions

Submission stores decayed copies/moves of the callable and its arguments. They are
invoked as rvalues exactly once if the task starts. Use `std::ref` for borrowed
references or lvalue-only functors. Borrowed objects and objects captured by reference
must outlive task execution and capture destruction. Returning a reference does not
extend the referent's lifetime. Member pointers work with `std::invoke` semantics.

```cpp
auto task = pool.submit([](std::unique_ptr<int> n) { return *n; },
                        std::make_unique<int>(42));
try {
    auto answer = task.get();
} catch (const std::exception& error) {
    // The original task exception is preserved.
}
```

Tasks throwing standard or nonstandard exceptions do not stop workers. Their futures
store the exception; failures are also logged and counted. Dropping a future neither
cancels its task nor waits for it. A future can outlive the pool. Captures are released
before the task is counted complete, although the future can become ready earlier.
Do not let capture destructors throw or wait for pool-wide completion.

## Nested tasks

Inside a task, use `pool.await(future)` to execute queued work while waiting for a
child. An ordinary `future.get()` cannot be intercepted and may exhaust all workers.

```cpp
auto parent = pool.submit([&pool] {
    auto child = pool.submit([] { return 21; });
    return 2 * pool.await(child);
});
```

`await` consumes the future like `get`. It also works with `void`, reference results,
exceptions and deferred `std::async` futures. Outside this pool's workers it simply
gets the result. The worker may run any eligible queued task, not necessarily that
future's producer. Do not hold locks across `await`: it is reentrant. Dependency
cycles, cross-pool waits, a paused pool, or a task waiting on its ancestor can still
deadlock. Deep nesting uses recursive stack space. Helping uses a 1 ms timed future
wait when no task is available; it is not a latency guarantee.

## Priorities, pausing and waiting

`submit_with_priority(Priority::high, callable, args...)` selects `high`, `normal`, or
`low`. Higher priority queues are checked first; equal-priority dequeue order is FIFO.
Execution and completion order can differ with multiple workers. Strict priority can
starve low-priority work if high-priority submissions never stop.

`pause()` stops further dequeues, not tasks already taken by workers. `resume()` wakes
workers. Shutdown always unpauses so graceful draining cannot remain stuck on pause.

`wait_idle()` waits for zero queued tasks, running frames, and cancellation cleanup.
`wait_idle_for(std::chrono::milliseconds{50})` returns false on timeout. These are
snapshot barriers: another producer may immediately add work. Close admission first
with `request_shutdown()` when a stable barrier is required. Calling idle waits or
joining shutdown on this pool's workers throws `std::logic_error`.

## Shutdown and cancellation

| Operation | New submissions | Queued work | Running work | Joins workers |
| --- | --- | --- | --- | --- |
| `request_shutdown()` | Rejected | Executed | Continues | No |
| `shutdown()` | Rejected | Executed | Continues | Yes |
| `request_shutdown(ShutdownMode::cancel)` | Rejected | Cancelled | Stop requested | No |
| `shutdown(ShutdownMode::cancel)` | Rejected | Cancelled | Stop requested | Yes |
| Destructor | Rejected | Executed unless already cancelled | Continues | Yes |

Cancel completes discarded futures with `task_cancelled`. A task that has already
been dequeued is running for cancellation purposes, even if its callable has not
entered yet. It will execute. Stop is cooperative and pool-wide:

```cpp
auto f = pool.submit_stoppable([](std::stop_token stop) {
    while (!stop.stop_requested()) {
        // Perform a bounded unit of work, or use a stop-aware blocking primitive.
    }
});
pool.shutdown(threadpool::ShutdownMode::cancel);
f.get();
```

A running task that returns normally after stop produces a normal result; a running
task that throws preserves its own exception. Cancel does not force-kill threads or
interrupt arbitrary blocking I/O. A task that never returns can block destruction
forever. Use deadlines and stop-aware waits in application code.

Repeated shutdown is safe. Concurrent shutdown callers are serialized for joining.
Cancel may escalate an earlier drain; drain cannot revoke stop or restore discarded
tasks. Workers may call `request_shutdown`, but never destroy their own pool.
Destruction on a worker calls `std::terminate` rather than accessing a destroyed pool.
Destruction requires all external member calls to have finished.

`request_shutdown` is nonjoining, not wait-free: cancellation destroys queued captures
and runs stop callbacks synchronously. Keep callbacks short, nonthrowing, and do not
join/wait for work that depends on the calling thread or cancellation operation.
Callbacks execute outside the queue mutex and can read statistics or request shutdown.
Under concurrent cancellation, another caller may still be settling its discarded
futures; callbacks must not assume all cancellations have finished globally.

## Logging and observability

The pool integrates the ManuelLogger header at a pinned commit. Logging
is enabled at `info` by default. Set `Options::log_name`, `log_level`, and `log_file`.
An empty `log_level` disables logging, useful for benchmarks and quiet services.
Because the pool passes an explicit level, `MANUEL_LOG_LEVEL` does not override it;
read that environment variable into your options if desired. `NO_COLOR` is honored
by the upstream logger. Its file sink **truncates** the chosen file at construction;
use distinct filenames for pools that need separate logs.

Lifecycle events are info-level; task failures are error-level. Successful tasks do
not emit per-task logs. Logger construction errors propagate from pool construction.
Later logging failures are swallowed so diagnostics cannot break task delivery.
The upstream logger serializes and flushes each message, so workloads with many
failures may benefit from disabling logging.

`statistics()` takes a consistent snapshot. `completed` includes failed executions;
`failed` is a subset, and `cancelled` counts tasks discarded without execution.
`active` counts executing frames, so helping can make it exceed the worker count.
`cancelling` counts discarded tasks awaiting future/capture cleanup. Counters use
`std::size_t` and may wrap in extremely long-lived services. The invariant is:

```text
accepted = queued + active + completed + cancelled + cancelling
failed <= completed
```
