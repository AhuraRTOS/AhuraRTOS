# Ahura Kernel

[← Documentation index](README.md)

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Standard: C11](https://img.shields.io/badge/standard-C11-blue.svg)
![Platform: Cortex-M](https://img.shields.io/badge/platform-Cortex--M-informational.svg)
![Toolchains: GCC | Clang | armclang](https://img.shields.io/badge/toolchains-GCC%20%7C%20Clang%20%7C%20armclang-informational.svg)

A small, preemptive, priority-based RTOS kernel for ARM Cortex-M, covering
everything from the M0 to the M85. It is TrustZone-aware and has no mandatory
HAL or CMSIS dependency.

**It runs on any Cortex-M device, from any vendor.** The kernel takes over
exactly **one** exception - PendSV - and asks the application for exactly one
thing: a periodic call to `os_tick_handler()`. It claims no `SVC_Handler`, no
`SysTick_Handler`, no vendor headers, and no HAL.

> **This page is the kernel reference: what it does, and how it does it.**
> For step-by-step installation - vendor by vendor, IDE by IDE - see the
> [documentation index](README.md).
> The [Getting the kernel into a project](#getting-the-kernel-into-a-project)
> section below is the short version of it.

---

## What the kernel is

- **Preemptive priority scheduler.** 31 priority levels, O(1) list-based ready
  queues, and round-robin among tasks of equal priority.
- **Full sync/IPC set.** Mutexes (always with single-level priority
  inheritance), counting semaphores, queues, events, and lightweight
  per-task notifications, all with millisecond timeouts.
- **Software timers and a deferrable work queue.** One-shot and periodic timers
  plus a work queue in the style of Zephyr, each on its own kernel service task.
- **A scheduler lock that masks no interrupts.** `os_kernel_lock()` defers
  preemption while every ISR keeps running - the barrier a critical section
  cannot be.
- **Optional zero-latency interrupts.** With a nonzero
  `OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY` the kernel masks with `BASEPRI` instead
  of `PRIMASK`, so ISRs above that priority are never delayed by the kernel at
  all.
- **Optional kernel heap.** A first-fit allocator with coalescing, comparable to
  FreeRTOS `heap_4`, compiled out entirely when unused.
- **Built-in diagnostics.** Stack watermarking, stack-overflow detection and CPU
  usage sampling, all opt-in and close to free at runtime.
- **TrustZone-aware.** Secure, non-secure, or disabled, selectable per build on
  ARMv8-M.
- **Every feature is a compile-time switch.** `OS_CONFIG_<FEATURE>_ENABLE`
  removes unused code, RAM, and API surface entirely, not just at runtime.
- **Self-testing.** A built-in suite exercises every enabled feature on real
  hardware with no board or HAL dependencies, and ends with a cycle-accurate
  benchmark table.
- **Broad Cortex-M coverage.** M0/M0+/M23, M3/M4/M7, and M33/M35P/M52/M55/M85
  all share just three portable port implementations.
- **Experimental:** multi-core (SMP) scheduling and tickless idle.

**No dynamic allocation anywhere in the kernel itself.** Task control blocks,
ready lists, timer and work registries, the log ring and the optional heap are
all static arrays sized by `os_config.h`. What a build costs in RAM is decided
at compile time and visible in the map file.

---

## Contents

**[Getting the kernel into a project](#getting-the-kernel-into-a-project)**
[Quick start](#quick-start) ·
[Adding the kernel to a build](#adding-the-kernel-to-a-build) ·
[Configuration](#configuration) ·
[The integration contract](#the-integration-contract)

**[How it works](#how-it-works)**
[Boot sequence](#boot-sequence) ·
[Tasks and the TCB](#tasks-and-the-tcb) ·
[The scheduler](#the-scheduler) ·
[The context switch](#the-context-switch) ·
[The tick](#the-tick) ·
[Blocking and waking](#blocking-and-waking) ·
[Priority inheritance](#priority-inheritance-mechanics) ·
[The three barriers](#the-three-barriers) ·
[Kernel service tasks](#kernel-service-tasks) ·
[The kernel heap](#the-kernel-heap-internals) ·
[Where the RAM goes](#where-the-ram-goes)

**[Using the kernel](#using-the-kernel)**
[API at a glance](#api-at-a-glance) ·
[Default application task](#default-application-task) ·
[Task priorities](#task-priorities) ·
[Scheduler lock](#scheduler-lock) ·
[Timeout semantics](#timeout-semantics) ·
[Mutexes and priority inheritance](#mutexes-and-priority-inheritance) ·
[Task notifications](#task-notifications) ·
[Queues](#queues) ·
[Atomics](#atomics) ·
[Work queue](#work-queue) ·
[Kernel heap](#kernel-heap) ·
[Diagnostics](#diagnostics) ·
[Debugging](#debugging)

**[Platform support](#platform-support)**
[Supported cores](#supported-cores) ·
[Supported toolchains](#supported-toolchains) ·
[Application callbacks](#application-callbacks) ·
[Platform clock](#platform-clock) ·
[TrustZone](#trustzone) ·
[Multi-core (experimental)](#multi-core-experimental) ·
[Tickless idle (experimental)](#tickless-idle-experimental)

**[Testing and examples](#testing-and-examples)**
[Self-test suite](#self-test-suite) ·
[Examples](#examples)

**[Internals](#internals)**
[Source layout](#source-layout) ·
[Notes and constraints](#notes-and-constraints)

---

## Getting the kernel into a project

Full installation - CubeMX checkboxes, per-vendor notes, exact CMake blocks,
build and flash commands - lives in the AhuraRTOS documentation:
[Installation](installation.md),
[Vendor notes](vendor-notes.md),
[STM32CubeMX step by step](stm32cubemx.md).
This section is the short form plus the kernel-side reference those pages point
back to.

### Quick start

Five steps. Steps 1-3 copy three files and point the build at them; steps 4-5
are the only two places the kernel touches the device.

1. **Copy three files** out of the kernel into the project. Any directories
   work - the layout does not matter, only that the build can see them.

   | Copy this template | into the project as | and add it to |
   |---|---|---|
   | [`os_config_template.h`](../kernel/os_config_template.h) | `os_config.h` | nothing (it is a header) |
   | [`os_cb_template.c`](../kernel/os_cb_template.c) | `os_cb.c` | the **application** build |
   | [`os_main_template.c`](../kernel/os_main_template.c) | `os_main.c` | the **application** build |

   The kernel deliberately compiles none of the three. `os_config.h` is the
   application's configuration, and `os_cb.c` / `os_main.c` hold application
   code - see [Configuration](#configuration) and
   [Application callbacks](#application-callbacks).

2. **Build the kernel** and point it at `os_config.h`. With CMake that is two
   lines, and `OS_CONFIG_DIR` must be set *before* `add_subdirectory` so the
   kernel library and the application compile against the same configuration:

   ```cmake
   set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)  # wherever the copy lives
   add_subdirectory(AhuraRTOS/kernel)
   target_link_libraries(my_firmware ahura_kernel)
   ```

   Not using CMake? See [Adding the kernel to a
   build](#adding-the-kernel-to-a-build) for the plain file list - it is short.

3. **Give the kernel its tick.** Route the tick interrupt to
   `os_tick_handler()`. On a stock CMSIS device that is one line:

   ```c
   void SysTick_Handler(void) { os_tick_handler(); }
   ```

   If SysTick is unavailable or already taken - which is the case on Nordic
   nRF5x, among others - use a different timer instead, see
   [The integration contract](#the-integration-contract).

4. **Make sure the kernel owns PendSV.** It is the one exception the kernel
   must have, and on most projects there is nothing to do: the port defines
   `PendSV_Handler`, which is the name every CMSIS startup file already puts in
   the vector table. The exception is a vendor IDE that generates its own empty
   `PendSV_Handler` - STM32CubeMX does - which must be turned off;
   [Vendor notes](vendor-notes.md) covers it vendor by vendor.

5. **Boot it** from `main()`, after the clock tree is configured:

   ```c
   os_init();
   os_start();   /* never returns */
   ```

   `os_init()` already creates and starts a default application task, so there
   is nothing else to create just to get moving. Write the application in
   `os_main()`, in the `os_main.c` copied in step 1, and spawn further tasks
   from there with `OS_TASK_DEFINE` and `os_task_create`.

Not sure it worked? The [self-test suite](#self-test-suite) validates the whole
port with no application code.

### Adding the kernel to a build

The kernel ships a `CMakeLists.txt`, but nothing about it requires CMake. It is
plain C11 plus GCC-style inline assembly, with no generated sources and no
build-time code generation, so any toolchain that can compile a C file can build
it - Keil µVision, MPLAB X, SEGGER Embedded Studio, MCUXpresso, STM32CubeIDE
without CMake, or a hand-written Makefile.

**Source files to compile** (all of `core/`, plus exactly one arch file):

```text
core/os_atomic.c      core/os_list.c        core/os_semaphore.c
core/os_critical.c    core/os_log.c         core/os_task.c
core/os_delay.c       core/os_mem.c         core/os_tick.c
core/os_event.c       core/os_mutex.c       core/os_timer.c
core/os_kernel.c      core/os_notify.c      core/os_work.c
core/os_queue.c

arch/arm/<core>/os_arch_port.c    <- exactly ONE, matching the target
```

Pick `<core>` to match the device: `cortex_m0`, `cortex_m0plus`, `cortex_m3`,
`cortex_m4`, `cortex_m7`, `cortex_m23`, `cortex_m33`, `cortex_m35p`,
`cortex_m52`, `cortex_m55`, or `cortex_m85`. Each is a two-line wrapper that
pulls in the shared implementation for its architecture, and each carries an
`#error` guard, so a mismatch with `-mcpu` fails loudly at compile time rather
than producing a subtly wrong context switch.

Do **not** add the files under `common/` to the build. They are textual
includes, pulled in by the wrapper above; compiling them separately produces
duplicate symbols.

**Include directories** (three):

```text
<kernel root>/                      <- ahura.h
<kernel root>/arch/arm/<core>/      <- os_arch_port.h
<path to your os_config.h>/
```

**Application files**, compiled into the application, never the kernel library:
`os_cb.c` and `os_main.c` from the quick start.

That is the whole story. No linker-script edits, no preprocessor defines from
the build system (`os_config.h` is the single source of configuration - see
[Configuration](#configuration)), and no vendor headers.

New to a specific feature?
[`examples/kernel/`](../examples/kernel/)
has a minimal, standalone example per feature. See [Examples](#examples).

### Configuration

Projects never edit kernel files, and the kernel ships no editable configuration
of its own. The application owns the one and only config file, following the
same model as `FreeRTOSConfig.h`:

1. Copy `AhuraRTOS/kernel/os_config_template.h` into the project as `os_config.h`.
   Any directory works. Every option is active at its default value, so adjust
   values in place.
2. Make that directory visible to the **kernel library build**, not just the
   application, by setting `OS_CONFIG_DIR` before
   `add_subdirectory(AhuraRTOS/kernel)`:

   ```cmake
   set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)  # wherever the copy lives
   add_subdirectory(AhuraRTOS/kernel)
   ```

   If only the application saw the file, the kernel and the application would
   compile with different `OS_CONFIG_` values and their structure sizes would
   silently disagree. The kernel CMakeLists warns when `OS_CONFIG_DIR` is unset,
   and the build stops with a clear `#error` when no `os_config.h` is found or
   when it is missing options. A missing option would otherwise read as 0 in
   `#if` and silently disable features, so keep all of them. The template lists
   exactly what is required.

`os_config.h` is the single source of configuration. All options are plain
defines, so do not additionally define `OS_CONFIG_` macros from the build system
with `target_compile_definitions`, since that would redefine them. The
`OS_CONFIG_TRUSTZONE_*` and `OS_CONFIG_TICK_SOURCE_*` value macros are
kernel-owned (`os_arch_port_common.h`) and the config file only selects among
them. `OS_TASK_PRIO_MAX` is kernel-owned too, fixed at `31` in `ahura.h` because
that is the most a 32-bit ready bitmap can support, so it is not one of the
options `os_config.h` defines.

#### The option set

PART 1 is always compiled in. PART 2 is one section per feature, each holding
its `_ENABLE` switch next to the sizing that switch controls - so turning a
feature off shows exactly which values stop mattering. PART 3 is the platform.

| Option | Default | What it decides |
|---|---|---|
| **PART 1 - core** | | |
| `OS_CONFIG_TICK_HZ` | `1000U` | Tick rate. Delays, timeouts and timers all resolve to ticks |
| `OS_CONFIG_TICK_SOURCE` | `..._SYSTICK` | Who owns the tick timer: the port, or the application |
| `OS_CONFIG_TIME_SLICE_TICKS` | `1U` | Round-robin quantum; `0` turns rotation off |
| `OS_CONFIG_MAX_USER_TASKS` | `6U` | Application task slots. Service tasks are counted on top |
| `OS_CONFIG_MIN_STACK_SIZE` | `256U` | Floor for every task stack, and the idle task's own size |
| `OS_CONFIG_STACK_CHECK_ENABLE` | `1U` | Stack-pointer and guard-word check on every switch away |
| `OS_CONFIG_MAIN_TASK_STACK_SIZE` | `1024U` | Stack of the default application task (`tsk_main`) |
| `OS_CONFIG_MAIN_TASK_PRIORITY` | `OS_TASK_PRIO_1` | Priority of `tsk_main` |
| `OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY` | `0U` | `0` = mask everything with PRIMASK; nonzero = BASEPRI, see [The three barriers](#the-three-barriers) |
| **PART 2 - features** | | |
| `OS_CONFIG_MUTEX_ENABLE` | `1U` | Mutexes, always with single-level priority inheritance |
| `OS_CONFIG_SEMAPHORE_ENABLE` | `1U` | Counting semaphores |
| `OS_CONFIG_QUEUE_ENABLE` | `1U` | Fixed-item-size queues |
| `OS_CONFIG_EVENT_ENABLE` | `1U` | Event bit groups |
| `OS_CONFIG_TIMER_ENABLE` + `MAX_TIMERS` `8U`, `TIMER_STACK_SIZE` `512U`, `TIMER_PRIORITY`, `TIMER_CORE_AFFINITY` | `1U` | Software timers and the `tsk_timer` service task |
| `OS_CONFIG_WORK_ENABLE` + `MAX_WORKS` `8U`, `WORK_STACK_SIZE` `512U`, `WORK_PRIORITY`, `WORK_PAYLOAD_SIZE` `4U`, `WORK_CORE_AFFINITY` | `1U` | Work queue and the `tsk_work` service task |
| `OS_CONFIG_NOTIFY_ENABLE` | `1U` | The per-task notification mailbox |
| `OS_CONFIG_ALLOC_ENABLE` + `HEAP_SIZE` `4096U` | `1U` | The kernel heap |
| `OS_CONFIG_ATOMIC_ENABLE` | `1U` | The atomic operation set |
| `OS_CONFIG_STACK_WATERMARK_ENABLE` | `1U` | Pattern-fill stacks and report worst-case headroom |
| `OS_CONFIG_CPU_USAGE_ENABLE` | `1U` | Idle-versus-busy tick counting |
| `OS_CONFIG_ASSERT_ENABLE` | `1U` | `OS_ASSERT` and `os_assert_failed_cb` |
| `OS_CONFIG_LOG_ENABLE` + `LOG_LEVEL`, `LOG_BUFFER_SIZE` `1024U`, `LOG_LINE_MAX` `128U`, `LOG_TASK_STACK_SIZE` `512U`, `LOG_TASK_PRIORITY` | `1U` | Buffered logging and the `tsk_log` service task |
| `OS_CONFIG_TEST_ENABLE` + `TEST_STACK_SIZE` `2048U`, `TEST_PRIORITY` | `0U` | Run the self-test suite instead of `tsk_main` |
| **PART 3 - platform** | | |
| `OS_CONFIG_ARCH_PENDSV_HANDLER` | `PendSV_Handler` | Name of the context-switch vector |
| `OS_CONFIG_ARCH_VECTOR_CHECK` | `1U` | Boot-time check that the vector table routes PendSV to the kernel |
| `OS_CONFIG_TRUSTZONE` | `..._DISABLED` | Security state on ARMv8-M |
| `OS_CONFIG_CORE_COUNT` + `SPINLOCK_SOC_BACKEND` | `1U` | SMP scheduling (experimental) |
| `OS_CONFIG_TICKLESS_ENABLE` + `TICKLESS_MIN_IDLE`, `MAX_SUPPRESSED_TICKS`, `LPTIM_CLOCK_HZ` | `0U` | Tick suppression while idle (experimental) |

Three options are **optional** and may be left out, in which case the kernel
supplies the default shown: `OS_CONFIG_TICK_SOURCE`,
`OS_CONFIG_ARCH_PENDSV_HANDLER` and `OS_CONFIG_ARCH_VECTOR_CHECK`. They are
exempt because each is a name or a yes/no diagnostic that the kernel can default
correctly on its own, and a missing one is caught by `#ifndef` rather than
misread. Every other option is mandatory for the opposite reason: a missing
sizing or feature switch would read as `0` in an `#if` and silently disable or
misconfigure something, so the kernel rejects the build instead.

### The integration contract

The whole contract between the kernel and the device is two items. Everything
else in this README is behavior, not obligation.

| # | What the kernel needs | Who provides it |
|---|---|---|
| 1 | The **PendSV** exception vector | The kernel (it defines `PendSV_Handler`) - the application must only avoid defining it too |
| 2 | `os_tick_handler()` called `OS_CONFIG_TICK_HZ` times a second | The application, from a timer ISR |

That is all. The kernel does not use `SVC`, does not require `SysTick`, does not
need CMSIS headers, a HAL, or a linker-script change, and never asks the build
system for a preprocessor define.

#### 1. PendSV, the one vector the kernel owns

A context switch has to **be** the exception entry point: it manipulates the
stack frame the hardware pushed and returns through `EXC_RETURN`, neither of
which survives an ordinary C function call. So this vector cannot be "routed"
the way the tick is - the kernel has to define the handler itself.

It defines it under the name `PendSV_Handler`, the CMSIS-Pack convention that
essentially every vendor's startup file already places in the vector table. On
a normal project this means **there is nothing to do**.

Two things can go wrong, and both are worth knowing:

- **Something else defines `PendSV_Handler` too.** The link fails with a
  duplicate-symbol error. The fix is to remove the other definition - it is
  almost always an empty stub generated by a vendor IDE. Note that the `.weak`
  in a startup file does *not* help here: it only covers the default handler,
  not a second real one.
- **The vector table calls the entry something else.** A hand-written startup
  file, a non-CMSIS environment, or a bootloader's own table. Point the kernel
  at the right name in `os_config.h`:

  ```c
  #define OS_CONFIG_ARCH_PENDSV_HANDLER  my_pendsv_vector_name
  ```

The kernel checks this for itself at boot - see
[Boot sequence](#boot-sequence).

> **`SVC` is not used and never has been claimed.** Starting the first task
> through `svc 0` is the traditional Cortex-M approach - FreeRTOS does it - but
> it spends a second permanent vector on one action performed once at boot, and
> `SVC` is the most contended exception on the architecture: Nordic's SoftDevice
> reserves part of the SVC number space, TF-M and other secure firmware use it
> for gateway calls, and vendor bootloaders and ROM APIs use it. Ahura folds the
> first-task start into PendSV's "no task has run yet" path instead, so `SVC` is
> entirely the application's. Define `SVC_Handler` however you like.

#### 2. The tick

The kernel needs `os_tick_handler()` called at `OS_CONFIG_TICK_HZ`. It drives
delays, timeouts, timer expiry and round-robin preemption; nothing else in the
kernel reads a clock. `OS_CONFIG_TICK_SOURCE` picks who owns the timer.

**`OS_CONFIG_TICK_SOURCE_SYSTICK`** (the default). The port programs SysTick
from the live CPU clock. The application routes the vector, and that handler
does one thing:

```c
void SysTick_Handler(void) { os_tick_handler(); }
```

**`OS_CONFIG_TICK_SOURCE_EXTERNAL`.** The port touches no timer hardware. It
calls `os_arch_tick_init_cb()` once from `os_init()`, where the application
starts whatever timer it wants, and that timer's ISR calls `os_tick_handler()`.
Both live in `os_cb.c`; the template shows a full example.

Choose EXTERNAL when SysTick is unusable or unavailable:

- **It stops in low-power modes on several families.** Nordic nRF5x drives time
  from the RTC peripheral for exactly this reason - a SysTick-based kernel there
  loses time whenever the CPU sleeps.
- **A vendor HAL or bootloader already owns it** and will not give it up.
- **The device does not implement it.**

Either way, give the tick interrupt the **lowest** priority the device offers.
It drives preemption, so it must never itself preempt an application interrupt.
The port does this for SysTick automatically; with EXTERNAL it is the
application's job.

> **If the vendor HAL also wants SysTick**, do not try to share the handler by
> calling both `os_tick_handler()` and the HAL's tick. Two schedulers on one
> interrupt is a recipe for drift and for HAL timeouts that behave differently
> depending on what else is running. Move one of them: give the HAL a spare
> hardware timer (the usual choice), or leave the HAL on SysTick and give the
> kernel a timer through `OS_CONFIG_TICK_SOURCE_EXTERNAL`.

---

## How it works

This part is the mechanism: which data structures exist, what runs on every
tick, what a context switch physically does, and why each piece is where it is.
[Using the kernel](#using-the-kernel) covers the same subsystems from the API
side - what to call and what it guarantees.

### Boot sequence

`os_init()` builds the kernel in a fixed order, then `os_start()` hands the CPU
to the first task and never comes back.

| Step | Call | What it does |
|---|---|---|
| 1 | `os_arch_init()` | Verifies the vector table, primes the PSP sentinel, sets exception priorities, brings up the cycle counter |
| 2 | `os_task_system_init()` | Clears the TCB table, empties the 32 ready lists, the bitmap and the delay list |
| 3 | `os_task_idle_create()` | One idle task per scheduling core, at `OS_TASK_PRIO_IDLE`, outside the task table |
| 4 | `os_work_system_init()` | `tsk_work`, if `OS_CONFIG_WORK_ENABLE` |
| 5 | `os_timer_system_init()` | `tsk_timer`, if `OS_CONFIG_TIMER_ENABLE` |
| 6 | `os_log_system_init()` | `tsk_log`, if `OS_CONFIG_LOG_ENABLE` - created before the application task, so anything logged at startup already has a consumer |
| 7 | `os_main_system_init()` **or** `os_test_system_init()` | `tsk_main` running `os_main()`, or `tsk_test` running `os_test()` in a self-test build. Never both |
| 8 | `os_tick_init()` | Zeroes the tick counter and starts the tick source (or calls `os_arch_tick_init_cb()` with an external tick) |

Everything above runs with the scheduler stopped, on the boot (MSP) stack. No
task executes yet - creation only fills in a TCB and pushes it onto a ready
list.

**What `os_arch_init()` checks.** Misintegration is caught here rather than
allowed to become a silent hang:

- **The vector table.** With `OS_CONFIG_ARCH_VECTOR_CHECK` (default on), the
  port reads entry 14 through VTOR and compares it against the address its own
  handler assembled to. A mismatch parks the core immediately in
  `os_arch_config_fault_trap()`, where a debugger lands on the cause. Without
  that check, the failure mode is a board that reaches `os_start()` and stops
  dead - no fault, no output, nothing to attach to.
- **The interrupt-mask configuration.** With a nonzero
  `OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY`, the value must fit the device's
  implemented priority bits exactly, and NVIC grouping must give every
  implemented bit to preemption. Both are read back from the hardware and
  verified.
- **The cycle counter.** DWT CYCCNT is architecturally optional, may report
  `DWT_CTRL.NOCYCCNT`, and on several devices sits behind a debug power domain
  that refuses the enable until a debugger attaches. All three cases are
  checked, and a SysTick-derived counter is selected instead when needed - which
  is also the only counter ARMv6-M has. Without that fallback `os_delay_us()`
  would spin forever on such a part.

`os_arch_init()` also sets PendSV and SysTick to the **lowest** exception
priority, so neither can ever preempt an application interrupt, and writes
`PSP = 0` - the sentinel the context switch reads as "no task has run yet".

**What `os_start()` does.** It asserts that the idle task exists (without one,
the first restore would dereference a NULL stack pointer), sets the running
flag, and calls `os_arch_start_first_task()`, which switches `CONTROL` to use
PSP, re-writes the `PSP = 0` sentinel, and pends PendSV. Since PendSV is the
lowest priority and interrupts are enabled, it is taken immediately; the
function after it is never reached.

### Tasks and the TCB

A task is a stack plus a control block. `OS_TASK_DEFINE(name, stack_bytes)`
declares both at file scope and binds them to a handle at compile time;
`os_task_create` fills in the TCB from an `os_task_config_t` and claims a slot
in the static table.

The TCB (`os_task_tcb_t`, private to `os_task.c`) holds:

| Field group | Contents |
|---|---|
| Identity | `name` (kept only for debugger and trace visibility - the kernel never reads it), `id`, `system_task` flag |
| Stack | `stack_base`, `stack_bytes`, and `stack_ptr` - the saved PSP, written by the context switch |
| Scheduling | `priority` (effective), `state`, `core_affinity`, `running_core`, `delay_ticks` |
| List membership | `state_node` - links into **one** ready list or the delay list; `wait_node` - links into **one** object's waiter list |
| Wait bookkeeping | `wait_list`, `wait_signaled`, `wait_data[2]`, `wait_result`, `woken_from` |
| Mutex (optional) | `base_priority` and an `owned_mutexes` list, for priority inheritance |
| Notify (optional) | A one-word mailbox: `value`, `pending`, `waiting` |

Two separate list nodes is what lets a task be on a waiter list and the delay
list at the same time - which is exactly what a `timeout_ms` wait is.

**The task table is sized to mean what it says.**

```text
OS_TASK_TABLE_SIZE = OS_CONFIG_MAX_USER_TASKS
                   + 1                              (tsk_main or tsk_test)
                   + 1 per enabled timer / work / log service
```

So enabling the log or the work queue never quietly costs the application a
task slot. The idle tasks live outside the table entirely, one per core.

**States.** `os_task_state_get()` reports one of five:

```mermaid
stateDiagram-v2
    [*] --> INACTIVE
    INACTIVE --> SUSPENDED: os_task_create
    SUSPENDED --> READY: os_task_start
    READY --> RUNNING: scheduler dispatch
    RUNNING --> READY: preempted / yield / slice end
    RUNNING --> BLOCKED: delay or wait with timeout
    BLOCKED --> READY: signaled or timeout
    RUNNING --> SUSPENDED: os_task_pause
    RUNNING --> INACTIVE: os_task_delete / entry returns
```

A created-but-not-started task is `SUSPENDED`, so `os_task_start` is a real
step rather than a formality. A task whose entry function returns is not a
fault: the kernel wraps every entry so the return lands in `os_task_exit()`,
which deletes the task cleanly.

### The scheduler

Three structures, all static, all shared between cores on an SMP build:

```c
static os_list_t os_task_ready_list[OS_TASK_PRIO_MAX + 1U];  /* 32 FIFO lists  */
static uint32_t  os_task_ready_bitmap;                       /* bit n = list n non-empty */
static os_list_t os_task_delay_list;                         /* finite-delay sleepers    */
```

**Picking the next task is O(1).** `os_task_stack_select_next()` asks
`os_arch_highest_bit_get()` for the highest set bit in the bitmap - a single
`CLZ` instruction on ARMv7-M and up - indexes straight into that priority's
list, and takes the head. Removing the last member clears the bit. There is no
scan over tasks and no priority queue to re-heap, so the cost does not depend on
how many tasks exist or how many priorities are in use.

**Round-robin is list rotation.** A task that was preempted while `RUNNING` goes
back to the **tail** of its own priority's list in
`os_task_stack_save_current()`; the next pick takes the head. Nothing is sorted,
and equal-priority tasks therefore rotate in FIFO order for free. A task that
blocked or suspended itself keeps its state - it is already in the right list,
or in none - and the idle tasks are never queued at all.

**Who is not in any list:** the running task on each core (it is in
`os_task_current[core]`), `OS_WAIT_FOREVER` sleepers (only an explicit wake
releases them, so a tick has no reason to walk past them), suspended tasks, and
the idle tasks.

**The time slice.** `os_task_slice_left[core]` is reloaded to
`OS_CONFIG_TIME_SLICE_TICKS` every time a task is dispatched - including a task
re-dispatched immediately after a yield, which is what makes `os_task_yield()` a
genuine restart rather than a way to inherit the tail of the previous slice.
`os_task_slice_tick()` counts it down once per tick, on every core. Only this
core writes that counter, and a 32-bit store is indivisible, so it needs no
lock.

**Deciding whether a tick should switch at all.**
`os_task_reschedule_possible()` answers in three tests, cheapest first:

| Test | Result |
|---|---|
| Scheduler locked on this core | `false` outright |
| Any ready bit **strictly above** the running priority | `true` - a real preemption is due |
| Slice exhausted **and** a ready bit at the running priority | `true` - a peer's turn has come |

Because the running task is never queued, "a bit at my own priority" can only
mean a peer is waiting. The tick calls this before pending PendSV, so a tick
that would not switch anything costs one bitmap check instead of a full
exception round trip - which, with a quantum above 1, is most ticks.

### The context switch

PendSV does everything, in one handler, for both jobs: starting the very first
task and switching between two running ones.

```mermaid
flowchart TD
    A[PendSV entry] --> B{PSP == 0?}
    B -- yes --> F[first start: nothing to save]
    B -- no --> C[save s16-s31 if the task used the FPU]
    C --> D[push r4-r11 and EXC_RETURN]
    D --> E[os_task_stack_save_current]
    E --> G[os_task_stack_select_next]
    F --> G
    G --> H[reset MSP from the vector table<br/>first start only]
    G --> I[clrex, pop r4-r11 and EXC_RETURN]
    H --> I
    I --> J[restore s16-s31 if that task used the FPU]
    J --> K[msr psp, dsb, isb, bx lr]
```

**The frame on a task stack**, low address first:

```text
[ s16-s31 ]   only when the task was using the FPU (EXC_RETURN bit 4 clear)
r4-r11, EXC_RETURN
[ hardware frame: r0-r3, r12, lr, pc, xpsr, (s0-s15, fpscr) ]
```

The hardware pushes the bottom group on exception entry; the handler pushes the
middle group. Details worth knowing:

- **`EXC_RETURN` is saved *with* the context**, per task. That is what lets one
  task have an FPU frame and another not, which is mandatory with
  `-mfloat-abi=hard` where any task - or the startup code - may touch the FPU.
- **`clrex` on the restore path** drops any `LDREX` reservation the outgoing
  task left behind, so an interrupted atomic cannot have its `STREX` succeed in
  a different context.
- **`os_task_stack_select_next()` never returns NULL.** The idle task always
  exists, so the restore path needs no fallback branch.
- **The first start abandons the boot context.** With `PSP == 0` there is
  nothing to save, so the handler resets MSP to the initial value from the
  vector table - discarding `main()`'s frame and the exception frame it just
  pushed - and returns through PSP into the first task. The MSP stack is then
  the handler stack, as intended.

**What changes per architecture:**

| Port | Cores | Differences |
|---|---|---|
| `v7m` | M3, M4, M7 | Thumb-2, FPU-aware (`s16-s31` + per-task `EXC_RETURN`), BASEPRI available |
| `v8m` | M33, M35P, M52, M55, M85 | A superset of v7m: also saves and restores `PSPLIM` per task, programs `MSPLIM` for the handler stack, and carries all three TrustZone modes |
| `v6m` | M0, M0+, M23 | Thumb-1 subset, no FPU, no BASEPRI (PRIMASK only), cycle counter synthesized from SysTick |

The port layer defines exactly **one** externally visible vector handler. It
does not define `SVC_Handler` (the first task starts through PendSV's `PSP == 0`
path) and does not define `SysTick_Handler` (the application routes it).

**Where the switch is requested from.** Nothing calls the scheduler directly.
Every path that can make a switch due - the tick, a wake, a yield, a priority
change, the outermost `os_kernel_unlock()` - pends PendSV, which then runs after
every other pending interrupt because it sits at the lowest priority. So a
context switch never happens in the middle of an ISR, and the switching code
exists in exactly one place.

### The tick

One tick is deliberately short. `os_tick_handler()` does this and nothing else:

1. Increment `os_tick_count` (core 0 only on an SMP build - other cores' ticks
   drive only their own preemption, or elapsed time would be counted once per
   core).
2. Count the tick for CPU-usage sampling, and additionally as idle if it
   interrupted the idle task.
3. `os_work_tick_process(1)` - advance delayed work items.
4. `os_timer_tick_process(1)` - decrement active timers, mark expiries, wake
   `tsk_timer` if any fired.
5. `os_task_tick_update(1)` - walk the delay list, decrement, and make ready
   whatever reached zero.
6. `os_task_slice_tick(1)` - count down this core's quantum.
7. Pend PendSV **only if** `os_task_reschedule_possible()` says a switch would
   really happen.

Step 5 costs O(sleeping tasks), not O(task table), because only finite-delay
sleepers are in the delay list. Steps 3 and 4 are fixed-slot array scans bounded
by `OS_CONFIG_MAX_WORKS` and `OS_CONFIG_MAX_TIMERS`.

No callback of any kind runs in the tick. An expired timer marks its object and
wakes `tsk_timer`; a ready work item wakes `tsk_work`. Both callbacks therefore
execute in task context, where they may use kernel APIs, block, and be preempted
like anything else.

`os_tick_announce(n)` is the same sequence for `n` ticks at once, called from
task context after a tickless sleep. It guards the counter update against a
concurrent tick interrupt; the tick handler does not need to, being the only
writer in the normal case.

### Blocking and waking

Every blocking primitive is built from the same four internal calls, so mutex,
semaphore, queue, event and notify all behave identically at the edges.

```c
os_task_wait_data_set(d0, d1);            /* optional: condition data for a match waker */
os_task_wait_begin(&object->waiters, timeout_ticks);
/* ... the caller exits its critical section; the switch happens there ... */
signaled = os_task_wait_signaled();       /* true = object signaled, false = timeout */
os_task_wait_end();                       /* consumed - every exit path calls this   */
```

**Joining a waiter list.** `os_task_wait_begin()` inserts the task into the
object's waiter list **in priority order**, sets `state = BLOCKED`, and - for a
finite timeout - also links it into the delay list with `delay_ticks` set. That
is why a wait can end two ways: `os_task_waiters_wake_one()` takes the list head
(the highest-priority waiter, FIFO among equals) and sets `wait_signaled`, while
the tick's delay-list walk unlinks the task from **both** lists and leaves
`wait_signaled` false. The two are distinguishable, so the caller returns
`OS_STATUS_TIMEOUT` or retries accordingly.

**Three wake shapes**, each matching what the object means:

| Waker | Used by | Effect |
|---|---|---|
| `os_task_waiters_wake_one` | mutex unlock, semaphore give, queue send/receive | Wakes the highest-priority waiter only |
| `os_task_waiters_wake_all` | event `set_bits` | Wakes every waiter, so each re-evaluates its own bit condition |
| `os_task_waiters_wake_match` | conditional deliveries | Calls a per-waiter predicate with that waiter's two words of `wait_data`, wakes only those it confirms, and stores a per-waiter result for `os_task_wait_result_get()` |

**Wakeups are advisory, not a handover.** A woken task re-checks the condition
it was waiting on; a faster third task that took the object in between simply
sends it back to waiting. The remaining budget for that second wait is computed
by `os_internal_wait_remaining()` against the **wall clock** - the tick at which
the wait started - not by re-arming the original relative timeout. Time spent
`READY` between the wake and the re-check therefore counts against the timeout;
a relative re-arm would freeze the clock and let a busy system stretch a 10 ms
wait indefinitely.

**When a wait cannot block at all.** `os_internal_can_block()` is false in an
ISR, before `os_start()`, and while the calling core holds a scheduler lock. In
all three cases every blocking primitive degrades to its `OS_WAIT_NOTHING`
behavior rather than parking a task the kernel could not switch away from.

### Priority inheritance mechanics

The state lives in the TCB, not in the mutex: `base_priority` is what the task
was configured with, `priority` is what the scheduler actually uses, and
`owned_mutexes` is a list of the mutexes it currently holds (linked through an
`owner_node` embedded in each mutex).

- **On a contended lock**, before joining the waiter list, the blocking task
  calls `os_task_mutex_priority_inherit(owner_id)`. If the owner's effective
  priority is lower, it is raised to the waiter's - and if the owner is sitting
  in a ready list, it is moved to the list for its new priority, with the bitmap
  updated on both ends.
- **On unlock**, `os_task_mutex_owner_unlink_and_reprioritize()` removes that
  mutex from the owner's list and recomputes the owner's effective priority as
  `max(base_priority, highest waiter still queued on any mutex it still holds)`.
  Recomputing against every remaining mutex, rather than just restoring
  `base_priority`, is what keeps a task holding two contended mutexes correct.

Two limits follow directly from this design, and both are accepted rather than
worked around: the boost does not propagate through a **chain** (an owner itself
blocked on a second mutex), and it is recomputed **lazily**, at the owner's next
lock or unlock, rather than eagerly when a waiter times out. See [Mutexes and
priority inheritance](#mutexes-and-priority-inheritance) for what that means in
practice.

### The three barriers

The kernel offers three different ways to make something indivisible, and they
exclude different things. Picking the wrong one is the classic source of races
that only appear under load.

| Barrier | Excludes | Does **not** exclude | Cost |
|---|---|---|---|
| `os_critical_enter/exit` | Interrupts on this core, and (SMP) other cores via the kernel spinlock | Nothing - it is the strongest | Interrupt latency for the whole region |
| `os_kernel_lock/unlock` | Other **tasks** on this core | Every ISR, and every other core | None to interrupt latency |
| `os_atomic_*` | Concurrent updates of that one word | Anything about other words | One `LDREX`/`STREX` loop, or a critical section on ARMv6-M |

**How the critical section masks.** `OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY` selects
the backend:

- **`0` (default): PRIMASK.** `cpsid i` masks everything. Every ISR may call the
  ISR-safe kernel APIs. This is the only option on cores without BASEPRI (M0,
  M0+, M23).
- **Nonzero: BASEPRI.** The kernel raises `BASEPRI` to that value with
  `basepri_max`, which only ever tightens - so a nested save can never loosen a
  threshold an outer level already raised. Interrupts **more urgent** than the
  threshold are never masked by the kernel at all, giving them zero
  kernel-induced latency, but they must not call any kernel API. ISRs at the
  threshold or below keep full API access. The value is the raw NVIC priority
  byte, already shifted into the device's implemented bits
  (`logical << (8 - __NVIC_PRIO_BITS)`), and it is verified against the hardware
  at boot.

Nesting is counted in both cases, and the mask is save/restore rather than an
unconditional enable at exit, so a critical section inside an ISR does not
re-enable interrupts on the way out.

**How the scheduler lock defers.** `os_kernel_lock()` increments a per-core
counter and nothing else - no interrupt is touched. Every path that would pend
PendSV consults that counter, and a switch that gets swallowed sets
`os_kernel_switch_pending[core]`. The outermost `os_kernel_unlock()` clears the
flag and pends PendSV *after* dropping the count to zero, so the resulting
exception finds the scheduler open and really does switch. A PendSV that was
already pending when the lock was taken still runs, and it too is caught: the
switch path sees the lock, hands the running task's own stack pointer straight
back, and records the pending flag.

### Kernel service tasks

Three of the kernel's subsystems need to run application callbacks in task
context, so each gets its own task, created by `os_init()` and protected from
`os_task_pause` / `os_task_delete` (both return `OS_STATUS_BUSY`) - pausing one
would turn every later call into a silent no-op that still reported success.

| Task | Default priority | Fed by | Runs |
|---|---|---|---|
| `tsk_timer` | `OS_TASK_PRIO_MAX` | The tick, which decrements `os_timer_registry[]` and marks expiries | One-shot and periodic timer callbacks |
| `tsk_work` | `OS_TASK_PRIO_MAX` | The tick, which ages each slot's `delay_ticks` and flags it ready | Submitted work handlers |
| `tsk_log` | `OS_CONFIG_LOG_TASK_PRIORITY` (low) | `OS_LOG_*` calls from any context | Drains the log ring into `os_log_output_cb` |

**Timers** are a fixed registry of `OS_CONFIG_MAX_TIMERS` pointers. Each
`os_timer_t` carries its own `period_ticks`, `remaining_ticks`, mode, and
`active` / `paused` / `expired` flags; the tick decrements and sets `expired`,
then wakes `tsk_timer` once for the whole batch. Arming past capacity returns
`OS_STATUS_FULL` rather than dropping a timer silently.

**Work items** are a fixed array of `OS_CONFIG_MAX_WORKS` slots, each holding
the handler, a `delay_ticks` countdown, a `ready` flag, and the payload copied
inline into a `uint64_t`-aligned union of `OS_CONFIG_WORK_PAYLOAD_SIZE` bytes.
The slot is released as the handler starts, not when it finishes, so a handler
may submit again. Because the payload is copied, the caller's buffer may go out
of scope the moment `os_work_submit` returns.

**The log** is a byte ring buffer of `OS_CONFIG_LOG_BUFFER_SIZE` with a head, a
tail and a dropped counter. `os_log_write` formats into an
`OS_CONFIG_LOG_LINE_MAX` scratch buffer on the **caller's** stack, then copies
the finished line into the ring under the kernel mask - whole, or not at all.
The dropped count is emitted into the log itself once space frees. `tsk_log`
runs deliberately low so draining never preempts application work, and the
transport callback runs there, never from an ISR, so it may block or start a
DMA transfer.

### The kernel heap (internals)

`OS_CONFIG_ALLOC_ENABLE` compiles in a static `uint8_t
os_mem_heap[OS_CONFIG_HEAP_SIZE]`. Nothing is taken from the linker heap, so
`malloc` and the kernel never compete, and the footprint is visible in the map
file.

Each block carries a two-word header:

```c
typedef struct os_mem_block { struct os_mem_block *next; size_t size; } os_mem_block_t;
```

Free blocks form a single **address-ordered** list starting at a static sentinel.
Allocation is **first-fit**: walk until a block is large enough, split it if the
remainder is worth keeping, and return the payload 8-byte aligned. Freeing
inserts by address and **coalesces** with the physically adjacent neighbours on
both sides, which is what keeps mixed alloc/free patterns from fragmenting
permanently - the same structure FreeRTOS's `heap_4` uses.

The walk runs inside the kernel critical section, which is what makes
`os_mem_alloc` callable from an ISR - though allocating in an ISR is discouraged
precisely because that walk runs with interrupts masked.

### Where the RAM goes

Every kernel object is statically sized, so a build's RAM cost is a sum of
`os_config.h` values:

| Consumer | Size |
|---|---|
| Task table | `(OS_CONFIG_MAX_USER_TASKS + service slots)` × `sizeof(os_task_tcb_t)` |
| Idle stacks | `OS_CONFIG_CORE_COUNT` × `OS_CONFIG_MIN_STACK_SIZE` |
| Ready lists + bitmap + delay list | 32 list heads + one word + one list head |
| Application stacks | Whatever each `OS_TASK_DEFINE` asks for |
| `tsk_main` / `tsk_test` | `OS_CONFIG_MAIN_TASK_STACK_SIZE` or `OS_CONFIG_TEST_STACK_SIZE` |
| Timer service | `OS_CONFIG_TIMER_STACK_SIZE` + `OS_CONFIG_MAX_TIMERS` pointers |
| Work service | `OS_CONFIG_WORK_STACK_SIZE` + `OS_CONFIG_MAX_WORKS` × (slot header + `OS_CONFIG_WORK_PAYLOAD_SIZE`) |
| Log service | `OS_CONFIG_LOG_TASK_STACK_SIZE` + `OS_CONFIG_LOG_BUFFER_SIZE` (+ `OS_CONFIG_LOG_LINE_MAX` transiently, on the stack of whichever task logs) |
| Heap | `OS_CONFIG_HEAP_SIZE` |

Turning a feature off removes its code, its API **and** its RAM - including the
service task and stack for timer, work and log.

---

## Using the kernel

### API at a glance

Everything below is declared in the single public header, `ahura.h`. Each group
compiles away entirely when its `OS_CONFIG_<FEATURE>_ENABLE` is 0.

| Group | Functions |
|---|---|
| **Lifecycle** | `os_init` · `os_start` · `os_kernel_is_running` · `os_core_start` |
| **Tasks** | `os_task_create` · `os_task_start` · `os_task_pause` · `os_task_delete` · `os_task_yield` · `os_task_state_get` · `os_task_priority_get` · `os_task_priority_set` · `os_task_core_affinity_set` |
| **Delays and time** | `os_delay_ms` · `os_delay_us` · `os_delay_s` · `os_tick_get` |
| **Critical sections** | `os_critical_enter` · `os_critical_exit` |
| **Scheduler lock** | `os_kernel_lock` · `os_kernel_unlock` · `os_kernel_is_locked` |
| **Atomics** | `os_atomic_get` · `os_atomic_set` · `os_atomic_add` · `os_atomic_sub` · `os_atomic_inc` · `os_atomic_dec` · `os_atomic_or` · `os_atomic_and` · `os_atomic_xor` · `os_atomic_nand` · `os_atomic_clear` · `os_atomic_cas` · `os_atomic_test_bit` · `os_atomic_set_bit` · `os_atomic_clear_bit` · `os_atomic_test_and_set_bit` · `os_atomic_test_and_clear_bit` · `os_atomic_set_bit_to` |
| **Mutex** | `os_mutex_init` · `os_mutex_lock` · `os_mutex_try_lock` · `os_mutex_unlock` |
| **Semaphore** | `os_semaphore_init` · `os_semaphore_give` · `os_semaphore_take` |
| **Queue** | `OS_QUEUE_DEFINE_STATIC` · `OS_QUEUE_DEFINE_BUFFER` · `OS_QUEUE_DEFINE_DYNAMIC` · `os_queue_init_dynamic` · `os_queue_send` · `os_queue_receive` · `os_queue_count_get` · `os_queue_free_get` · `os_queue_cleanup` |
| **Event** | `os_event_init` · `os_event_set_bits` · `os_event_clear_bits` · `os_event_wait_bits` |
| **Task notifications** | `os_notify_give` · `os_notify_wait` |
| **Software timers** | `os_timer_init` · `os_timer_start` · `os_timer_restart` · `os_timer_pause` · `os_timer_stop` · `os_timer_delete` |
| **Work queue** | `os_work_submit` |
| **Kernel heap** | `os_mem_alloc` · `os_mem_free` · `os_mem_free_get` · `os_mem_watermark_get` |
| **Diagnostics** | `os_task_stack_watermark_get` · `os_cpu_usage_get` · `os_stack_overflow_cb` |
| **Debugging** | `OS_ASSERT` · `os_assert_failed_cb` · `OS_LOG_ERROR` / `OS_LOG_WARN` / `OS_LOG_INFO` / `OS_LOG_DEBUG` · `os_log_write` · `os_log_dropped_get` · `os_log_output_cb` |
| **Intrusive list** | `os_list_init` · `os_list_is_empty` · `os_list_push_back` · `os_list_pop_front` · `os_list_remove` · `os_list_insert_before` |
| **Tickless idle** | `os_tickless_idle_process` · `os_tickless_expected_idle_ticks_get` · `os_tickless_max_suppressed_ticks_get` |
| **Application-provided** | `os_main` · `os_test` · `os_tickless_pre_sleep_cb` · `os_tickless_post_sleep_cb` · `os_arch_tz_context_save_cb` · `os_arch_tz_context_restore_cb` · `os_arch_core_id_get_cb` · `os_arch_core_ipi_request_cb` |

Helper macros: `OS_TASK_DEFINE` (name, stack and handle), `OS_TASK_CONFIG` (what
the task does), `OS_TICKS_FROM_MS`, `OS_WAIT_NOTHING`, and `OS_WAIT_FOREVER`.

Every call that can fail returns an `os_status`: `OK`, `ERROR`, `INVALID_ARG`,
`EMPTY`, `FULL`, `BUSY`, `TIMEOUT`, `NOT_OWNER`, `NO_MEMORY`.

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

`OS_TASK_CONFIG` follows the core count. On a single-core build it takes
`(entry, context, priority)`, because there is nothing to place a task on. On a
multi-core build it takes a fourth `core_affinity` argument, required rather
than defaulted, so every task states where it may run:

```c
/* OS_CONFIG_CORE_COUNT > 1 */
os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_3,
                                       OS_TASK_CORE(1) | OS_TASK_CORE(2)));
```

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

Override it with its own template, separate from `os_cb_template.c`. Copy
`AhuraRTOS/kernel/os_main_template.c` into the project as `os_main.c`, add it to the
**application** build (never to the kernel, where it is deliberately absent from
the CMakeLists, just like `os_cb_template.c`), and replace `os_main()`'s body
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
task. See [Self-test suite](#self-test-suite). `os_main()` itself still compiles,
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
| `31` | `OS_TASK_PRIO_MAX` | Kernel: `tsk_work` and `tsk_timer` by default |

- **`OS_TASK_PRIO_1_LOWEST` through `OS_TASK_PRIO_30_HIGHEST` is the user
  range**, and those two names *are* the limits - there is no separate pair of
  range constants to keep in sync with them. `os_task_create` and
  `os_task_priority_set` reject anything outside it with
  `OS_STATUS_INVALID_ARG`.
- **`OS_TASK_PRIO_IDLE` is the empty-ready-bitmap fallback.** The idle task must
  be the only thing at that level, or the scheduler could pick a real task when
  it means to idle - so it is out of reach of `os_task_create`.
- **`OS_TASK_PRIO_MAX` is kept out of reach too**, so the kernel's service tasks
  `tsk_work` and `tsk_timer` - which `os_init()` creates automatically - have a
  level nothing else can claim. That is where `OS_CONFIG_WORK_PRIORITY` and
  `OS_CONFIG_TIMER_PRIORITY` put them by default; either may be lowered into the
  user range when a user task should outrank deferred work or timer callbacks.
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
`OS_CONFIG_TEST_PRIORITY`. Unlike `tsk_work` and `tsk_timer` they are ordinary
application tasks, so pick values that fit alongside your own.

The kernel's own service tasks - `tsk_timer`, `tsk_work` and `tsk_log` - are
also protected: `os_task_pause` and `os_task_delete` refuse them with
`OS_STATUS_BUSY`, because the timer, work and log APIs are all built on one
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
barriers](#the-three-barriers) for how each one is implemented.

While the lock is held the calling task cannot block, because blocking means
switching away. Every blocking primitive behaves as if it had been called with
`OS_WAIT_NOTHING`, `os_delay_ms` busy-waits instead of sleeping, and
`os_task_pause`/`os_task_delete` aimed at the *calling* task return
`OS_STATUS_BUSY`. Keep locked regions short and free of blocking calls, exactly
as with a critical section.

### Timeout semantics

Blocking APIs (`os_mutex_lock`, `os_semaphore_take`, `os_queue_send`,
`os_queue_receive`, `os_event_wait_bits`, `os_notify_wait`) take a
`timeout_ms` argument:

| Value | Behavior |
|---|---|
| `OS_WAIT_NOTHING` | Try once, return `BUSY`, `EMPTY`, or `FULL` immediately. |
| `1..N` ms | Wait up to that long, then return `OS_STATUS_TIMEOUT`. |
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
waking](#blocking-and-waking).

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
either, so locking a mutex the caller already holds fails with `OS_STATUS_BUSY`
rather than deadlocking.

### Task notifications

A lightweight, single-value mailbox built directly into every task's own control
block, enabled by `OS_CONFIG_NOTIFY_ENABLE`. It lets you signal one
specific task without allocating a separate semaphore or queue object:

```c
os_notify_give(&some_task, 42U);         /* ISR-safe; overwrite: last write wins */
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

os_status status = os_queue_init_dynamic(&rx_q, item_size, capacity);
...
os_queue_cleanup(&rx_q);         /* returns the buffer to the heap */
```

Keeping the queue object out of the allocation means its lifetime stays obvious
and a failed call leaves nothing to clean up. `os_queue_init_dynamic` returns
`OS_STATUS_NO_MEMORY` when the heap cannot satisfy the request, and
`OS_STATUS_INVALID_ARG` for a zero or overflowing geometry rather than wrapping
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

It is not compiled out with the heap, and returns `OS_STATUS_BUSY` while any
task is still blocked on the queue, because freeing underneath waiters would
leave them parked on list nodes inside memory the heap can hand out again. Drain
the queue and let the waiters time out first.

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
two backends, selected by `OS_ARCH_ATOMIC_LOCK_FREE` in
`os_arch_port_common.h`:

| Backend | Cores | How | Cost |
|---|---|---|---|
| Lock-free | Cortex-M3, M4, M7, M33, M35P, M52, M55, M85 | One `LDREX`/`STREX` retry loop per operation, each a single asm block | Interrupts stay enabled; interference costs a retry, not correctness |
| Critical section | Cortex-M0, M0+, M23 | Each operation inside `os_critical_enter`/`os_critical_exit` | Adds the update's length to interrupt latency, and can wait on unrelated kernel work on multi-core builds |

The second row exists because ARMv6-M has no instruction that can *detect*
interference mid-update, so it has to be prevented instead. Worth knowing before
putting an atomic in an ARMv6-M interrupt-latency budget. All of them are safe
to call from tasks and from ISRs.

**Both backends are inlined into the caller**, with `always_inline` rather than
a plain `inline` so it holds at `-O0` as well. Every one of them has exactly one
caller - the matching one-line wrapper in `os_atomic.c`, which validates its
argument and forwards - so leaving them in the port translation unit meant every
atomic paid a real call and stack frame to reach five instructions. Inlining
costs nothing in size either, because a body this small is smaller than the call
sequence that used to reach it. On a Cortex-M7 at `-Os`, `os_atomic_add()` is
now a NULL check followed by the bare `LDREX`/`ADD`/`STREX`/`CMP`/`BNE` loop,
with no call anywhere in it.

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

### Work queue

Defer a function to run later on the highest-priority kernel task. One call, and
it is ISR-safe:

```c
static void my_handler(void *data, size_t len) { /* runs on tsk_work */ }

my_payload_t payload = { ... };                                  /* an ordinary local */

os_work_submit(my_handler, &payload, sizeof(payload), 100U);     /* 0 ms = as soon as possible */
os_work_submit(my_handler, NULL, 0U, 0U);                        /* or carry no payload at all */
```

There is no work object to declare, initialize or keep alive, and **the payload
is copied, not referenced**. The kernel takes the handler and the `len` bytes at
`data` into one of its `OS_CONFIG_MAX_WORKS` slots, then releases the slot as the
handler starts and hands it the copy. So the buffer above may go out of scope the
moment `os_work_submit` returns - a submission is complete in itself.

`OS_CONFIG_WORK_PAYLOAD_SIZE` (default 4 bytes) bounds it; anything larger is
refused with `OS_STATUS_INVALID_ARG` rather than truncated. Raising it costs
`OS_CONFIG_MAX_WORKS` × that many bytes of RAM, plus the same again on the work
task's stack while a handler runs. To hand over something bigger, submit a
**pointer** to it:

```c
os_work_submit(my_handler, &object_ptr, sizeof(object_ptr), 0U);
```

which keeps the fact that the target's lifetime is now yours to manage visible at
the call site, instead of being the silent default.

Two consequences of having no handle, both deliberate:

- **Each submission is its own call.** Submitting the same handler twice runs it
  twice; there is no item for the second submission to reschedule.
- **A submission cannot be cancelled or inspected.** If a handler needs to be
  able to change its mind, give it a context it re-reads when it runs.

`os_work_submit` returns `OS_STATUS_FULL` when every slot is occupied, so a burst
larger than `OS_CONFIG_MAX_WORKS` is refused rather than silently dropped.

Handlers and timer callbacks run in task context, so they may use kernel APIs,
but at the default `OS_CONFIG_WORK_PRIORITY` / `OS_CONFIG_TIMER_PRIORITY` they
execute above every user task. Keep them short and do not block in them, or
everything else starves - or lower those priorities so they cannot.

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
(internals)](#the-kernel-heap-internals) for the block layout.

### Diagnostics

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

The kernel ships no default for it, so a build with the check enabled and no
callback is a link error rather than an overflow detector reporting to nobody -
same rule as `os_assert_failed_cb`. Copy the definition from `os_cb_template.c`.

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
status usually means someone made a mistake. `OS_STATUS_NOT_OWNER` from
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

## Platform support

### Supported cores

| Architecture profile | Cortex-M cores | Ahura port | TrustZone support |
|---|---|---|---|
| ARMv6-M | M0, M0+ | `v6m` | No, the Security Extension is absent. `OS_CONFIG_TRUSTZONE_DISABLED` only |
| ARMv7-M / ARMv7E-M | M3 / M4, M7 | `v7m` | No. `OS_CONFIG_TRUSTZONE_DISABLED` only |
| ARMv8-M baseline | M23 | `v6m` | Yes, optional per device. All three `OS_CONFIG_TRUSTZONE` modes |
| ARMv8-M mainline | M33, M35P | `v8m` | Yes, optional per device. All three modes |
| ARMv8.1-M | M52, M55, M85 | `v8m` | Yes, optional per device. All three modes |

A few notes on the table. M4 and M7 are ARMv7E-M with the DSP extension, but
they are port-identical to the M3 here. The M23 is baseline, running a Thumb-1
subset, which is why it shares the `v6m` port rather than the mainline one. The
Cortex-M1 (ARMv6-M, FPGA) is deliberately not supported. "Optional per device"
means the Security Extension is a silicon-vendor choice and may also be disabled
in option bytes, in which case use `OS_CONFIG_TRUSTZONE_DISABLED`.

Every core above is supported on **every silicon vendor**. Nothing in the kernel
or the ports refers to a vendor, a family, or a HAL - the only device-specific
symbol anywhere is CMSIS `SystemCoreClock`, and even that has a documented
one-line fallback (see [Platform clock](#platform-clock)).

### Supported toolchains

| Toolchain | Status |
|---|---|
| **GCC** (`arm-none-eabi-gcc`, 10 and later) | Supported. The primary build and test toolchain |
| **LLVM/Clang** for bare-metal Arm | Supported |
| **Arm Compiler 6** (`armclang`) | Supported |
| Arm Compiler 5 (`armcc`) | **Not** supported - end of life, and no GCC-style inline assembly |
| IAR EWARM (`iccarm`) | **Not** supported yet |

The dividing line is GCC-style inline assembly, which armclang and Clang both
implement and `armcc`/`iccarm` do not. The port layer needs it for the context
switch and the atomics; the entire `core/` tree is ordinary portable C11 and
would build anywhere. Everything else the kernel relies on -
`__attribute__((weak))`, `__builtin_clz`/`__builtin_ctz`, the `__ARM_ARCH_*` and
`__ARM_FP` predefined macros - is common to those three compilers.

An IAR port is a contained piece of work: `os_arch_port_common.h` plus the three
files under `arch/arm/common/`. Nothing in `core/` would change.

### Application callbacks

Application hooks carry the `_cb` suffix. Most are weak, so overriding them is
optional and the kernel's default applies otherwise; a few have no default at all
because a silent one would hide the very thing the hook exists to report, and
those are link errors until the application supplies them. For a clean starting
point, copy `AhuraRTOS/kernel/os_cb_template.c` into the application source tree as
`os_cb.c`, add it to the **application** build (never to the kernel, where the
template is deliberately absent from the CMakeLists), and adapt:

- `os_arch_tick_init_cb` starts the application's own tick timer. **Required**
  when `OS_CONFIG_TICK_SOURCE` is `OS_CONFIG_TICK_SOURCE_EXTERNAL`, and not
  declared at all otherwise. See [The integration
  contract](#the-integration-contract).
- `os_assert_failed_cb` reports a failed assertion. **Required** when
  `OS_CONFIG_ASSERT_ENABLE` is 1, see [Debugging](#debugging).
- `os_stack_overflow_cb` reports a detected overflow. **Required** when
  `OS_CONFIG_STACK_CHECK_ENABLE` is 1, see [Diagnostics](#diagnostics).
- `os_log_output_cb` transmits finished log bytes. **Required** when
  `OS_CONFIG_LOG_ENABLE` is 1: a log with nowhere to go is a link error rather
  than silence.
- `os_tickless_pre_sleep_cb` and `os_tickless_post_sleep_cb` bracket the sleep.
- `os_arch_tz_context_save_cb` and `os_arch_tz_context_restore_cb` handle
  TrustZone secure-context banking, for non-secure kernels only.
- `os_arch_core_id_get_cb` and `os_arch_core_ipi_request_cb` are multi-core SoC
  glue, along with `os_arch_spinlock_acquire_cb` and `os_arch_spinlock_release_cb`
  on ARMv6-M multi-core SoCs, where they are mandatory.

### Platform clock

Everything that needs the CPU frequency, such as the SysTick reload,
`os_delay_us` busy-waits, and tickless accounting, reads it through one arch
function:

```c
uint32_t os_arch_clock_hz_get(void);   /* current CPU clock in Hz */
```

On ARM that is simply the live CMSIS `SystemCoreClock` variable, so the function
lives in the arch layer rather than in portable core code. There is nothing to
configure and nothing to override on a normal CMSIS project: the device's
`SystemInit()` sets the variable and `SystemCoreClockUpdate()` refreshes it after
every clock-tree change, so a board that boots on an internal oscillator and
later switches to a PLL is handled with no kernel involvement.

There is deliberately **no** build-time clock constant. A constant cannot follow
a runtime clock switch, and a stale one would silently mis-program the SysTick
reload and every busy-wait delay, which is a hard class of bug to find.

Devices whose startup code does not define the CMSIS symbol simply define it
themselves, anywhere in the application:

```c
uint32_t SystemCoreClock = 120000000U;   /* keep updated if the clock tree changes */
```

That is also the hook for a platform that keeps its frequency somewhere else,
such as a HAL getter: mirror the value into this variable whenever it changes.
The kernel re-reads it on every use, so dynamic frequency scaling works as long
as the variable stays current.

### TrustZone

`OS_CONFIG_TRUSTZONE` selects which security state the kernel runs in on ARMv8-M
cores (M23, M33, M35P, M52, M55, M85). The build fails with a clear `#error` on
cores without the Security Extension, or when the compile flags do not match the
chosen mode.

- `OS_CONFIG_TRUSTZONE_DISABLED` is the default, and the kernel ignores
  TrustZone. Use it on devices without the Security Extension, or with TrustZone
  disabled in option bytes.
- `OS_CONFIG_TRUSTZONE_SECURE` runs the kernel and every task in the secure
  state. Compile the kernel and the application with `-mcmse`. The context
  switch itself needs nothing extra, because the secure `EXC_RETURN` encoding
  equals the TrustZone-less one and the PSPLIM/MSPLIM guards stay active.
- `OS_CONFIG_TRUSTZONE_NON_SECURE` runs the kernel and its tasks non-secure,
  beside separate secure firmware. Initial task frames use the non-secure
  `EXC_RETURN` (`0xFFFFFFBC`), and the context switch calls two weak callbacks,
  which the application overrides following the `_cb` convention, so the
  secure-side glue can bank per-task secure contexts such as the secure stack
  and `PSP_S`. This mirrors how FreeRTOS's ARM_CM33 secure context management
  works:

  ```c
  void os_arch_tz_context_save_cb(uint32_t task_id);     /* before the switch, outgoing task */
  void os_arch_tz_context_restore_cb(uint32_t task_id);  /* after selection, incoming task   */
  ```

  A `task_id` of 0 is the idle task, which never owns a secure context. Both are
  REQUIRED in this mode - the kernel ships no defaults, so a missing one is a
  link error rather than tasks switching with their secure state left behind. A
  task that never calls secure functions still reaches the callback; it is the
  application's place to return immediately for those ids.

### Multi-core (experimental)

`OS_CONFIG_CORE_COUNT` (default 1, max 31) declares how many cores schedule
tasks. Every scheduling core runs its own PendSV and its own idle task, and
pulls work from the shared ready lists. **Core affinity** selects where each
task may run:

- `os_task_config_t.core_affinity` is a bitmask of allowed cores, where bit n
  means core n. `OS_TASK_CORE_ANY` (0, the default) means any core. Change it at
  runtime with `os_task_core_affinity_set(task, mask)`. A task executing on a
  core the new mask excludes is asked to reschedule, either locally or by IPI.
- When a task becomes ready, through a wake or a start, the kernel preempts
  locally if the task's affinity allows this core. Otherwise it nudges the first
  core in the mask through `os_arch_core_ipi_request_cb`, which the SoC layer
  must supply - without an IPI a woken task waits for that core's next tick.
- Core 0 boots the kernel as usual with `os_init` and `os_start`. Each secondary
  core is booted by the SoC layer, with a vector table pointing at the kernel's
  PendSV and SysTick handlers, then calls `os_core_start()`. That configures the
  banked SHPR, SysTick, DWT, and MSPLIM for that core and enters the scheduler.
  It never returns.
- Core 0 owns the time base. Delays, timers, work queues, and `os_tick_get`
  advance only from core 0's tick, while ticks on other cores drive that core's
  preemption and round-robin. CPU usage through `os_cpu_usage_get` samples core 0.
- The kernel service tasks are placed with `OS_CONFIG_WORK_CORE_AFFINITY` and
  `OS_CONFIG_TIMER_CORE_AFFINITY`, both core-affinity bitmasks where 0 means any
  core, so work handlers and timer callbacks run where the config says.
- Critical sections are the local interrupt mask plus a global kernel spinlock
  with per-core nesting. The spinlock uses `LDREX/STREX` on ARMv7-M and ARMv8-M,
  while ARMv6-M multi-core SoCs such as the RP2040 must provide
  `os_arch_spinlock_acquire_cb` and `os_arch_spinlock_release_cb` backed by
  hardware spinlocks. A missing implementation fails at link time by design.
- The SoC layer supplies `os_arch_core_id_get_cb()`, since Cortex-M has no
  architectural core-id register. It has no default: every core reporting 0
  would leave them sharing one current-task slot.
- A task that is mid-switch-out on another core (its context not yet saved) is
  skipped by this core's pick rather than dispatched from a stale stack pointer.
  That is what `running_core` in the TCB tracks.

There is one constraint worth knowing: a task currently executing on another
core cannot be paused or deleted from this one, and the call returns
`OS_STATUS_BUSY`. Suspend it from its own core first. The SMP paths compile in
the CI matrix but have not run on real multi-core silicon yet, so treat them as
experimental.

### Tickless idle (experimental)

Config options: `OS_CONFIG_TICKLESS_ENABLE` (default 0),
`OS_CONFIG_TICKLESS_MIN_IDLE` (the shortest idle worth sleeping for), and
`OS_CONFIG_MAX_SUPPRESSED_TICKS`.

**Interaction with `OS_CONFIG_TICK_SOURCE`.** Real tick suppression works by
reprogramming the tick timer's reload, which the port can only do for a timer it
owns. With `OS_CONFIG_TICK_SOURCE_EXTERNAL` the timer belongs to the
application, so `os_arch_max_suppressed_ticks_get()` reports 0 and idle degrades
to a plain WFI - correct, and no worse than the v6m/v7m ports do today, but not
power-optimal. Suppressing an application-owned tick means suppressing it in
`os_tickless_pre_sleep_cb()` and reporting the real elapsed time afterwards,
which the current callback pair does not yet express.

The whole group compiles away with the option, like every other feature in PART
2 of `ahura.h`: with `OS_CONFIG_TICKLESS_ENABLE` at 0 the three control
functions are neither declared nor defined, so calling one is a compile error
naming it rather than a call that silently does nothing. Guard your own call
sites the same way the self-test suite does if they must build both ways.

Two application callbacks bracket the sleep window, with prototypes in
`ahura.h`. Both are **mandatory** whenever `OS_CONFIG_TICKLESS_ENABLE` is 1: the
kernel declares them and defines neither, so a missing one is a link error
naming the function rather than a hook that quietly does nothing. Write them in
the application's callback file. Callbacks the application provides carry the
`_cb` suffix by convention:

```c
void os_tickless_pre_sleep_cb(void)   { /* select sleep mode (e.g. SLEEPDEEP), gate clocks */ }
void os_tickless_post_sleep_cb(void)  { /* clear SLEEPDEEP, restore clocks */ }
```

The reason they are required rather than defaulted is the paragraph below: an
empty pre-sleep hook on a part whose HAL runs its own tick source shortens every
suppressed sleep to that source's period, which presents as tickless idle simply
not saving any power.

**What the post-sleep hook may assume.** It runs with the kernel's interrupts
still masked and **before** the sleep has been announced, so `os_tick_get()` is
still short by the entire sleep duration while it executes. Restore hardware
there; do not call anything that blocks, delays, or waits on a tick-driven
timeout, and keep it short, because its whole duration is added to interrupt
latency. The order is deliberate:

| Step | Why it is there and not later |
|---|---|
| Measure the sleep | The counter still holds it, and the normal tick cadence is restored |
| `os_tickless_post_sleep_cb` | Hardware must be back before any kernel work depends on it |
| `os_tick_announce` | Catches up `os_tick_count`, timers, work and task delays in one go |
| Release the interrupt mask | Only now is the kernel's view of time consistent |

Announcing after the mask is released would let a tick or timer run against a
clock hundreds of ticks behind reality; announcing before the hardware is
restored would let the context switch `os_tick_announce` can pend be taken
immediately, leaving the idle task, and the restore, stranded.

Note also that the mask is raised **before** the sleep length is computed. Every
input to that decision - the next timer expiry, the next ready work item, the
earliest sleeping task - is something an ISR can change, and a nearer deadline
registered in that window would be slept straight past. A WFI still wakes on a
pending interrupt while masked, so anything arriving after that point shortens
the sleep rather than being missed.

**Suspend every other periodic interrupt source here too, not just SysTick.**
WFI wakes on any pending interrupt regardless of masking, so anything else
firing more often than the planned sleep will cut every suppressed sleep short
at its own period, no matter how long SysTick itself was reprogrammed for. A
common example is a HAL library's own tick redirected to a spare timer,
precisely so the RTOS can have SysTick to itself. On STM32, `HAL_SuspendTick()`
and `HAL_ResumeTick()` are the standard hook for this. A periodic ADC or comms
timer has the same effect. `os_tickless_pre_sleep_cb` and
`os_tickless_post_sleep_cb` are exactly where to pause and resume those sources.

**Status.** `os_tickless_expected_idle_ticks_get()` already bounds the planned
sleep by the earliest of the next software-timer expiry, the next ready work
item, and the next finite-delay task sleeper, so `os_delay_ms` waiters are
covered and not just timers.

On the ARMv8-M mainline port (`os_arch_port_v8m.c`, covering the M33, M35P, M52,
M55, and M85), `os_arch_sleep_prepare` and `os_arch_elapsed_ticks_get` now
really suppress SysTick for the sleep window. They reprogram its reload one tick
short of the plan, so the real final tick still fires normally and supplies the
last tick's accounting through the ordinary `os_tick_handler` path. This is the
same technique FreeRTOS's tickless idle uses. They also measure the real elapsed
time from SysTick itself rather than the DWT cycle counter, which is not
reliable across an actual sleep on most implementations.

Remaining work:

- The ARMv6-M and ARMv7-M/E-M ports (`os_arch_port_v6m.c`,
  `os_arch_port_v7m.c`) still need the identical fix. Same register layout, not
  yet ported over.
- The idle task still runs a plain `WFI` loop and does not yet call
  `os_tickless_idle_process()`. That function is exposed in `ahura.h` and
  exercised directly by the self-test suite through `test_tickless_sleep()`,
  ahead of the wiring landing.
- A deeper-sleep path, such as STOP mode where SysTick itself stops, would need
  an always-running wake and measurement source instead.
  `OS_CONFIG_LPTIM_CLOCK_HZ` is reserved for that but unused so far.

---

## Testing and examples

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
   error, which is why `os_cb_template.c` guards its copy with:

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
| `test_stress_work_flood` | Registry oversubscribed 2:1: exactly `OS_CONFIG_MAX_WORKS` submissions accepted, every one past capacity refused with `FULL`, and every accepted one runs exactly once - then 20 more churn rounds |
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
| `os_main_delay.c` | `os_delay_ms`, `os_delay_us`, `os_delay_s` |
| `os_main_critical.c` | Critical sections protecting a shared counter |
| `os_main_kernel_lock.c` | Deferring preemption with `os_kernel_lock` while interrupts keep running |
| `os_main_mutex.c` | Mutual exclusion with `os_mutex_*` |
| `os_main_semaphore.c` | Counting semaphore, producer and consumer |
| `os_main_queue.c` | Message queue, producer and consumer, both static (`OS_QUEUE_DEFINE_STATIC`) and dynamic (`os_queue_init_dynamic`) storage |
| `os_main_event.c` | Event, waiting on multiple bits |
| `os_main_notify.c` | Task notifications with `os_notify_*` |
| `os_main_timer.c` | One-shot and periodic software timers |
| `os_main_work.c` | Deferrable work queue |
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

## Internals

### Source layout

#### Top-level files

- `ahura.h` is the public umbrella API, and the only header applications
  include. It reads in two parts: **PART 1** is everything no option can remove
  (types, tasks, time, critical sections, the intrusive list), so anything found
  there can be used unconditionally; **PART 2** is one group per `OS_CONFIG_`
  option, each behind a single guard covering its types, macros and functions
  together, in the same order as `os_config.h`. It declares `os_main()` and
  `os_test()` as well, even though the kernel defines neither: supplying them is
  the application's job through its `os_main.c`, or the test library's. Neither
  carries the `_cb` suffix used elsewhere in this header. That suffix is
  reserved for callbacks the kernel queries for platform behavior, such as
  `os_tickless_pre_sleep_cb`, whereas `os_main()` and `os_test()` are where the
  application's or suite's own code runs.
- `os_config_template.h` is the template for the application's `os_config.h`. It
  lists every build-time option at its default value - see
  [Configuration](#configuration) for the full table. This file is never
  included by the kernel, so copy it into the project. Disabling a feature
  compiles out its code and API, and disabling timer, work, the default task, or
  the self-test task also removes the corresponding kernel service task and its
  stack.
- `os_cb_template.c` is the template for the application-side callbacks, and is
  deliberately not compiled into the kernel. See [Application
  callbacks](#application-callbacks).
- `os_main_template.c` is the template for the default application task's body,
  also deliberately not compiled into the kernel. See [Default application
  task](#default-application-task).
- `test/` holds the kernel self-test suite (`os_test.c`), its own buildable
  module with the target `os_test` and its own `CMakeLists.txt`. See [Self-test
  suite](#self-test-suite).

#### `core/` portable kernel modules

All filenames are `os_`-prefixed:

- `os_kernel.c` covers the lifecycle (`os_init`, `os_start`, the running flag),
  the scheduler lock, the default application task (`os_main`, see
  [Default application task](#default-application-task)), and the self-test task
  (`os_test`, see [Self-test suite](#self-test-suite)).
- `os_mem.c` is the kernel heap (`os_mem_alloc` and `os_mem_free`), a first-fit
  allocator with coalescing over a static `OS_CONFIG_HEAP_SIZE` heap.
- `os_task.c` holds the static TCB pool and O(1) list-based scheduling: one FIFO
  ready list per priority plus a ready bitmap, where the highest set bit is the
  next priority to run and costs a single `CLZ` on ARMv7-M and up, round-robin
  by list rotation, and a delay list holding only the finite-delay sleepers.
  This is also where the scheduler lock's effect on dispatch and mutex priority
  inheritance's effective-priority changes live, since both are entirely about
  the TCB and the ready lists rather than about separate kernel objects. See
  [The scheduler](#the-scheduler).
- `os_notify.c` is direct-to-task notifications (`os_notify_give`,
  `os_notify_wait`). The one-word mailbox itself sits in the TCB, because it
  belongs to the task rather than to any object, so `os_task.c` hands out the
  slot and this module owns everything about what a notification means.
- `os_tick.c` is the tick counter and tick handler, which wakes delays, drives
  timers, and preempts. See [The tick](#the-tick).
- `os_delay.c` provides blocking millisecond and second delays plus a
  cycle-counter-precise microsecond busy-wait.
- `os_critical.c` implements the nesting critical section over the port's kernel
  interrupt mask - PRIMASK, or BASEPRI when
  `OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY` is nonzero. See [The three
  barriers](#the-three-barriers).
- `os_mutex.c`, `os_semaphore.c`, `os_queue.c`, and `os_event.c` are the sync
  and IPC primitives with `timeout_ms` waits, all built on the shared
  wait/wake machinery in `os_task.c`. See [Blocking and
  waking](#blocking-and-waking).
- `os_timer.c` holds the software timers. Expiry is detected by the tick and
  callbacks run on the kernel timer task `tsk_timer`, at
  `OS_CONFIG_TIMER_PRIORITY`.
- `os_work.c` is the deferrable work queue in the style of Zephyr. Items run on
  the kernel work task `tsk_work`, at `OS_CONFIG_WORK_PRIORITY`.
- `os_log.c` is the buffered log: a static ring buffer drained by `tsk_log` into
  `os_log_output_cb`.
- `os_atomic.c` is the argument-validating wrapper layer over the port's inline
  atomic implementations. See [Atomics](#atomics).
- `os_list.c` is the intrusive doubly-linked list. It is always compiled, since
  the scheduler itself runs on it and it cannot be configured out, and it is
  also public API.
- `os_internal.h` is the internal cross-module contract, not for applications.

#### `arch/arm/` port layer

This layer covers the tick source, the PendSV context switch (including the
first-task start), initial stack frames, the cycle counter, the interrupt mask,
and the atomics. Shared code is organized by architecture, the same split Zephyr
and CMSIS-RTX use: one v6m implementation, one v7m implementation, one v8m
implementation, with thin per-core wrapper folders on top.

`os_arch_port_common.h` is more than a header of declarations: everything small
and hot enough that a cross-unit call would dominate it lives there as an inline
definition rather than in a port `.c`. That covers the interrupt mask, the
in-ISR and core-id checks, the spinlock, the boot-time vector check, and the
whole atomic set (see [Atomics](#atomics)). The port `.c` files are left with
what genuinely differs per architecture and is too large to inline.

The layer defines exactly **one** externally visible vector handler,
`PendSV_Handler` (renameable, see [The integration
contract](#the-integration-contract)). It does not define `SVC_Handler` - the
first task is started through PendSV's `PSP == 0` path, so SVC stays free for
the application - and it does not define `SysTick_Handler`, which the
application routes to `os_tick_handler()`.

- `common/os_arch_port_v7m.c` is the ARMv7-M (M3) and ARMv7E-M (M4, M7)
  implementation. It is Thumb-2 and FPU-aware, saving `s16-s31` and a per-task
  `EXC_RETURN` when built with a hard or softfp float ABI.
- `common/os_arch_port_v8m.c` is the ARMv8-M mainline (M33, M35P) and ARMv8.1-M
  (M52, M55, M85) implementation. It is a superset of the v7m port that always
  saves and restores `PSPLIM` per task and programs `MSPLIM` for the handler
  stack when the linker script provides the stack-bottom symbol, so a stack
  overflow raises a UsageFault instead of silently corrupting memory. TrustZone,
  in all three `OS_CONFIG_TRUSTZONE` modes, lives here.
- `common/os_arch_port_v6m.c` is the ARMv6-M (M0, M0+) and ARMv8-M baseline
  (M23) implementation. It uses the Thumb-1 subset and has no FPU, and the cycle
  counter is synthesized from SysTick because these cores have no DWT CYCCNT.
  Baseline does not belong in the v8m file because it cannot execute the
  mainline Thumb-2 ISA, so its TrustZone support is handled here. Non-secure
  v8-M baseline has no `PSPLIM`, so there is no stack-limit handling.
- `common/os_arch_cycle_systick.c` is the SysTick-derived cycle counter, shared
  by all three ports. It is the only counter ARMv6-M has, and it is what the
  mainline ports fall back to when DWT CYCCNT turns out to be unavailable - the
  unit is architecturally optional, may report `DWT_CTRL.NOCYCCNT`, and on
  several devices sits behind a debug power domain that refuses the enable until
  a debugger attaches. `os_arch_init()` checks all three cases and picks once.
  Without that fallback, `os_delay_us()` on such a part would spin forever.
- The four files under `common/` are **textual includes**, pulled in by the
  per-core wrappers below. Never add them to a build as separate compilation
  units. Each carries a `#error` guard against being compiled for the wrong
  architecture profile.
- `cortex_m0/`, `cortex_m0plus/`, and `cortex_m23/` are thin wrappers over the
  v6m port.
- `cortex_m3/`, `cortex_m4/`, and `cortex_m7/` are thin wrappers over the v7m
  port. The M7 additionally relies on the DWT LAR unlock done in `os_arch_init`.
- `cortex_m33/`, `cortex_m35p/`, `cortex_m52/`, `cortex_m55/`, and
  `cortex_m85/` are thin wrappers over the v8m port. On the v8.1-M cores,
  Helium/MVE state is covered by the existing s16-s31 save plus hardware lazy
  stacking of s0-s15, FPSCR, and VPR. The folder names follow GCC's `-mcpu`
  spelling, so it is `cortex_m0plus` because the core is the M0"plus", but
  `cortex_m35p` because that core's "P" means physical security rather than plus
  (`-mcpu=cortex-m35p`).
- The build selects the variant from `-mcpu`, falling back to `-march`, so
  `armv8.1-m.main` maps to `cortex_m55` and so on. All folders of one profile
  include the same shared port, so any core of the right architecture is
  equivalent. See `AhuraRTOS/kernel/CMakeLists.txt`. Override the choice with
  `-DOS_ARCH_VARIANT=cortex_m4`. Note that GCC learned `-mcpu=cortex-m52` in GCC
  14, so older toolchains build that core with `-march=armv8.1-m.main+mve.fp`,
  which the fallback resolves automatically.
- The `MSPLIM` guard is active when the linker script provides the bottom of the
  main stack as `__StackLimit`, used by CMSIS-style scripts, or `_sstack`, used
  by several vendor-generated ones. Both are weak references, so either naming
  works unmodified, and when neither symbol exists the guard is skipped.
- TrustZone, the ARMv8-M Security Extension, is selected with
  `OS_CONFIG_TRUSTZONE`. See [TrustZone](#trustzone).
- Not covered yet: PAC/BTI (`-mbranch-protection` on the M85).

### Notes and constraints

- The kernel owns exactly one exception vector, PendSV, and needs exactly one
  call from the application, `os_tick_handler()`. `SVC` and `SysTick` are not
  claimed. See [The integration contract](#the-integration-contract).
- Do not block, whether by delaying or locking with a timeout, inside a critical
  section, a scheduler-locked region, or an ISR. Under a scheduler lock the
  kernel enforces it: blocking calls degrade to non-blocking rather than parking
  a task it cannot switch away from. See [Scheduler lock](#scheduler-lock).
- The kernel's service tasks (`tsk_timer`, `tsk_work`, `tsk_log`) cannot be
  paused or deleted by the application; both calls return `OS_STATUS_BUSY`.
- Work handlers and timer callbacks run on the highest-priority kernel tasks, so
  keep them short and non-blocking or user tasks will starve.
- Timers run in two modes, `OS_TIMER_MODE_ONE_SHOT` which fires once then stops,
  and `OS_TIMER_MODE_PERIODIC` which reloads every period. Select the mode in
  `os_timer_init`.
- Mutexes are task-only and non-recursive. See [Mutexes and priority
  inheritance](#mutexes-and-priority-inheritance).
- Mutex priority inheritance is single-level: it does not propagate through a
  chain of nested mutexes held by different tasks.
- With a nonzero `OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY`, ISRs above that priority
  must not call any kernel API - they are never masked, so the kernel cannot
  protect its own state against them.
- The project builds with the hard-float ABI, and the port saves and restores
  the FPU context (`s16-s31` plus a per-task `EXC_RETURN`) automatically.

## License

MIT - every source file carries `SPDX-License-Identifier: MIT` in its header.
See the
[project LICENSE](../LICENSE).
