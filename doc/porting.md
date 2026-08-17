# Platform support

[← Back to the documentation index](README.md) · [Kernel reference](kernel.md)

What the kernel asks of a platform: the callbacks it needs, the clock, TrustZone,
and the experimental multi-core and tickless paths. For which cores and
toolchains are supported see [Platforms](platforms.md).


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
point, copy `AhuraRTOS/kernel/template/os_cb.c` into the application source tree as
`os_cb.c`, add it to the **application** build (never to the kernel, where the
template is deliberately absent from the CMakeLists), and adapt:

- `os_arch_tick_init_cb` starts the application's own tick timer. **Required**
  when `OS_CONFIG_TICK_SOURCE` is `OS_CONFIG_TICK_SOURCE_EXTERNAL`, and not
  declared at all otherwise. See [The integration
  contract](integration.md#the-integration-contract).
- `os_assert_failed_cb` reports a failed assertion. **Required** when
  `OS_CONFIG_ASSERT_ENABLE` is 1, see [Debugging](api.md#debugging).
- `os_stack_overflow_cb` reports a detected overflow. **Required** when
  `OS_CONFIG_STACK_CHECK_ENABLE` is 1, see [Diagnostics](api.md#diagnostics).
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
- Core 0 owns the time base. Delays, timers, deferred calls, and `os_tick_get`
  advance only from core 0's tick, while ticks on other cores drive that core's
  preemption and round-robin. CPU usage through `os_cpu_usage_get` samples core 0.
- The kernel service tasks are placed with `OS_CONFIG_TIMER_CORE_AFFINITY` and
  `OS_CONFIG_LOG_CORE_AFFINITY`, core-affinity bitmasks where 0 means any core,
  so timer callbacks and deferred calls run where the config says.
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
`OS_ERR_BUSY`. Suspend it from its own core first. The SMP paths compile in
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
