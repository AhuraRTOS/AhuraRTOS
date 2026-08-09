# Running the self-test suite

[← Documentation index](README.md) · [Installation](installation.md)

The kernel ships a suite that exercises every enabled feature and reports
PASS/FAIL over `printf`, finishing with a cycle-accurate benchmark table. It
needs no application code and no board support beyond a working `printf`, which
makes it the fastest way to prove a new target is correctly integrated **before**
writing anything on top of it.

---

## Contents

[Turning it on](#turning-it-on) ·
[One switch instead of two](#one-switch-instead-of-two) ·
[Reading the output](#reading-the-output) ·
[Nothing on the terminal?](#nothing-on-the-terminal-check-the-libc-before-the-kernel) ·
[Budget the flash for it](#budget-the-flash-for-it)

---

## Turning it on

Three things have to line up:

1. **Turn it on** in `os_config.h`:

   ```c
   #define OS_CONFIG_TEST_ENABLE  1U
   ```

2. **Link the suite.** `os_test()` is declared in `ahura.h` and defined only in
   this library - the kernel ships no stub, not even a weak one, so forgetting
   this fails at link time rather than booting a test build that silently tests
   nothing:

   ```cmake
   add_subdirectory(AhuraRTOS/kernel/test)
   target_link_libraries(my_firmware os_test)
   ```

3. **Let the suite own `os_log_output_cb`** (only matters when
   `OS_CONFIG_LOG_ENABLE` is 1). The suite defines that callback itself, because
   testing the log means inspecting what the kernel actually emitted, so your
   `os_cb.c` copy must step aside. The current `os_cb_template.c` already does
   this for you with:

   ```c
   #if (OS_CONFIG_LOG_ENABLE == 1U) && (OS_CONFIG_TEST_ENABLE == 0U)
   ```

   An `os_cb.c` copied before that guard existed needs the same condition added.

With the switch on, `os_init()` creates the test task **instead of** the default
application task, so the suite runs alone and `os_main()` is never called
(`os_main.c` can stay in the build; it is simply unused).

| Message | Cause |
|---|---|
| `undefined reference to 'os_test'` | Point 2: the test library is not linked |
| `multiple definition of 'os_log_output_cb'` | Point 3: your `os_cb.c` still defines it |

## One switch instead of two

Rather than keep points 1 and 2 in sync by hand, let CMake read the switch out
of the config, and register `os_config.h` as a configure dependency so that
editing it re-runs CMake by itself - a header is not otherwise a configure-time
dependency, and the build would go on linking the previous choice:

```cmake
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OS_CONFIG_DIR}/os_config.h)

file(READ ${OS_CONFIG_DIR}/os_config.h _os_cfg)
if(_os_cfg MATCHES "#define[ \t]+OS_CONFIG_TEST_ENABLE[ \t]+1")
    message(STATUS "Ahura self-test suite: ENABLED (os_main() is not run in this build)")
    add_subdirectory(AhuraRTOS/kernel/test)
    set(AHURA_TEST_LIB os_test)
else()
    set(AHURA_TEST_LIB "")
endif()

target_link_libraries(my_firmware ahura_kernel ${AHURA_TEST_LIB})
```

Flipping the define is then the whole procedure:

```text
$ cmake --build build/Debug
[0/1] Re-running CMake...
-- Ahura self-test suite: ENABLED (os_main() is not run in this build)
```

## Reading the output

The suite prints to `printf`, which on a Nucleo means COM1 at 115200 8N1 over
the ST-LINK virtual COM port - open it before resetting the board, the header
appears within milliseconds of boot:

```text
========================================
 Ahura RTOS self-test suite starting...
========================================

--- Kernel / Tick ---
  [PASS] ...
--- Task Lifecycle ---
  [PASS] os_task_create() rejects priority 0 (idle-reserved)
  [PASS] ...
  (one section per subsystem: mutex, semaphore, queue, events, timers,
   work queue, notifications, atomics, heap, logging, stress, ...)

========================================
 RESULT: <n> passed, 0 failed (of <n> checks)
 ALL RTOS FEATURES VERIFIED OK
========================================

========================================
 BENCHMARKS
========================================
  core      : ARMv8-M mainline (Cortex-M33/M35P), FPU, DSP
  build     : -Os, optimized for size, 32-bit
  clocks    : tick 1000 Hz, CPU 250000000 Hz

  Operation (uncontended fast path)              cycles         ns
  ------------------------------------------------------------
  (one row per hot kernel path)
```

A failing check prints `[FAIL]` with the file and line it came from, and the
`RESULT` line counts it.

`[SKIP]` lines are not failures - they name a feature switched off in
`os_config.h`, or one the port does not implement yet. Only the `RESULT` line
decides.

What the suite covers, including its three tiers of stress tests, is documented
in the
[kernel README → Self-test suite](https://github.com/AhuraRTOS/ahura_kernel/blob/main/README.md#self-test-suite).

## Nothing on the terminal? Check the libc before the kernel

A silent console is almost never the kernel - it is usually C library buffering,
and it is worth ruling out first because the symptom (a board that boots and
says nothing) looks identical to a tick that never arrives.

newlib decides how to buffer `stdout` on the *first* `printf`, from what your
`syscalls.c` reports about file descriptor 1. It wants **both** answers, and
falls back to full buffering if either is missing:

| `_fstat()` sets `st_mode` | `_isatty()` returns | Buffering | What you see |
|---|---|---|---|
| `S_IFCHR` | nonzero | line buffered | each `\n` flushes - output appears line by line |
| `S_IFCHR` | `0` | **fully buffered** | silence |
| anything else, or no `_fstat` | (not consulted) | **fully buffered** | silence |

Fully buffered means the output accumulates in a 1 KB buffer that a kernel which
never exits never flushes.

CubeMX's generated `syscalls.c` sets both (`st_mode = S_IFCHR`, `_isatty`
returning `1`), so STM32 projects usually get line buffering for free.
Hand-written or trimmed-down retarget layers often supply only one, or neither.

Take the decision away from the libc entirely - one line in `main()`, before
`os_init()`:

```c
setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: every byte goes out at once */
```

Three things this buys, beyond making a fully-buffered target work at all:

- **No `malloc`.** Both buffered modes allocate their buffer on the first
  `printf`; `_IONBF` with a `NULL` pointer allocates nothing. On a target whose
  heap is a few KB, that matters, and it removes a `malloc` call from whichever
  task happens to print first.
- **Output survives a park.** `os_assert_failed_cb()` and
  `os_stack_overflow_cb()` write directly to the transport and then the core
  stops. A partially filled buffer is lost at exactly the moment you needed to
  read it.
- **Nothing is reordered.** Buffered `printf` from a task and a direct UART
  write from a callback interleave unpredictably; unbuffered does not.

The cost is throughput - one character per `_write()` call - which is
irrelevant for a bring-up console and is why this is a debug-build habit rather
than a rule.

## Budget the flash for it

The suite is deliberately exhaustive, and most of its footprint is the
`.rodata` behind those PASS/FAIL strings: **roughly 100 KB of flash at `-Os`**,
against about 20 KB for a small application. Measured on the NUCLEO-H503RB
(128 KB flash, 32 KB RAM) with everything in `os_config.h` at its default:

| Build | Flash | RAM |
|---|---|---|
| Application, `Release` (`-Os`) | 22 KB (17%) | 7.8 KB (24%) |
| Application, `Debug` (`-O0 -g3`) | 35 KB (27%) | 7.8 KB (24%) |
| **Self-test, `Release` (`-Os`)** | **130 KB (99%)** | 17.7 KB (54%) |
| Self-test, `Debug` (`-O0 -g3`) | overflows by ~14 KB | - |

So on a 128 KB part **run the self-test from the `Release` preset**:

```bash
cmake --preset Release
cmake --build build/Release
```

`Debug` does not fit, and the linker says so plainly - `region 'FLASH'
overflowed by 14172 bytes` - rather than producing a broken image. The suite
already drops its extended stress tests (~15 KB) in unoptimized builds for the
same reason, printing a `[SKIP]` that names the cause; `-Os` runs the full set.
Parts with 256 KB or more take either preset comfortably. Once the port is
verified, set `OS_CONFIG_TEST_ENABLE` back to `0` and the suite leaves the image
entirely.
