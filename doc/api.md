# Using the kernel

[← Back to the documentation index](README.md) · [Kernel reference](kernel.md)

Every API the kernel offers, what each call guarantees, and the rules that apply
across all of them.


### API at a glance

Everything below is declared in the single public header, `ahura.h`. Each group
compiles away entirely when its `OS_CONFIG_<FEATURE>_ENABLE` is 0.

| Group | Functions |
|---|---|
| **Lifecycle** | `os_init` · `os_start` · `os_kernel_is_running` · `os_core_start` |
| **Tasks** | `os_task_create` · `os_task_start` · `os_task_pause` · `os_task_delete` · `os_task_yield` · `os_task_state_get` · `os_task_name_get` · `os_task_priority_get` · `os_task_priority_set` · `os_task_core_affinity_set` |
| **Delays and time** | `os_delay_ms` · `os_delay_us` · `os_tick_get` |
| **Critical sections** | `os_critical_enter` · `os_critical_exit` |
| **Scheduler lock** | `os_kernel_lock` · `os_kernel_unlock` · `os_kernel_is_locked` |
| **Atomics** | `os_atomic_get` · `os_atomic_set` · `os_atomic_add` · `os_atomic_sub` · `os_atomic_inc` · `os_atomic_dec` · `os_atomic_or` · `os_atomic_and` · `os_atomic_xor` · `os_atomic_nand` · `os_atomic_clear` · `os_atomic_cas` · `os_atomic_test_bit` · `os_atomic_set_bit` · `os_atomic_clear_bit` · `os_atomic_test_and_set_bit` · `os_atomic_test_and_clear_bit` · `os_atomic_set_bit_to` |
| **Mutex** | `os_mutex_init` · `os_mutex_lock` · `os_mutex_unlock` |
| **Semaphore** | `os_semaphore_init` · `os_semaphore_give` · `os_semaphore_take` |
| **Queue** | `OS_QUEUE_DEFINE_STATIC` · `OS_QUEUE_DEFINE_BUFFER` · `OS_QUEUE_DEFINE_DYNAMIC` · `os_queue_init_dynamic` · `os_queue_send` · `os_queue_receive` · `os_queue_count_get` · `os_queue_free_get` · `os_queue_cleanup` |
| **Message buffer** | `OS_MSG_DEFINE_STATIC` · `OS_MSG_DEFINE_BUFFER` · `OS_MSG_DEFINE_DYNAMIC` · `OS_MSG_SPACE` · `os_msg_init_dynamic` · `os_msg_send` · `os_msg_receive` · `os_msg_count_get` · `os_msg_free_get` · `os_msg_peek_size` · `os_msg_cleanup` |
| **Event** | `os_event_init` · `os_event_set_bits` · `os_event_clear_bits` · `os_event_wait_bits` |
| **Task notifications** | `os_notify_give` · `os_notify_wait` |
| **Software timers** | `OS_TIMER_DEFINE_PERIODIC` / `OS_TIMER_DEFINE_ONESHOT` · `os_timer_start` · `os_timer_restart` · `os_timer_pause` · `os_timer_stop` · `os_timer_period_set` · `os_timer_callback_set` · `os_timer_value_set` |
| **Deferred calls** | `OS_TIMER_DEFINE_SUBMIT` · `os_timer_submit` |
| **Kernel heap** | `os_mem_alloc` · `os_mem_free` · `os_mem_free_get` · `os_mem_watermark_get` |
| **Diagnostics** | `os_task_stack_watermark_get` · `os_cpu_usage_get` · `os_stack_overflow_cb` |
| **Debugging** | `OS_ASSERT` · `os_assert_failed_cb` · `OS_LOG_ERROR` / `OS_LOG_WARN` / `OS_LOG_INFO` / `OS_LOG_DEBUG` · `os_log_write` · `os_log_dropped_get` · `os_log_output_cb` |
| **Intrusive list** | `os_list_init` · `os_list_is_empty` · `os_list_push_back` · `os_list_pop_front` · `os_list_remove` · `os_list_insert_before` |
| **Tickless idle** | `os_tickless_idle_process` · `os_tickless_expected_idle_ticks_get` · `os_tickless_max_suppressed_ticks_get` |
| **Application-provided** | `os_main` · `os_test` · `os_tickless_pre_sleep_cb` · `os_tickless_post_sleep_cb` · `os_arch_tz_context_save_cb` · `os_arch_tz_context_restore_cb` · `os_arch_core_id_get_cb` · `os_arch_core_ipi_request_cb` · `os_arch_handler_stack_top_cb` · `os_arch_handler_stack_limit_cb` |

Helper macros: `OS_TASK_DEFINE` (name, stack and handle), `OS_TASK_CONFIG` (what
the task does), `OS_TICKS_FROM_MS`, `OS_WAIT_NOTHING`, and `OS_WAIT_FOREVER`.

Every call that can fail returns an `os_err_t`: `OK`, `ERROR`, `INVALID_ARG`,
`EMPTY`, `FULL`, `BUSY`, `TIMEOUT`, `NOT_OWNER`, `NO_MEMORY`, `ISR`.

A task is declared once and created once, and its name appears only in the
declaration:

```c
OS_TASK_DEFINE(worker, 512U);   /* file scope: name, stack, handle */

os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_3));
os_task_start(&worker);
```

`OS_TASK_DEFINE` records what the task is called and where its stack lives, and
binds both to the handle at compile time. `OS_TASK_CONFIG` carries only what the
task *does* - entry, context, priority - so there is no name to repeat and no
stack to match up. Giving one task another task's stack is not something the API
can express.

`OS_TASK_CONFIG` is one macro whose parameter list follows
`OS_CONFIG_CORE_COUNT`. On a single core there is nothing to place, so affinity
does not appear; above one, every task states where it runs:

