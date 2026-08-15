# AhuraRTOS Examples

One small, runnable program per kernel feature. Each file **is** a complete
`os_main.c`: copy it over the one in your project, build, and read the console.
No board support beyond a working `printf`, no HAL headers, no per-example
build changes.

- **Kernel** - [`kernel/`](kernel/), one `os_main_<feature>.c` per feature.
  Catalogue [below](#the-examples).

New to AhuraRTOS? Install it first -
[AhuraRTOS → Installation](../doc/installation.md).
What each API actually guarantees is documented in the
[kernel reference](../doc/kernel.md).

---

## Contents

[How to run one](#how-to-run-one) ·
[The examples](#the-examples) ·
[What they all have in common](#what-they-all-have-in-common) ·
[Configuration they assume](#configuration-they-assume) ·
[If nothing happens](#if-nothing-happens) ·
[Writing your own](#writing-your-own) ·
[License](#license)

---

## How to run one

The kernel calls `os_main()` from the default application task that `os_init()`
creates for you, and the application supplies that function in its own
`os_main.c` (copied from `template/os_main.c` during installation). Every file
here defines exactly that function - so running an example is replacing one file
and rebuilding. Nothing is added to the build, and nothing is removed from it.

### 1. Put the example where your `os_main.c` lives

```bash
cp AhuraRTOS/examples/kernel/os_main_mutex.c  Core/Src/os_main.c
```

`Core/Src/` is the STM32CubeMX convention; use wherever your own copy sits. The
file name on the right has to stay whatever your build already compiles - the
name on the left is only the example's identity in this repo.

Prefer to keep your own application around? Copy the example in beside it and
swap which one the build compiles instead:

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/os_cb.c
  # Core/Src/os_main.c              # your application
    Core/Src/os_main_mutex.c        # the example, while you are reading it
)
```

Exactly **one** of them may be in the build at a time. Two files defining
`os_main()` is a duplicate-symbol link error - which is the failure you want,
rather than a coin flip over whose code runs.

### 2. Build and flash as usual

```bash
cmake --build build/Debug
```

Nothing else changes: same `os_config.h`, same `os_cb.c`, same
`CMakeLists.txt`, same `main()` calling `os_init()` and `os_start()`.

### 3. Open the console

115200 8N1 on whatever `printf` is retargeted to - the ST-LINK virtual COM port
on a Nucleo. Open it **before** resetting the board; the first lines appear
within milliseconds of boot.

Every example prints a `[name]` prefix on each line, so output is easy to tell
apart when you are switching between them:

```text
[mutex] os_main read=0
[mutex] worker  read=1
[mutex] os_main read=2
...
[mutex] final value=10 (expect 10 - every read-modify-write stayed atomic)
```

### 4. Swap the next one in

Repeat step 1 with a different file. The examples are independent - there is no
order to follow and no state carried between them.

## The examples

All of them live in [`kernel/`](kernel/). "Needs" is the `os_config.h` switch
the file requires; every one of those is already `1` in
`template/os_config.h`, so the default configuration runs every example except
where noted. "Tasks" is how many tasks the example creates on top of the
default application task.

| Example | What it shows | Needs | Tasks |
|---|---|---|---|
| [`os_main_hello.c`](kernel/os_main_hello.c) | The smallest possible application: `os_main()`, `os_delay_ms`, `os_tick_get` | - | 0 |
| [`os_main_delay.c`](kernel/os_main_delay.c) | Both delay flavors - `os_delay_ms` and `os_delay_us` - against the tick counter | - | 0 |
| [`os_main_task.c`](kernel/os_main_task.c) | A task handle walked through every state `os_task_state_get()` can report: SUSPENDED → READY/RUNNING → SUSPENDED → INACTIVE, plus what `os_task_priority_set` refuses | - | 2 |
| [`os_main_critical.c`](kernel/os_main_critical.c) | Two tasks hammering one non-atomic counter from inside a critical section; the total is exact because no update can tear | - | 1 |
| [`os_main_kernel_lock.c`](kernel/os_main_kernel_lock.c) | A higher-priority task started under `os_kernel_lock()` that does not run until the unlock - while the rising tick count proves interrupts never stopped | - | 1 |
| [`os_main_atomic.c`](kernel/os_main_atomic.c) | The lost-update race run deliberately: two equal-priority writers increment one counter atomically and one plainly. The atomic total is always exact; the plain one usually is not | `ATOMIC` | 2 |
| [`os_main_mutex.c`](kernel/os_main_mutex.c) | Two tasks running a read-delay-write sequence under the same mutex, so the halves of an update can never interleave; also `os_mutex_try_lock` | `MUTEX` | 1 |
| [`os_main_semaphore.c`](kernel/os_main_semaphore.c) | Producer/consumer over a counting semaphore: the consumer blocks in `os_semaphore_take` and wakes the moment a token appears | `SEMAPHORE` | 1 |
| [`os_main_queue.c`](kernel/os_main_queue.c) | The same items through two queues - one `OS_QUEUE_DEFINE_STATIC`, one heap-backed via `os_queue_init_dynamic` - to show that only the storage differs, never the calls | `QUEUE` (+ `ALLOC` for the dynamic half) | 1 |
| [`os_main_event.c`](kernel/os_main_event.c) | Two tasks setting their own bit while `os_main` waits for both at once, consuming them atomically on match | `EVENT` | 2 |
| [`os_main_notify.c`](kernel/os_main_notify.c) | Signalling one specific task with no separate object: values delivered straight into the receiver's own mailbox | `NOTIFY` | 1 |
| [`os_main_timer.c`](kernel/os_main_timer.c) | Periodic, one-shot and deferred calls: the three DEFINE macros, and why `os_timer_submit` runs every event where `os_timer_start` keeps only the last | `TIMER` | 0 |
| [`os_main_mem.c`](kernel/os_main_mem.c) | Heap allocate/free with the free count and watermark printed around each step; freeing both blocks restores the exact starting count, because adjacent blocks coalesce | `ALLOC` | 0 |
| [`os_main_stack_watermark.c`](kernel/os_main_stack_watermark.c) | Worst-case stack headroom for a worker and for `os_main`, polled from a slow monitoring loop | `STACK_WATERMARK` | 1 |
| [`os_main_cpu_usage.c`](kernel/os_main_cpu_usage.c) | Load sampling across a worker that alternates between blocking and busy-spinning - the two numbers should contrast sharply | `CPU_USAGE` | 1 |
| [`os_main_log.c`](kernel/os_main_log.c) | Why `OS_LOG_*` costs the caller almost nothing next to `printf`, measured - then what happens when logging outruns the transport | `LOG` (+ `os_log_output_cb`) | 0 |
| [`os_main_list.c`](kernel/os_main_list.c) | The intrusive list the scheduler itself runs on: build, remove, re-insert, drain | - (always compiled) | 0 |

Switch names are short here: `MUTEX` means `OS_CONFIG_MUTEX_ENABLE`, and so on.

## What they all have in common

- **They include `ahura.h` and `<stdio.h>`, and nothing else.** No `main.h`, no
  HAL, no BSP. That is what lets the same file run on any board and any vendor
  the kernel supports.
- **They check their own configuration.** An example whose feature is switched
  off in `os_config.h` fails at compile time with a message naming both the file
  and the switch, rather than failing to link or printing nothing:

  ```text
  error: "os_main_mutex.c needs OS_CONFIG_MUTEX_ENABLE=1 in os_config.h"
  ```

- **`os_main()` never returns.** Each one ends in a slow idle loop, so the board
  keeps running and the console stays readable after the interesting part is
  over. Returning would simply delete the default task, which is legal but
  leaves you staring at a silent terminal.
- **Worker tasks use 512-byte stacks** through `OS_TASK_DEFINE`, which is
  comfortable for a `printf` and little else. Your own tasks should be sized
  from a measurement - see `os_main_stack_watermark.c`.
- **Nothing is board-specific**, so there is no LED to wire up and no pin to
  configure. Output is the console only.

## Configuration they assume

The stock `template/os_config.h` runs every example as-is. If you have edited
yours, three values matter:

| Option | Needed | Why |
|---|---|---|
| `OS_CONFIG_MAX_USER_TASKS` | ≥ 2 (default 6) | The busiest examples create two tasks besides the default one |
| `OS_CONFIG_MAIN_TASK_PRIORITY` | `OS_TASK_PRIO_1` (the default) | Examples create their workers at `OS_TASK_PRIO_1` to be *peers* of `os_main`, or at `PRIO_2`/`PRIO_3` to *outrank* it. Raising `os_main` past those inverts the relationship the example is demonstrating |

Two examples need something beyond a switch:

- **`os_main_log.c`** needs `os_log_output_cb()` defined in your `os_cb.c`, or
  the log goes nowhere and the example looks broken while working perfectly.
  `template/os_cb.c` has the definition to copy.
- **`os_main_queue.c`** runs its static half with `OS_CONFIG_ALLOC_ENABLE` at
  `0`, but the dynamic half needs the kernel heap.

Also note that with `OS_CONFIG_TEST_ENABLE` set to `1`, `os_init()` runs the
self-test suite **instead of** the default application task - so no example runs
at all, whichever file is in the build. Set it back to `0`.

## If nothing happens

| Symptom | Cause |
|---|---|
| `undefined reference to 'os_main'` | No example (and no `os_main.c`) is in the application build |
| `multiple definition of 'os_main'` | Two of them are. Exactly one at a time |
| `#error "os_main_X.c needs OS_CONFIG_..."` | That feature is switched off in your `os_config.h` |
| Builds and runs, console silent | Almost always `printf` buffering, not the kernel - see [Nothing on the terminal?](../doc/self-test.md#nothing-on-the-terminal-check-the-libc-before-the-kernel). `setvbuf(stdout, NULL, _IONBF, 0)` in `main()` settles it |
| Console silent, and it is the `log` example | `os_log_output_cb()` is missing from `os_cb.c` |
| First lines missing | The console was opened after reset. Open it first, then reset |
| Nothing runs, whichever example is in the build | `OS_CONFIG_TEST_ENABLE` is `1` - the self-test task replaced the application task |

If the board itself is in question rather than the example, the kernel's
[self-test suite](../doc/self-test.md)
validates the whole port with no application code at all. Run that first.

## Writing your own

The conventions above are worth keeping if you add one, whether for a pull
request or for your own tree:

1. **One file, one feature**, named `os_main_<feature>.c`, defining `os_main()`
   and nothing that has to be called from elsewhere.
2. **`ahura.h` and `<stdio.h>` only.** If it needs a board header, it is an
   application, not an example.
3. **Guard the feature switch** with an `#error` naming the file and the option,
   so a wrong configuration is a compile error rather than a mystery.
4. **Print a `[feature]` prefix** on every line, and print what the *expected*
   result is next to the actual one - the way `os_main_mutex.c` prints
   `final value=10 (expect 10 ...)`. An example that only prints numbers cannot
   tell a reader that it worked.
5. **End in a slow idle loop**, so the console stays readable.
6. **Say in the file header what it demonstrates and what it needs**, and add a
   row to the table above.

## License

GNU General Public License v3.0 or later. See [LICENSE](LICENSE).
