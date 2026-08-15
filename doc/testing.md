# Testing and examples

[← Back to the documentation index](README.md) · [Kernel reference](kernel.md)

Proving a port with the self-test suite, and the runnable examples.


### Self-test suite

The suite is not copied into the application. It is a normal buildable module
with its own `CMakeLists.txt` (`AhuraRTOS/kernel/test/CMakeLists.txt`), producing a
static library `os_test` that links against `ahura_kernel` and supplies
`os_test()`. Any project that already builds the kernel can add it:

```cmake
add_subdirectory(AhuraRTOS/kernel)
add_subdirectory(AhuraRTOS/kernel/test)   # builds the os_test library

target_link_libraries(my_app PRIVATE
    ahura_kernel
    os_test
)
```

That is an ordinary static-library link, with no `--whole-archive` needed. The
kernel deliberately ships no stub for `os_test()`, so `os_kernel.c.o` leaves the
symbol undefined and the linker has a reason to extract `os_test.c.o` from the
archive. Forgetting the library is a link error rather than a test suite that
silently vanishes from the build.

Three things have to line up, and the build says clearly if any is missing:

1. `#define OS_CONFIG_TEST_ENABLE 1U` in `os_config.h`.
2. Link the `os_test` library, as above. Miss it and you get `undefined
   reference to 'os_test'`.
3. Let the suite own `os_log_output_cb` when `OS_CONFIG_LOG_ENABLE` is also 1 -
   **the suite defines it itself**, because testing the log means inspecting
   what the kernel actually emitted, so it captures into a buffer it can search.
   A second definition in the application's `os_cb.c` is a duplicate-symbol link
   error, which is why `template/os_cb.c` guards its copy with:

   ```c
   #if (OS_CONFIG_LOG_ENABLE == 1U) && (OS_CONFIG_TEST_ENABLE == 0U)
   ```

   An `os_cb.c` copied before that guard existed needs the same condition added.
   Nothing is lost: the suite's own PASS/FAIL report goes to `printf`, not
   through that callback.

[Running the self-test suite](self-test.md)
has the full run-through, including how to derive point 2 from point 1 in CMake
so there is only one switch to flip, how to read the output, and how much flash
to budget for it.

Once linked, `os_init()` creates and starts the self-test task by itself, gated
by `OS_CONFIG_TEST_ENABLE`, which is off by default in the template so each
project opts in. There is nothing to call:

```c
os_init();   /* creates and starts tsk_test too, since os_test is linked and TEST_ENABLE=1 */
os_start();
```

`tsk_main` is **not** created alongside `tsk_test`, so the suite runs alone
rather than letting the application's own task race it for CPU time and
task-table slots. Set `OS_CONFIG_TEST_ENABLE` back to `0` to get `tsk_main`
running normally again.

The task runs `os_test()` once. It exercises whichever
`OS_CONFIG_<FEATURE>_ENABLE` switches are on, covering tasks, delays, critical
sections, the scheduler lock, mutexes and priority inheritance, semaphores,
queues, events, task notifications, timers, work items, the kernel heap,
stack watermarks, CPU usage, and the intrusive list. It prints a detailed
PASS/FAIL log via `printf` with a pass/fail summary, then finishes with a
**benchmark table**: each hot kernel path timed with the CPU cycle counter,
sampled repeatedly with the minimum kept, and the measurement overhead
subtracted. The header reports the core profile, optimization level, and clocks,
so a result is always interpretable.

The suite depends on nothing but `ahura.h`, with no board or HAL headers, so it
runs on real hardware for any arch or board the kernel supports. Retarget
`printf`'s destination, typically a UART, in the application to see the log.

> Benchmark numbers from a `-O0` debug build run several times slower than a
> release build. The table says which kind of build produced it, so compare like
> with like.

#### Stress tests

Beyond the functional checks, the suite runs three tiers of stress, each aimed at
a different class of bug:

| Tier | What it does |
|---|---|
| **Multi-primitive soak** | `test_stress_soak` - 4 tasks at distinct priorities hit a mutex, an under-provisioned semaphore and queue, an event and the heap *simultaneously* for many iterations, then check hard invariants (exact mutex-protected counter, exact token reconciliation, no leak, no corruption) |
| **Create/destroy churn** | `test_stress_task_churn`, `test_stress_timer_churn` - one create/run/exit or init/start/stop path cycled 500 times, to shake out slot-reuse and list-corruption bugs |
| **Per-subsystem stress** | Nine tests, one subsystem each, at high volume with exact accounting (below) |

The per-subsystem tier:

