<div align="center">

# ⚡ AhuraRTOS

**A small, portable, preemptive RTOS for microcontrollers.**

One public header · no editable kernel files · every feature a compile-time switch

![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)
![Standard: C11](https://img.shields.io/badge/standard-C11-blue.svg)
![Architectures: Cortex-M | RISC-V](https://img.shields.io/badge/arch-Cortex--M%20%7C%20RISC--V-informational.svg)
![Toolchains: GCC | Clang | armclang](https://img.shields.io/badge/toolchains-GCC%20%7C%20Clang%20%7C%20armclang-informational.svg)
![Status: under development](https://img.shields.io/badge/status-under%20development-orange.svg)

**[Why](#why-ahurartos)** ·
**[Features](#what-you-get)** ·
**[Verified](#verified-on-hardware)** ·
**[Install](#install-it)** ·
**[Documentation](#documentation)**

</div>

> [!WARNING]
> **Early and under active development.** The kernel is functional and
> self-testing on every board in the table below, but **APIs may still change**
> and it is not yet recommended for production use.

## Why AhuraRTOS

**It costs one exception vector.** The kernel takes over the lowest-priority
exception - PendSV on Cortex-M, the machine software interrupt on RISC-V - and
nothing else. `SVC` is left entirely to the application, which keeps it
compatible with everything that legitimately wants it: Nordic's SoftDevice,
TF-M and other secure firmware, vendor bootloaders and ROM APIs. The tick is a
single application call to `os_tick_handler()`, and its timer is configurable,
so parts whose SysTick stops in low-power modes are first-class rather than
special cases. No HAL, no CMSIS dependency, no linker-script edits.

**Misintegration fails loudly.** The kernel checks at boot that the vector table
really routes the switch to it, and traps at the cause if not. The usual
alternative is a board that reaches `os_start()` and stops dead - no fault, no
output, nothing to attach a debugger to. The same principle runs through the
API: missing callbacks are link errors naming the function, an incomplete
`os_config.h` is a compile error, and a port/`-mcpu` mismatch fails to compile
rather than producing a subtly wrong context switch.

**You never edit a kernel file.** One template becomes your `os_config.h`, and
that is the single source of configuration for both the kernel library and your
application. Updating the kernel is a drop-in replacement of `AhuraRTOS/`, every
time - there are no local modifications to re-apply, because there are none to
make.

**Every feature is a compile-time switch.** `OS_CONFIG_<FEATURE>_ENABLE` removes
code, RAM *and* API surface - not just runtime behavior. And nothing in the
kernel allocates dynamically: task blocks, ready lists, timer objects, the log
ring and the optional heap are all static, so what a build costs is decided at
compile time and visible in the map file.

**The scheduler is O(1), and so is deciding not to run it.** One FIFO ready list
per priority plus a 32-bit ready bitmap: the next task is a `CLZ` and a list
head, whatever the task count. A tick that would not switch anything costs one
bitmap check instead of a full context-switch round trip.

**Barriers that fit what you are actually guarding.** A scheduler lock that
defers preemption *without masking a single interrupt* - the barrier a critical
section cannot be. Critical sections that can mask with `BASEPRI` instead of
`PRIMASK`, leaving urgent ISRs with zero kernel-induced latency. And a full
atomic set that is lock-free wherever the instruction set allows it.

**Priority inheritance is always on.** No switch to turn it off while still
calling the object a mutex, and the accounting stays correct when one task holds
several contended mutexes at once. What inheritance cannot do is prevent a
*deadlock* - that is a lock-ordering fault, not a timing one - so development
builds detect that instead: blocking on a mutex whose wait chain leads back to
you asserts the moment the cycle would form, while the guilty call stack is
still there to read, rather than leaving a board that silently stops.

**It proves itself on your board.** A built-in self-test suite exercises every
enabled feature over `printf`, with no application code and no board support,
and finishes with a cycle-accurate benchmark table for every hot kernel path.
Bring-up is: flash it, read the console, then start writing firmware.

**Two instruction sets, one kernel.** M0 through M85 - ARMv6-M to ARMv8.1-M -
across three shared port implementations, plus RV32 on Hazard3. Nothing in the
kernel names a vendor, a family, or a HAL, and adding an architecture is a new
port rather than a kernel change.

## What you get

| | |
|---|---|
| **Scheduling** | Preemptive, 31 priority levels, one FIFO ready list per priority, O(1) pick, configurable round-robin |
| **Sync & IPC** | Mutexes with priority inheritance · counting semaphores · queues, static or heap-backed · variable-length message buffers · event groups · per-task notifications - all with millisecond timeouts |
| **Timers** | One-shot and periodic software timers, plus caller-owned deferred-call pools, all delivered on one service task so callbacks run in task context |
| **Memory** | An optional first-fit kernel heap with coalescing. Everything else is static |
| **Diagnostics** | Stack watermarking · stack-overflow detection · mutex deadlock detection · CPU-load sampling · buffered logging that never stalls the caller |
| **Multi-core** | SMP scheduling with a per-task core-affinity mask over shared ready lists |
| **Security** | TrustZone on ARMv8-M: secure, non-secure or disabled, per build |

A complete application, in full:

```c
int main(void)
{
    SystemClock_Config();

    os_init();      /* idle task, service tasks, your default task, the tick */
    os_start();     /* never returns */
}

void os_main(void)  /* your application - already a running task */
{
    while (1)
    {
        my_led_toggle();
        os_delay_ms(500U);
    }
}
```

`os_init()` creates and starts a default task for you, so there is nothing to
declare just to get moving. Every feature above is described with the mechanism
behind it in the **[kernel reference](doc/kernel.md)**.

## Verified on hardware

The self-test suite is not a CI badge - it runs on the board. Every row below is
a real part that has run the full suite to completion:

| Board | Cores | Self-test | Dual-core SMP |
|---|---|---|---|
| **Raspberry Pi Pico 2** | 2 × Cortex-M33 (RP2350) | ✅ | ✅ |
| **Raspberry Pi Pico 2** | 2 × Hazard3 RV32 (RP2350) | ✅ | ✅ |
| **Raspberry Pi Pico** | 2 × Cortex-M0+ (RP2040) | ✅ | ✅ |
| **NUCLEO-H503RB** | Cortex-M33 (STM32H5) | ✅ | single-core part |

Not yet run on silicon: **TrustZone** (builds, callbacks wired, never exercised
on a part with the Security Extension) and **tickless idle** (implemented on the
ARMv8-M port, not yet wired into the idle task). Both are listed in the
[roadmap](doc/roadmap.md) rather than claimed here.

## Install it

On a **Raspberry Pi Pico SDK** or **STM32CubeMX** project it is one command,
which prints the exact diff it wants to apply and waits for a `y` before
touching anything. Every installer has an offline twin for machines with no
internet, and the same integration by hand is six steps on any other vendor,
IDE or build system.

**→ [Installing AhuraRTOS](doc/installation.md#pick-your-chip)** is the table
that names all three routes for every packaged chip.

## Documentation

**[📖 Documentation index](doc/README.md)** - or go straight to a page:

| Page | What is in it |
|---|---|
| **Start here** | |
| [Installing AhuraRTOS](doc/installation.md) | The pick-your-chip table - all three routes per chip - then the general procedure: six steps, any vendor, IDE and build system |
| [AhuraRTOS on STM32](doc/stm32.md) | **Everything STM32 in one page.** Three install routes - [automatic](doc/stm32.md#automatic---one-command), [offline](doc/stm32.md#offline---no-internet-on-the-machine), [manual](doc/stm32.md#manual---step-by-step) with the exact CubeMX checkboxes - then [the `st/stm32` package](doc/stm32.md#the-soc-package) |
| [AhuraRTOS on Raspberry Pi](doc/raspberry-pi.md) | **Everything RP2040 / RP2350 / RP2354 in one page.** Three install routes - [automatic](doc/raspberry-pi.md#automatic---one-command), [offline](doc/raspberry-pi.md#offline---no-internet-on-the-machine), [manual](doc/raspberry-pi.md#manual---step-by-step) - then [the three packages](doc/raspberry-pi.md#the-packages-chip-by-chip), Arm and RISC-V |
| [Vendor notes](doc/vendor-notes.md) | The one thing that differs per vendor - whether its code generator emits a competing `PendSV_Handler` - on STM32, Nordic, NXP, TI, Silicon Labs, Renesas, Microchip, Infineon, GD32 and anything else |
| [Running the self-test suite](doc/self-test.md) | Prove a fresh port before writing anything on top of it: how to enable it, how to read the output, why a silent console is usually the libc, and how much flash to budget |
| **The kernel** | |
| [Kernel reference](doc/kernel.md) | **The authoritative reference.** What the kernel is, and the entry point to the five pages below |
| [Using the kernel](doc/api.md) | Every API: tasks, priorities, mutexes, queues, message buffers, notifications, atomics, timers, deferred calls, the heap, diagnostics and debugging |
| [How the kernel works](doc/design.md) | Boot, the scheduler, the context switch, the tick, blocking and waking, priority inheritance, and where the RAM goes |
| [What the kernel needs from a platform](doc/integration.md) | The reference behind the install: non-CMake build inputs, every `os_config.h` option, and the two-item integration contract |
| [Platform support](doc/porting.md) | The callbacks a platform must supply, the clock, TrustZone, multi-core and tickless idle |
| [Source layout](doc/source.md) | What each directory and each `kernel/` source file is, and why `os_internal.h` is reachable from none of them |
| [SoC packages](doc/soc.md) | The optional per-silicon layer under `soc/`: who owns which callback, what may and may not live there, and how to write one. The four packaged parts are documented in the two vendor pages above |
| **Testing and examples** | |
| [Testing and examples](doc/testing.md) | The self-test suite and the runnable examples, together |
| [Examples](doc/examples.md) | One runnable `os_main.c` per kernel feature, and how to run them |
| **The project** | |
| [Platforms](doc/platforms.md) | Which cores, vendors and toolchains are supported today, what is planned, and what is experimental |
| [Roadmap](doc/roadmap.md) | The three phases, the known gaps tracked for later, and the current status |
| [C style](CSTYLE.md) | The style every file in this repository is written to |

## Repository layout

```text
AhuraRTOS/
├── CMakeLists.txt  <- builds the ahura_kernel library; the application calls
│                      add_subdirectory(AhuraRTOS) and links ahura_kernel
├── ahura.h         <- the single public header; applications include only this
├── kernel/         <- the portable core: scheduler, sync and IPC, timers, memory,
│                      log. Plain C11, no CPU knowledge anywhere in it
├── arch/           <- the port layer, <family>/<core>: arm/ and riscv/ today.
│                      Everything a CPU changes is confined here
├── soc/            <- optional per-silicon packages, <vendor>/<family> (see doc/soc.md)
├── template/       <- the files you copy into your project: os_config.h, os_cb.c,
│                      os_main.c, plus soc_cb.c when no SoC package covers the part
├── test/           <- the self-test suite, built as its own os_test library
├── examples/       <- one runnable main per feature
├── doc/            <- every documentation page, including the per-SoC ones
├── tools/          <- one-command installers. The four install_*.py are small
│                      bootstraps: each locates an AhuraRTOS checkout - downloading
│                      one if it has to - then loads _ahura_install.py (the engine,
│                      shared by every platform) and _platforms/<vendor>.py from it.
│                      Adding a platform is one file in _platforms/
├── CSTYLE.md       <- the C style every file here is written to
├── LICENSE
└── README.md       <- this file
```

The three axes the layout separates are the three questions a port asks:
`kernel/` never changes, `arch/` changes with the instruction set, `soc/`
changes with the chip.

One repository, no submodules - a plain clone gives you everything:

```bash
git clone https://github.com/AhuraRTOS/AhuraRTOS.git
```

## Contributing

Contributions are welcome. Kernel work, new ports, testing, and documentation
all help. Open an issue or submit a pull request.

## License

GNU General Public License v3.0 or later. See [LICENSE](LICENSE).
