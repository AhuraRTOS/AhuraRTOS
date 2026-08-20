# Ahura Kernel

[← Documentation index](README.md)

![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)
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

> **This page is the entry point to the kernel reference.**
> For step-by-step installation - vendor by vendor, IDE by IDE - see the
> [documentation index](README.md).
> [Getting the kernel into a project](integration.md) is the short version of it.

---

## What the kernel is

- **Preemptive priority scheduler.** 31 priority levels, O(1) list-based ready
  queues, and round-robin among tasks of equal priority.
- **Full sync/IPC set.** Mutexes (always with single-level priority
  inheritance), counting semaphores, queues, events, and lightweight
  per-task notifications, all with millisecond timeouts.
- **Software timers and deferred calls.** One-shot and periodic timers, plus
  a one-shot timer for running a function later - both
  on one kernel service task.
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
- **Experimental:** tickless idle. Multi-core (SMP) scheduling is verified on
  the RP2350's dual Cortex-M33; the RP2040's ARMv6-M path has not run on
  silicon.

**No dynamic allocation anywhere in the kernel itself.** Task control blocks,
ready lists, timer objects, the log ring and the optional heap are all static.
What a build costs in RAM is decided at compile time and visible in the map
file.

---

## The rest of the reference

This page was one long document; it is now five, so each one can be read on its
own.

| Page | What is in it |
|---|---|
| **[Getting the kernel into a project](integration.md)** | Quick start, adding it to a build, every `os_config.h` option, and the integration contract |
| **[Using the kernel](api.md)** | Every API: tasks, priorities, mutexes, queues, notifications, atomics, timers, deferred calls, the heap, diagnostics and debugging |
| **[How the kernel works](design.md)** | Boot, the scheduler, the context switch, the tick, blocking and waking, priority inheritance, where the RAM goes, and the source layout |
| **[Platform support](porting.md)** | The callbacks a platform must supply, the clock, TrustZone, multi-core and tickless idle |
| **[Testing and examples](testing.md)** | The self-test suite and the runnable examples |

Related pages outside this reference: **[Installation](installation.md)**,
**[Platforms](platforms.md)**, **[Self-test suite](self-test.md)**,
**[Vendor notes](vendor-notes.md)**, **[Roadmap](roadmap.md)**.

## License

GNU GPL v3 (or later) - every source file carries `SPDX-License-Identifier: GPL-3.0-or-later`
in its header.
See the
[project LICENSE](../LICENSE).