| Test | Invariant it enforces |
|---|---|
| `test_stress_queue_dynamic_churn` | 200 `os_queue_init_dynamic`/use/`os_queue_cleanup` cycles, geometry varying each time, leak nothing and corrupt no payload |
| `test_stress_queue_dynamic_concurrent` | 3 producers x 32 items through a heap-allocated queue of capacity 2; every `(producer, sequence)` pair arrives exactly once - a lost send-waiter wakeup is a missing bit, a double delivery an already-set one |
| `test_stress_heap_fragmentation` | Freeing a block never disturbs a live neighbour; adjacent holes really coalesce; the heap recovers byte-exactly after being driven to exhaustion |
| `test_stress_semaphore_pingpong` | 1000 round trips (2000 blocking handoffs) through two empty binary semaphores, so every take blocks and every give wakes a waiter - no token is ever already available to mask a lost wakeup |
| `test_stress_notify_storm` | 1000 notifications to a higher-priority waiter that consumes each before the next is written, so exact 1:1 accounting is meaningful for a last-write-wins mailbox |
| `test_stress_event_bit_storm` | 4 tasks x 250 iterations of set/wait/clear-on-exit on their own bit of one group; all bits must end clear |
| `test_stress_timer_flood` | Every timer slot armed periodically at once, each at its own period; one past capacity refused with `FULL`; a stopped timer never fires again |
| `test_stress_mutex_convoy` | 4 tasks x 200 acquisitions on one mutex, yielding *inside* the section - exclusivity checked from within, exact total from without, and no task starved |

Every count is exact rather than approximate, deliberately: a check that only
asserts "roughly the right number of things happened" cannot tell a dropped
wakeup from scheduling jitter, so it has to be written loose enough to pass
through the very bug it exists to catch. The one exception is timer fire counts
over a wall-clock window, where the tolerance is bounded at ±2 rather than left
open.

The per-subsystem tier costs about 15 KB of flash, most of it the `.rodata` for
its PASS/FAIL messages. It is therefore compiled in whenever the build is
optimized at all (`__OPTIMIZE__`, i.e. any `-O` above `-O0`) and skipped
otherwise, with a run-time `[SKIP]` line naming the reason. That key is the
optimization level rather than a hand-set switch because that is what actually
decides whether it fits - an unoptimized build of a 128 KB part may well have no
room - and because stress timings at `-O0` say little about shipped firmware
anyway. Define `OS_TEST_STRESS_EXTENDED` to override in either direction.

### Examples

[`examples/kernel/`](../examples/kernel/)
has one small, focused example per kernel feature, each meant to be copied over
`os_main.c`. It sits alongside the kernel in this repository. Same rule
as the self-test suite: they depend on nothing but `ahura.h`, with no board or
HAL headers.

| Example | Demonstrates |
|---|---|
| `os_main_hello.c` | The minimal application: `os_main()`, `os_delay_ms`, `printf` |
| `os_main_task.c` | Task lifecycle: create, start, pause, resume, delete; `os_task_priority_get/set` |
| `os_main_delay.c` | `os_delay_ms`, `os_delay_us` |
| `os_main_critical.c` | Critical sections protecting a shared counter |
| `os_main_kernel_lock.c` | Deferring preemption with `os_kernel_lock` while interrupts keep running |
| `os_main_mutex.c` | Mutual exclusion with `os_mutex_*` |
| `os_main_semaphore.c` | Counting semaphore, producer and consumer |
| `os_main_queue.c` | Message queue, producer and consumer, both static (`OS_QUEUE_DEFINE_STATIC`) and dynamic (`os_queue_init_dynamic`) storage |
| `os_main_event.c` | Event, waiting on multiple bits |
| `os_main_notify.c` | Task notifications with `os_notify_*` |
| `os_main_timer.c` | Periodic, one-shot and deferred calls, and the difference between `os_timer_start` and `os_timer_submit` |
| `os_main_mem.c` | Kernel heap with `os_mem_alloc` and `os_mem_free` |
| `os_main_stack_watermark.c` | Worst-case stack headroom |
| `os_main_cpu_usage.c` | CPU load sampling |
| `os_main_log.c` | Buffered debug logging (`OS_LOG_*`) |
| `os_main_atomic.c` | Atomic counters and flags with `os_atomic_*` |
| `os_main_list.c` | The intrusive list utility |

Each file needs its matching `OS_CONFIG_<FEATURE>_ENABLE` on, and a compile-time
`#error` says so if it is not. Each one is a complete, standalone `os_main()`,
so copy any of them over the project's `os_main.c` to see it run.

---