```c
/* OS_CONFIG_CORE_COUNT == 1 */
os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_3));

/* OS_CONFIG_CORE_COUNT > 1 - pinned to core 0. Cores are numbered from 0, so a
   dual-core part is core 0 and core 1. */
os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_3,
                                       OS_TASK_CORE(0)));

/* OS_CONFIG_CORE_COUNT > 1 - allowed on either core of a dual-core part */
os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_3,
                                       OS_TASK_CORE(0) | OS_TASK_CORE(1)));

/* OS_CONFIG_CORE_COUNT > 1 - free to run on any core, said explicitly */
os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_3,
                                       OS_TASK_CORE_ANY));
```

Raising `OS_CONFIG_CORE_COUNT` above 1 therefore stops every `OS_TASK_CONFIG`
call from compiling until it is given an affinity. That is deliberate: placement
is the design question on SMP, and a compile error at each creation site forces
it to be answered once and on purpose, rather than defaulting to "anywhere"
everywhere and turning up later as a performance problem with nothing pointing
at its cause. `os_task_core_affinity_set` can still change placement at runtime.

### Default application task

Most RTOS applications create every task by hand in `main()` before calling
`os_start()`. Ahura instead gives every application one default task for free.
`os_init()` unconditionally creates and starts it (see `os_kernel.c`'s
`os_main_system_init()`), except in self-test builds, so `main()` needs nothing
beyond the usual:

```c
os_init();
os_start();
```

The task's body is `os_main()`, declared in `ahura.h` and defined by the
application. The kernel ships no stub for it, so forgetting the file is a link
error rather than a task that silently idles. It is deliberately **not** a `_cb`
function: this is where the application's own code runs, not a kernel query for
platform behavior.

Override it with its own template, separate from `template/os_cb.c`. Copy
`AhuraRTOS/template/os_main.c` into the project as `os_main.c`, add it to the
**application** build (never to the kernel, where it is deliberately absent from
the CMakeLists, just like `template/os_cb.c`), and replace `os_main()`'s body
with the application's own code. That can be a plain `while (1)` loop, or it can
spawn further tasks. Two config options size the task:

```c
#define OS_CONFIG_MAIN_TASK_STACK_SIZE  1024U           /* bytes                          */
#define OS_CONFIG_MAIN_TASK_PRIORITY    OS_TASK_PRIO_1  /* PRIO_1_LOWEST..PRIO_30_HIGHEST */
```

There is no switch to compile the default task out. It always exists unless the
build is a self-test build. Tasks that must exist before the scheduler starts,
which is rare, still belong in `main()`, created the usual way.

Note that `os_init()` discards this task's creation status, matching the
work/timer service-init calls. An out-of-range priority or a stack below
`OS_CONFIG_MIN_STACK_SIZE` therefore fails **silently**: the firmware builds,
boots and schedules, but `os_main()` never runs.

