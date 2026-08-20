# Getting the kernel into a project

[← Back to the documentation index](README.md) · [Kernel reference](kernel.md)

Getting the kernel into a build, what it needs from the platform, and every
`os_config.h` option. For the step-by-step manual install see
[Installation](installation.md).


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
   | [`template/os_config.h`](../kernel/template/os_config.h) | `os_config.h` | nothing (it is a header) |
   | [`template/os_cb.c`](../kernel/template/os_cb.c) | `os_cb.c` | the **application** build |
   | [`template/os_main.c`](../kernel/template/os_main.c) | `os_main.c` | the **application** build |

   The kernel deliberately compiles none of the three. `os_config.h` is the
   application's configuration, and `os_cb.c` / `os_main.c` hold application
   code - see [Configuration](#configuration) and
   [Application callbacks](porting.md#application-callbacks).

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

Not sure it worked? The [self-test suite](testing.md#self-test-suite) validates the whole
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
core/os_kernel.c      core/os_notify.c
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
has a minimal, standalone example per feature. See [Examples](testing.md#examples).

### Configuration

Projects never edit kernel files, and the kernel ships no editable configuration
of its own. The application owns the one and only config file, following the
same model as `FreeRTOSConfig.h`:

1. Copy `AhuraRTOS/kernel/template/os_config.h` into the project as `os_config.h`.
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
| `OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY` | `0U` | `0` = mask everything with PRIMASK; nonzero = BASEPRI, see [The three barriers](design.md#the-three-barriers) |
| **PART 2 - features** | | |
| `OS_CONFIG_MUTEX_ENABLE` | `1U` | Mutexes, always with single-level priority inheritance |
| `OS_CONFIG_SEMAPHORE_ENABLE` | `1U` | Counting semaphores |
| `OS_CONFIG_QUEUE_ENABLE` | `1U` | Fixed-item-size queues |
| `OS_CONFIG_EVENT_ENABLE` | `1U` | Event bit groups |
| `OS_CONFIG_TIMER_ENABLE` + `MAX_TIMERS` `8U`, `TIMER_STACK_SIZE` `512U`, `TIMER_PRIORITY`, `TIMER_CORE_AFFINITY` | `1U` | Software timers and the `tsk_timer` service task |
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
| `OS_CONFIG_CORE_COUNT` + `SPINLOCK_SOC_BACKEND` | `1U` | SMP scheduling (verified on RP2350; RP2040 path not yet on silicon) |
| `OS_CONFIG_TICKLESS_ENABLE` + `TICKLESS_MIN_IDLE`, `MAX_SUPPRESSED_TICKS` | `0U` | Tick suppression while idle (experimental) |

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
[Boot sequence](design.md#boot-sequence).

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