When `OS_CONFIG_TEST_ENABLE` is `1`, `os_init()` does not create `tsk_main` at
all, so the self-test suite runs alone instead of racing the application's own
task. See [Self-test suite](testing.md#self-test-suite). `os_main()` itself still compiles,
so an application's `os_main.c` links unchanged either way. It is simply never
called in that build.

### Task priorities

There are 32 levels, and `os_task_priority_t` in `ahura.h` names every one of
them - including the two the application may not use, so the enum describes the
whole scheduler rather than only the part applications touch.

| Level | Name | Owner |
|---|---|---|
| `0` | `OS_TASK_PRIO_IDLE` | Kernel: the idle task, one per scheduling core |
| `1` .. `30` | `OS_TASK_PRIO_1_LOWEST` .. `OS_TASK_PRIO_30_HIGHEST` (and `OS_TASK_PRIO_1` .. `OS_TASK_PRIO_30`) | The application |
| `31` | `OS_TASK_PRIO_MAX` | Kernel: `tsk_timer` by default |

- **`OS_TASK_PRIO_1_LOWEST` through `OS_TASK_PRIO_30_HIGHEST` is the user
  range**, and those two names *are* the limits - there is no separate pair of
  range constants to keep in sync with them. `os_task_create` and
  `os_task_priority_set` reject anything outside it with
  `OS_ERR_INVALID_ARG`.
- **`OS_TASK_PRIO_IDLE` is the empty-ready-bitmap fallback.** The idle task must
  be the only thing at that level, or the scheduler could pick a real task when
  it means to idle - so it is out of reach of `os_task_create`.
- **`OS_TASK_PRIO_MAX` is kept out of reach too**, so the kernel's service tasks
  `tsk_timer` - which `os_init()` creates automatically - has a
  level nothing else can claim. That is where `OS_CONFIG_TIMER_PRIORITY` puts it
  by default; it may be lowered into the user range when a user task should
  outrank timer callbacks and deferred calls.
  They stay system tasks at any priority, so `os_task_pause` and
  `os_task_delete` keep refusing them. They cost no `OS_CONFIG_MAX_USER_TASKS`
  slots: the kernel reserves its service tasks' slots on top of that number.

A static assertion in `ahura.h` keeps the user range contiguous with both
kernel-owned levels, so renumbering one end without the other fails to build
rather than leaving a level no task can ever occupy.

Using a name is a style choice - a plain number works identically, since
`os_task_config_t.priority` is a `uint32_t`. What a name cannot do is survive
the preprocessor: an enum constant is not a macro, so `#if` reads it as `0`.
That is why a configured priority written as a name is checked with
`_Static_assert` rather than `#if` (see `os_timer.c`), and why application code
should not test one in `#if` either.

The default application task (`tsk_main`) and the self-test task (`tsk_test`)
live in the user range too, at `OS_CONFIG_MAIN_TASK_PRIORITY` and
`OS_CONFIG_TEST_PRIORITY`. Unlike `tsk_timer` they are ordinary
application tasks, so pick values that fit alongside your own.

The kernel's own service tasks - `tsk_timer` and `tsk_log` - are
also protected: `os_task_pause` and `os_task_delete` refuse them with
`OS_ERR_BUSY`, because the timer, work and log APIs are all built on one
running and suspending it would turn every later call into a silent no-op that
still reports success. `tsk_main` and `tsk_test` are ordinary application tasks
and stay fully under the application's control. Note that the log task is *not*
identifiable by priority - it runs at `OS_CONFIG_LOG_TASK_PRIORITY`, deliberately
low - so the protection is a property of how the task was created, not of where
it sits in the priority range.

Because mutexes always do priority inheritance, a task's effective priority can
be temporarily boosted above its configured value while it holds a contended
mutex. See [Mutexes and priority
inheritance](#mutexes-and-priority-inheritance).

Tasks at the **same** priority round-robin. `OS_CONFIG_TIME_SLICE_TICKS` sets
how long one holds the CPU before its peers get a turn:

| Value | Behavior |
|---|---|
| `1` (default) | Rotate on every tick. |
| `N` | Rotate every N ticks. Fewer context switches, longer uninterrupted runs. |
| `0` | No rotation: a task runs until it blocks, yields, or is preempted. |

Only equal-priority tasks are affected - a higher-priority task always preempts
immediately, whatever the quantum. A task that blocks or yields gives up the
rest of its slice, and a freshly dispatched task always starts a whole one.
Raising the quantum also makes ticks cheaper: a tick that would only have
rotated now costs a bitmap check instead of a full `PendSV` round trip.

### Scheduler lock

`os_kernel_lock()` / `os_kernel_unlock()` defer context switches on the
calling core **without masking interrupts**. Interrupts keep running and keep
waking tasks; those tasks simply do not get the CPU until the outermost unlock,
which then takes the switch it deferred straight away. Nesting is counted, and
`os_kernel_is_locked()` reports the current state.

This is the right tool when what you are guarding against is another *task*:

| Data shared between | Use | Cost |
|---|---|---|
| task ↔ task | `os_kernel_lock` | No interrupt latency at all. |
| task ↔ ISR | `os_critical_enter` (or an atomic) | Interrupts masked for the region. |
| core ↔ core | `os_critical_enter` | Masks locally, spins the other cores. |

A scheduler lock excludes **no interrupt** and **no other core** - it is per
core, and another core keeps scheduling its own tasks normally. Anything an ISR
also touches still needs a critical section. See [The three
barriers](design.md#the-three-barriers) for how each one is implemented.

While the lock is held the calling task cannot block, because blocking means
switching away. Every blocking primitive behaves as if it had been called with
`OS_WAIT_NOTHING`, `os_delay_ms` busy-waits instead of sleeping, and
`os_task_pause`/`os_task_delete` aimed at the *calling* task return
`OS_ERR_BUSY`. Keep locked regions short and free of blocking calls, exactly
as with a critical section.

### Timeout semantics

Blocking APIs (`os_mutex_lock`, `os_semaphore_take`, `os_queue_send`,
`os_queue_receive`, `os_event_wait_bits`, `os_notify_wait`) take a
`timeout_ms` argument:

| Value | Behavior |
|---|---|
| `OS_WAIT_NOTHING` | Try once, return `BUSY`, `EMPTY`, or `FULL` immediately. |
| `1..N` ms | Wait up to that long, then return `OS_ERR_TIMEOUT`. |
| `OS_WAIT_FOREVER` | Wait until available. |

Nonzero timeouts are honored only from task context after `os_start`. From
interrupt context, before the scheduler starts, or while the calling core holds
a [scheduler lock](#scheduler-lock), the call degrades to a non-blocking
attempt.

Waits are exact. Every object carries its own waiter list, and queues carry two,
one for senders and one for receivers. A blocked task consumes zero CPU until
the object signals it or its timeout expires. Unlock, give, send, receive, and
set_bits all wake the **highest-priority** waiter, FIFO among equals, while
events wake all waiters so each one re-evaluates its bit condition. On a
timeout the tick removes the task from both the delay list and the waiter list.
Wakeups re-check the condition, so a faster third task taking the object in
between is handled by re-waiting with the remaining timeout - measured against
the wall clock, so a busy system cannot stretch it. See [Blocking and
waking](design.md#blocking-and-waking).

### Mutexes and priority inheritance

Mutexes always do priority inheritance, the way FreeRTOS and Zephyr do it, with
no switch to opt out while still calling it a mutex. `os_mutex_lock` boosts a
lower-priority owner to the blocking waiter's effective priority for as long as
it holds the mutex, and `os_mutex_unlock` restores it. This stays correct even
when the task holds several mutexes at once, because the restore recomputes
against every mutex it still holds rather than only the one just released.

Two limitations are accepted rather than implemented:

- **Single-level only.** An owner that is itself blocked on a second mutex held
  by a third, lower-priority task does not propagate the boost through that
  chain.
- **Lazy recompute.** A boost already in effect is not eagerly repositioned
  within some other object's wait queue, nor eagerly lowered when a waiter times
  out early. Both recompute at the owner's next `os_mutex_lock` or
  `os_mutex_unlock`.

A mutex is also an ownership object, which makes it task-only: calls from an ISR
are rejected, because an ISR has no identity of its own. It is not recursive
either, so locking a mutex the caller already holds fails with `OS_ERR_BUSY`
rather than deadlocking.

#### Deadlock detection

**Priority inheritance does not prevent deadlock, and no version of it can.**
The two problems are unrelated: inheritance fixes *when* a waiting task gets to
run, while a deadlock is an *ordering* fault — task 1 takes A then B, task 2
takes B then A, and both wait forever. No scheduler decision breaks a cycle.

So the kernel detects it instead. When a task is about to block on a locked mutex
**forever**, it walks the wait chain — who owns this mutex, and what is *that*
owner blocked on — and if the chain arrives back at the caller, that is a cycle,
and `OS_ASSERT` fires **at the moment the deadlock would form**, while the guilty
task is still running and its call stack still shows which code took the locks in
which order. Without it the symptom is a board that quietly stops, with several
tasks blocked and nothing recording how they got there.

**Waits with a timeout are never reported, and never appear inside a reported
cycle.** A task that will give up after `timeout_ms` is not deadlocked — it is
about to get `OS_ERR_TIMEOUT` and carry on — and it breaks any cycle it is
part of by doing so. So a timed `os_mutex_lock` skips the walk entirely, and
publishes no edge for anyone else's walk to follow. What is left only fires when
**every** task in the cycle is waiting forever, which is a deadlock that can
never resolve itself. False alarms would be worse than useless: a detector that
cries wolf gets switched off.

Because an assertion can only carry a file and a line — and they are always the
same ones inside `os_mutex.c` — the details go into a plain RAM record instead:

```c
/* after the board halts, break in and read: */
os_task_deadlock_report.requested      /* the mutex that was asked for   */
os_task_deadlock_report.waiter_name    /* the task that was about to block */
os_task_deadlock_report.owner_name     /* the task holding it            */
os_task_deadlock_report.cycle[]        /* every mutex in the cycle       */
os_task_deadlock_report.cycle_length
```

It is a non-static symbol, so it can be located by name in the map file even in a
build with no debug info. Printing on the way down is not an option: the kernel
log is a ring drained by a task that will never run again once the core parks,
and calling the output transport directly would hang outright if
`os_log_output_cb` uses DMA, since the completion interrupt cannot arrive with
interrupts masked.

The whole thing rides on `OS_CONFIG_ASSERT_ENABLE` rather than having a switch of
its own, because an assertion is the only way it can report anything. With
assertions off, neither the walk, nor the record, nor the pointer it needs in
each TCB exists. The walk is capped at 8 links and reports *nothing* on reaching
the cap.

The best protection is still not to need it. If a task never holds one mutex
while taking another, a deadlock through mutexes is impossible; where nesting is
unavoidable, take the locks in one fixed global order everywhere.

### Task notifications

A lightweight, single-value mailbox built directly into every task's own control
block, enabled by `OS_CONFIG_NOTIFY_ENABLE`. It lets you signal one
specific task without allocating a separate semaphore or queue object:

```c
os_notify_give(&some_task, 42U);         /* ISR-safe; overwrite: last write wins */
os_notify_give(NULL, 42U);               /* NULL = this task (not from an ISR)   */
os_notify_wait(OS_WAIT_FOREVER, &value); /* called by that task about itself     */
```

`os_notify_give` latches the value and, if the target is currently blocked
in `os_notify_wait`, wakes it immediately. A task blocked for any other
reason, such as a delay or a mutex, queue, semaphore, or event wait, is left
running as normal. The value simply waits to be picked up on its next
`os_notify_wait` call, so nothing is lost. `os_notify_wait` is
task-only, like `os_mutex_lock`, because an ISR has no task identity to wait as,
and it follows the `timeout_ms` convention above.

### Queues

A queue copies fixed-size items between tasks, or from an ISR to a task. The
macro that declares it decides where its item buffer comes from, and that is the
only difference between the two kinds. Everything else, including every send and
receive call, is the same.

| | static | your own buffer | dynamic |
|---|---|---|---|
| declare | `OS_QUEUE_DEFINE_STATIC(name, type, item_count)` | `OS_QUEUE_DEFINE_BUFFER(name, array)` | `OS_QUEUE_DEFINE_DYNAMIC(name)` |
| set up | nothing to call | nothing to call | `os_queue_init_dynamic(&name, item_size, cap)` |
| tear down | `os_queue_cleanup(&name)` | `os_queue_cleanup(&name)` | `os_queue_cleanup(&name)` |
| needs | nothing | nothing | `OS_CONFIG_ALLOC_ENABLE` |

Only the dynamic kind has an init call, because only it has work that cannot
happen until run time. Neither of the other two takes an item size or a
capacity: both are read off the array, so they cannot disagree with the storage
that actually exists.

**Static, when the size is known at compile time.** `OS_QUEUE_DEFINE_STATIC`
declares the queue and its buffer together *and* initializes them, so the queue
is usable where it stands:

```c
typedef struct { uint32_t id; uint8_t payload[6]; } sample_t;

OS_QUEUE_DEFINE_STATIC(sensor_q, sample_t, 8);   /* file scope: both objects are static */

os_queue_send(&sensor_q, &sample, 10U);          /* no init call, nothing to check */
```

There is deliberately no init call to pair it with. The item size and capacity
come from the declaration itself, so they cannot disagree with the storage that
actually exists - handing a queue a capacity larger than its buffer is otherwise
easy to do and silently reads or writes past the end of it. The buffer is
declared as `sensor_q_BUFFER` and should never be named by hand.

Everything the macro leaves out of the initializer - head, tail, count, the
waiter lists - is zero-initialized under the C rules for static storage, which
is byte-for-byte the state an init call would have written. The cost is that the
queue object lands in `.data` rather than `.bss`, so its initializer image
occupies flash.

**Dynamic, when the size is only known at run time.** `OS_QUEUE_DEFINE_DYNAMIC`
declares just the object; `os_queue_init_dynamic` allocates the item buffer from
the kernel heap and initializes the queue over it:

```c
OS_QUEUE_DEFINE_DYNAMIC(rx_q);   /* the object is still yours; only the buffer is allocated */

os_err_t status = os_queue_init_dynamic(&rx_q, item_size, capacity);
...
os_queue_cleanup(&rx_q);         /* returns the buffer to the heap */
```

Keeping the queue object out of the allocation means its lifetime stays obvious
and a failed call leaves nothing to clean up. `os_queue_init_dynamic` returns
`OS_ERR_NO_MEMORY` when the heap cannot satisfy the request, and
`OS_ERR_INVALID_ARG` for a zero or overflowing geometry rather than wrapping
it into a small allocation that later sends would index past.

**Storage you lay out yourself.** For a buffer `OS_QUEUE_DEFINE_STATIC` cannot
express - a named linker section, DMA-capable RAM, a particular alignment -
declare the array yourself and bind a queue to it. It is initialized at compile
time exactly like the static kind, so there is still nothing to call:

```c
static sample_t dma_area[8] __attribute__((section(".dma_buffers")));

OS_QUEUE_DEFINE_BUFFER(rx_q, dma_area);   /* file scope, ready to use */

os_queue_send(&rx_q, &sample, 10U);
```

Item size and capacity come from the array, so there is nothing to keep in step
by hand. Passing a *pointer* to the array instead of the array itself is a
compile error rather than a silently wrong capacity - `sizeof` on a pointer
would derive nonsense and every send past the first would run off the end of the
storage.

**Teardown is the same call for every kind.** `os_queue_cleanup` empties the
queue, and what happens to the storage depends on who owns it, so code tearing
down a mixed set of queues does not need to track which kind each one is:

- A buffer from `os_queue_init_dynamic` goes back to the heap, and the geometry
  goes with it. Re-use means another `os_queue_init_dynamic`.
- A buffer from `OS_QUEUE_DEFINE_STATIC` or `OS_QUEUE_DEFINE_BUFFER` is not the
  kernel's to release, so the queue keeps its storage and is left empty and
  immediately usable - a statically defined queue needs no init call after
  cleanup either, exactly as it needed none before.

It is not compiled out with the heap, and returns `OS_ERR_BUSY` while any
task is still blocked on the queue, because freeing underneath waiters would
leave them parked on list nodes inside memory the heap can hand out again. Drain
the queue and let the waiters time out first.

### Message buffers

A queue carries N items of one fixed size. A message buffer carries **whole
messages whose length varies**, out of one byte budget you choose. Enabled with
`OS_CONFIG_MSG_ENABLE`, independent of `OS_CONFIG_QUEUE_ENABLE` - neither is
built on the other.

```c
OS_MSG_DEFINE_STATIC(cmd_buf, 4U * OS_MSG_SPACE(32U));   /* 136 bytes of storage */

os_msg_send(&cmd_buf, frame, frame_len, 10U);

uint8_t rx[64];
size_t  rx_len;
os_msg_receive(&cmd_buf, rx, sizeof(rx), &rx_len, OS_WAIT_FOREVER);
```

**Which one to reach for.** If every item is the same `struct`, use a queue: the
slot arithmetic is what makes it cheap. If the length is *data* - a protocol
frame, a console line, a sensor burst - use this. Sizing a queue for the longest
message a link can carry and then sending mostly short ones spends the
difference on every slot; padding short messages up to that size throws away the
one thing the receiver needed, which is how many of those bytes are real.

| | queue | message buffer |
|---|---|---|
| capacity is | a slot count × one item size | a byte budget, shared |
| an item costs | one slot, always | its own length + `OS_MSG_HEADER_BYTES` |
| a short item | costs a whole slot | costs what it is |
| back-pressure reads as | free **slots** | free **bytes** |
| storage kinds | static, own buffer, heap | static, own buffer |

**Sizing it.** Capacity is in bytes, and each message carries a small length
header, so write the budget in the terms the application thinks in and let
`OS_MSG_SPACE` add the overhead:

```c
OS_MSG_DEFINE_STATIC(cmd_buf, 4U * OS_MSG_SPACE(32U));    /* "four commands of up to 32" */

static uint8_t rx_area[512] __attribute__((section(".dma_buffers")));
OS_MSG_DEFINE_BUFFER(rx_buf, rx_area);                    /* storage you placed yourself */
```

Nothing enforces that split at run time - those same 136 bytes will just as
happily hold eight 14-byte messages or one 130-byte one, which is the
flexibility being paid for. There is no dynamic kind: the capacity is one
number, not a geometry, and `OS_MSG_DEFINE_BUFFER` covers storage the static
macro cannot express.

**Sending and receiving.** Messages arrive whole and in order, one per
`os_msg_receive()` - never a fragment, never two joined. Both directions block
with the usual [timeout semantics](#timeout-semantics), so a full buffer applies
back-pressure to senders exactly as a full queue does.

Three refusals are worth knowing, because each one replaces a failure that would
otherwise be silent:

- **A destination too small** is `OS_ERR_INVALID_ARG`, and **the message stays
  where it is** - nothing is truncated, nothing is dropped. `length_out` still
  receives the size that message needs, so one failed call tells the caller
  exactly how big a buffer to come back with. Note the consequence: a receiver
  that keeps offering the same too-small buffer keeps meeting the same message,
  so treat this as a bug to fix rather than a condition to retry around. Use
  `os_msg_peek_size()` to size a destination before committing to one.
- **A message larger than the whole buffer** is refused immediately, even with
  `OS_WAIT_FOREVER`. Waiting for room that can never exist is a hang, and a hang
  is the one outcome a timeout cannot rescue the caller from.
- **A zero-length message** is refused. Use a semaphore or a notification to
  signal without carrying bytes.

**Reading the free count.** `os_msg_free_get()` reports free **bytes**, and has
to be read against `OS_MSG_SPACE`, not against a raw length - a buffer with
exactly `length` bytes free still has no room, because the message pays for its
own header too:

```c
if (os_msg_free_get(&cmd_buf) >= OS_MSG_SPACE(len))     /* right */
if (os_msg_free_get(&cmd_buf) >= len)                   /* wrong: forgets the header */
```

**Most code needs none of this.** `os_msg_send()` adds the 2 bytes itself and
returns `OS_ERR_FULL` when the message does not fit, so sending and reading the
status answers the question without any arithmetic. `os_msg_free_get()` is for
reporting and back-pressure heuristics, and like the queue's it is a snapshot:
anything that sends or receives in between changes the answer.

**Storage comes in the same three kinds as a queue's**, and for the same reason:
these are the only two kernel objects that own a buffer.
`OS_MSG_DEFINE_STATIC` sizes it at compile time, `OS_MSG_DEFINE_BUFFER` takes an
array you placed yourself, and `OS_MSG_DEFINE_DYNAMIC` plus `os_msg_init_dynamic()`
takes a byte budget not known until run time:

```c
OS_MSG_DEFINE_DYNAMIC(rx_buf);
...
status = os_msg_init_dynamic(&rx_buf, 4U * OS_MSG_SPACE(mtu_from_config));
```

`byte_size` is a byte budget exactly as in the static macro, so size it with
`OS_MSG_SPACE`. A budget too small to hold even one message - anything under
`OS_MSG_SPACE(1)` - is `OS_ERR_INVALID_ARG` rather than an object that exists and
refuses every send it is ever given. Only the buffer is allocated; the object
itself is yours, which is why a failed init leaves nothing to clean up.

**Teardown** is `os_msg_cleanup()`, and every kind converges on it, so a caller
tearing down a mixed set need not track which is which. A heap buffer goes back
to the heap and re-use means another init call; a compile-time buffer has nothing
to release, so it is left empty and immediately usable. It returns `OS_ERR_BUSY`
while any task is blocked on the buffer, for the same reason `os_queue_cleanup`
does - freeing underneath a waiter would park it on a list node the heap can hand
out again.

Note that it empties the buffer without erasing it: the bytes of consumed
messages stay in RAM until later ones overwrite them. Nothing can read them back
through the API, but a memory dump will still show them.

**Limits.** One message is at most `OS_MSG_LENGTH_MAX` (64 KiB - 1), which is
what the two-byte header can express. The header width is fixed rather than
configurable: it is the difference between a 12-byte message costing 14 bytes
and costing 16, and a knob for that is a decision every project would have to
make and none would benefit from. The copy runs inside a critical section, as
the queue's does, so keep messages modest for the same reason.

### Atomics

An operation no other task, ISR, or core can observe half-finished. Enabled with
`OS_CONFIG_ATOMIC_ENABLE`.

```c
static os_atomic_t counter = OS_ATOMIC_INIT(0);

os_atomic_inc(&counter);              /* returns the value from BEFORE the increment */
os_atomic_add(&counter, 5);
os_atomic_cas(&counter, 10, 20);      /* swap only if it still holds 10 */
os_atomic_set_bit(&flags, 3U);
```

The problem it solves: `count = count + 1` is a load, an add, and a store.
Anything that preempts between the load and the store makes both writers compute
from the same starting value, so one increment silently disappears.

**Every read-modify-write returns the value from before the operation**, not
after it. `os_atomic_inc` returning `4` means the counter now reads `5`.

Two rules worth stating outright:

- **Declare shared words as `os_atomic_t`**, not as a plain or `volatile` int
  that you cast at the call site. An ordinary read or write of the same word is
  not ordered against these calls, which is the usual way a counter that "uses
  atomics" still loses updates.
- **`os_atomic_cas` does not retry.** A `false` return may mean another writer
  won *or* that exclusive access was lost, so it does not by itself prove the
  value changed. Loop if you only care about the final state; re-read the value
  if you need to know which happened.

**Cost depends on the core**, because the whole operation set is part of the
port rather than something portable code builds out of one primitive. There are
two backends in `common/os_arch_atomic.c`, selected by
`OS_ARCH_ATOMIC_LOCK_FREE` in `os_arch_port_common.h`:

| Backend | Cores | How | Cost |
|---|---|---|---|
| Lock-free | Cortex-M3, M4, M7, M33, M35P, M52, M55, M85 | One `LDREX`/`STREX` retry loop per operation, each a single asm block | Interrupts stay enabled; interference costs a retry, not correctness |
| Critical section | Cortex-M0, M0+, M23 | Each operation inside `os_critical_enter`/`os_critical_exit` | Adds the update's length to interrupt latency, and can wait on unrelated kernel work on multi-core builds |

The second row exists because ARMv6-M has no instruction that can *detect*
interference mid-update, so it has to be prevented instead. Worth knowing before
putting an atomic in an ARMv6-M interrupt-latency budget. All of them are safe
to call from tasks and from ISRs.

`os_arch_atomic_load` is the one operation with no backend split, and the only
one defined in `os_arch_port_common.h` rather than the port `.c`: a single
naturally aligned 32-bit load is already indivisible on every core here, so it
is one `LDR` inlined into the caller. Everything else is a real function in the
port.

Writing each operation out separately, instead of sharing one implementation
behind a selector, is what keeps the emitted code to the five instructions the
sequence actually is, and keeps compiler-generated stack traffic from ever
landing between an `LDREX` and its `STREX`.

Portable code above the port only composes these: incrementing is an add of 1,
clearing a bit is an AND with its complement. So a new port implements nine
operations and gets the full API, with no behaviour able to drift between cores.

> Cortex-M23 is ARMv8-M *baseline*: it has `LDREX`/`STREX`, but runs the Thumb-1
> subset, where the three-operand data-processing forms these loops use have no
> encoding - so it takes the critical-section backend. Correct, just not as fast
> as that core allows. Lifting it means rewriting the loops in the flag-setting
> Thumb-1 forms, which would constrain register allocation on every other core.

### Deferred calls

Run a function later on the kernel timer task, off the hot path. There is no
separate API for it: **a deferred call is a one-shot timer**, and `os_timer_start`
carries the arguments.

```c
static void my_callback(void *context, uint32_t value) { /* runs on tsk_timer */ }

OS_TIMER_DEFINE_ONESHOT(defer_now,  1U, my_callback);
OS_TIMER_DEFINE_ONESHOT(defer_late, 100U, my_callback);

os_timer_start(&defer_now,  &my_device, sample);   /* as soon as possible */
os_timer_start(&defer_late, NULL,       42U);      /* or after a delay    */
```

**This is the work queue, and it is deliberately not a separate module** - nor
even a separate call. It uses the same tick, the same delivery queue and the same
task. What that costs is one shared priority for both; what it saves is an entire
service task, its stack, and five configuration options.

**Use `os_timer_submit` when every event matters:**

```c
OS_TIMER_DEFINE_SUBMIT(uart_defer, 8U, 0U, on_uart_event);
/*                                 |   |
 *                                 |   delay before each call (0 = as soon as possible)
 *                                 how many may be in flight at once                    */

os_timer_submit(&uart_defer, &dev, code1);   /* in the ISR   */
os_timer_submit(&uart_defer, &dev, code2);   /* fires again  */
/* the callback runs TWICE, code1 then code2 */
```

The delay lives in the definition, so the milliseconds are converted to ticks at
compile time and `os_timer_submit` does no arithmetic at all - which matters for
a call an interrupt makes. Work needing a different delay is a different pool.

`os_timer_start` in that position would have run the callback once, with `code2`
only - `code1` silently lost. Reach for `start` when you want the latest event
(a debounce, a watchdog kick, an inactivity timeout) and `submit` when you want
every event.

**Nothing is copied.** `context` is passed through exactly as given, so whatever
it points at must still exist when the callback runs. `value` is there precisely
so the common case needs no lifetime reasoning at all: a number travels by copy,
which covers most of what an ISR wants to hand over.

```c
uint32_t sample = ADC->DR;
os_timer_start(&defer_now, &my_device, sample);   /* device: yours. sample: copied. */
```

**Delivery is FIFO**, shared with every other timer expiry. Callbacks run in the
order they *became ready*, one at a time, never overlapping. A long delay does not
hold the queue: a 1 ms and a 100 ms deferral started together run in that order,
each on its own schedule.

`os_timer_submit` returns `OS_ERR_FULL` when that pool's own entries are all
in flight, so an overrun is reported rather than silently dropped - and it is
always your pool that ran out, never someone else's.

Deferred calls and timer callbacks run in task context, so they may use kernel
APIs, but at the default `OS_CONFIG_TIMER_PRIORITY` they execute above every user
task. Keep them short and do not block in them, or everything else starves - or
lower that priority so they cannot.

### Kernel heap

`OS_CONFIG_ALLOC_ENABLE` (default 1) compiles in a kernel heap of
`OS_CONFIG_HEAP_SIZE` bytes (default 4096). It is a static array, so nothing is
taken from the linker heap:

```c
void  *memory = os_mem_alloc(size);   /* 8-byte aligned, NULL when exhausted   */
os_mem_free(memory);                  /* NULL/foreign/double free are ignored  */
size_t now  = os_mem_free_get();      /* current free bytes                    */
size_t low  = os_mem_watermark_get(); /* worst-case watermark since boot       */
```

The allocator is first-fit with an address-ordered free list and coalescing of
adjacent free blocks, comparable to FreeRTOS `heap_4`, so mixed-size alloc and
free patterns do not fragment permanently. Calls are protected by the kernel
critical section, which makes them usable from tasks and ISRs, although
allocating in an ISR is discouraged because the walk over the free list runs
with interrupts masked. See [The kernel heap
(internals)](design.md#the-kernel-heap-internals) for the block layout.

### Diagnostics

**Task names.** `OS_TASK_DEFINE(worker, 512U)` names the task after its own
handle, and

```c
const char *name = os_task_name_get(task);   /* NULL task = calling task */
```

hands that string back - so an application's own diagnostics (a shell command
listing tasks, a trace line, an error report) can say *which* task without
keeping a second table mapping handles to strings. The pointer is a string
literal, so it stays valid after the task is deleted; what it stops describing
is a *live* task.

`NULL` comes back for a handle no task owns, and for **every** task in a build
with `OS_CONFIG_TASK_NAME_ENABLE` at `0`. That option is a pure size trade: the
kernel never reads a name itself, so turning it off removes one pointer per task
from the TCB and, because nothing references the strings any more, the names
themselves from flash - measured at 64 bytes of RAM and 604 bytes of flash on
the self-test build for RP2350. What it costs is diagnosis:
`os_stack_overflow_cb()` is handed `NULL` and the deadlock report names no task.
Because the unknown-handle answer is already `NULL`, a caller that handles one
case handles both.

**Stack watermark.** With `OS_CONFIG_STACK_WATERMARK_ENABLE` (default 1), task
stacks are pattern-filled at creation and

```c
size_t min_free;
os_task_stack_watermark_get(task, &min_free);   /* NULL task = calling task */
```

reports the worst-case remaining stack in bytes since that task was created.
It is a measurement you poll, not a detector - by the time a task has actually
overrun, the damage is already done. That is what the next option is for.

**Stack overflow detection.** With `OS_CONFIG_STACK_CHECK_ENABLE` (default 1),
every switch away from a task checks two things: that its stack pointer is still
inside its own stack, and that a guard word at the bottom of that stack is
intact. The first catches a task executing outside its stack right now; the
second catches one that went too deep and came back, which nothing else would
notice. On a hit the kernel calls

```c
void os_stack_overflow_cb(const char *task_name);   /* you define it; no kernel default */
```

`task_name` is `NULL` when `OS_CONFIG_TASK_NAME_ENABLE` is `0`. The overflow is
still detected and still parks the core; it just cannot say whose stack it was.

The kernel ships no default for it, so a build with the check enabled and no
callback is a link error rather than an overflow detector reporting to nobody -
same rule as `os_assert_failed_cb`. Copy the definition from `template/os_cb.c`.

and then parks the core, exactly as a failed `OS_ASSERT` does - there is no
attempt to continue, because memory outside the task has already been written
and there is no way to know whose. The callback runs inside PendSV with
interrupts masked, so it must not call kernel APIs; write to a UART directly or
latch the pointer for the debugger. Cost is a compare and a load per context
switch.

Worth knowing which cores need it: **ARMv8-M mainline** (M33, M35P, M52, M55,
M85) already traps this in hardware through a per-task `PSPLIM`, whatever this
option is set to. Every other supported core - M0, M0+, M23, M3, M4, M7 - has no
stack-limit register, and this software check is the only detection available
there.

**CPU usage.** With `OS_CONFIG_CPU_USAGE_ENABLE` (default 1) the tick interrupt
counts how many ticks interrupted the idle task versus anything else, and

```c
uint32_t percent = os_cpu_usage_get();   /* 0..100 since the previous call */
```

returns the load over the window since the previous call, then restarts the
window. Resolution is one tick, so sample at a period well above the tick period,
for example once per second at a 1 kHz tick. Ticks announced after a tickless
sleep count as idle. The cost is two counter updates per tick.

### Debugging

Two independent features: assertions that catch programming errors where they
happen, and logging that does not stall the caller.

#### Assertions

`OS_CONFIG_ASSERT_ENABLE` (default 1) turns on `OS_ASSERT(expr)`. A failing
check calls `os_assert_failed_cb(file, line)` so the application can print or
record the location, then parks the core with interrupts masked so a debugger
stops at the cause. That callback is **required** when assertions are enabled:
the kernel ships no stub, because a silent one would turn every assertion into
an unexplained halt. Leaving it out is a link error.

```c
void os_assert_failed_cb(const char *file, uint32_t line)
{
    /* print it, stash it in a noinit/backup register, or just break */
    __BKPT(0);
}
```

The line the kernel draws is between a **static mistake in the code** and a
**runtime outcome**. It asserts on the former: a NULL object handle, a blocking
call made from an ISR, an `os_critical_exit()` with no matching enter (which
returns `void`, so it has no other way to report at all).

It does **not** assert on anything with a documented status, even when that
status usually means someone made a mistake. `OS_ERR_NOT_OWNER` from
`os_mutex_unlock`, `BUSY`, `FULL`, `EMPTY`, and `TIMEOUT` all depend on runtime
scheduling, and callers are entitled to attempt the operation and handle the
result. Asserting there would halt correct programs.

Assertions only **add** checks. Every API still returns exactly the same status
either way, so a build with `OS_CONFIG_ASSERT_ENABLE=0` behaves identically,
minus the halt. Use them for programming errors, never for conditions that can
legitimately occur at runtime. The expression is not evaluated at all when
assertions are compiled out, so it must be free of side effects.

#### Logging

`OS_CONFIG_LOG_ENABLE` (default 1) provides printf-style logging that returns
immediately instead of waiting on a serial port:

```c
OS_LOG_ERROR("i2c timeout on 0x%02x", addr);
OS_LOG_WARN ("battery low: %lu mV", (unsigned long)mv);
OS_LOG_INFO ("sensor = %d", value);
OS_LOG_DEBUG("state %u -> %u", from, to);
```

Each call formats the line, copies it into a ring buffer, and returns. A
low-priority kernel task (`tsk_log`) drains the buffer in the background and
hands finished bytes to the application:

```c
void os_log_output_cb(const uint8_t *data, size_t length)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)data, length, HAL_MAX_DELAY);
}
```

That callback runs on `tsk_log`, never from an ISR or a critical section, so it
may block or start a DMA transfer.

Calls above `OS_CONFIG_LOG_LEVEL` expand to nothing, arguments included, so a
disabled `OS_LOG_DEBUG` costs neither code nor the evaluation of its arguments.
Logging is safe from tasks and ISRs and never blocks: when the buffer is full
the line is dropped **whole** and counted, never written in part, and the count
is reported into the log once space frees:

```text
[    1234] I sensor = 42
[    1250] W *** 17 log lines dropped ***
[    1251] I sensor = 45
```

Two costs to budget for. `tsk_log` needs its own stack (`OS_CONFIG_LOG_TASK_STACK_SIZE`,
though not an `OS_CONFIG_MAX_USER_TASKS` slot - the kernel reserves service-task slots
separately), and formatting uses libc `vsnprintf`, which pulls newlib's formatter into the link
(roughly 1 to 3 KB) for a project that does not already use `printf`. As usual,
`%f` additionally needs `-u _printf_float`. `OS_CONFIG_LOG_LINE_MAX` is the
scratch buffer `os_log_write` places on the **caller's** stack, so every task
that logs needs that much extra headroom.

---
