# AhuraRTOS

A small preemptive real-time operating system for microcontrollers, built around
an architecture-independent core, a single public header, and an explicit
boundary between the application and the kernel.

There are no editable kernel files and no hidden configuration. **Everything a
CPU changes** - the context switch, the tick, critical sections, atomics,
low-power entry - **is confined to a small port layer**, and the rest of the
kernel is ordinary portable C. ARM Cortex-M is the first architecture ported and
the one that works today; others follow on the same interface, without touching
the core.

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

> **Status:** early and under active development. The kernel is functional and
> self-testing across the Cortex-M range, but APIs may still change, and no
> other architecture is ported yet. Not yet recommended for production use.

---

## Why AhuraRTOS

**It costs one exception vector.** The kernel takes over PendSV and nothing
else. `SVC` is left entirely to the application, which keeps it compatible with
everything that legitimately wants it - Nordic's SoftDevice, TF-M and other
secure firmware, vendor bootloaders and ROM APIs. The tick is a single
application call to `os_tick_handler()`, and its timer is configurable, so parts
whose SysTick stops in low-power modes are first-class rather than special
cases. No HAL, no CMSIS dependency, no linker-script edits.

**Misintegration fails loudly.** The kernel checks at boot that the vector table
really routes PendSV to it, and traps at the cause if not. The usual alternative
is a board that reaches `os_start()` and stops dead - no fault, no output,
nothing to attach a debugger to. The same principle runs through the API:
missing callbacks are link errors naming the function, an incomplete
`os_config.h` is a compile error, and a port/`-mcpu` mismatch fails to compile
rather than producing a subtly wrong context switch.

**You never edit a kernel file.** One template becomes your `os_config.h`, and
that is the single source of configuration for both the kernel library and your
application. Updating the kernel is a drop-in replacement of `kernel/`, every
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
you asserts the moment the cycle would form, while the guilty call stack is still
there to read, rather than leaving a board that silently stops.

**It proves itself on your board.** A built-in self-test suite exercises every
enabled feature over `printf`, with no application code and no board support,
and finishes with a cycle-accurate benchmark table for every hot kernel path.
Bring-up is: flash it, read the console, then start writing firmware.

**One kernel, every Cortex-M, every vendor.** M0 through M85 - ARMv6-M to
ARMv8.1-M - across just three shared port implementations, with TrustZone
support on ARMv8-M. Nothing in the kernel names a vendor, a family, or a HAL.

## What you get

Preemptive priority scheduling with 31 levels and configurable round-robin ·
mutexes with priority inheritance, counting semaphores, queues, events and
per-task notifications, all with millisecond timeouts · one-shot and periodic
software timers plus caller-owned deferred-call pools, all delivered on one
service task so callbacks run in task context · an optional first-fit kernel heap with
coalescing · stack watermarking, stack-overflow detection, mutex deadlock
detection and CPU-load sampling · buffered logging that never stalls the caller ·
multi-core (SMP) scheduling verified on the RP2350 · experimental tickless idle.

Every one of these is described in full, with the mechanism behind it, in the
[kernel reference](doc/kernel.md).

## Documentation

**[📖 Documentation index](doc/README.md)**

| Getting it running | |
|---|---|
| **[Installation](doc/installation.md)** | **Start here.** The manual install with CMake: six steps, any vendor, IDE and build system. Get the source, copy three files, add them to the build, route the tick, check `PendSV`, boot |
| **[STM32CubeMX / CubeIDE](doc/stm32cubemx.md)** | On ST tooling, two ways: **[automatic](doc/stm32cubemx.md#automatic---one-command)** - one command does the lot - or **[manual](doc/stm32cubemx.md#manual---step-by-step)**, checkbox by checkbox on real hardware |
| [Vendor notes](doc/vendor-notes.md) | The one thing that differs per vendor, and what to do about it |
| [Self-test suite](doc/self-test.md) | Prove a fresh port before writing anything on top of it |

| The project | |
|---|---|
| [Platforms](doc/platforms.md) | Cores, vendors and toolchains: supported, planned, experimental |
| [Roadmap](doc/roadmap.md) | The three phases, the known gaps, the current status |

| The kernel | |
|---|---|
| [Kernel reference](doc/kernel.md) | **The authoritative reference:** how the kernel works inside, every API, every configuration option |
| [Examples](examples/README.md) | One runnable `os_main.c` per feature, and how to run them |

## Repository layout

```text
AhuraRTOS/
├── doc/        <- the documentation above
├── kernel/     <- the kernel: core, arch ports, templates, self-test suite
│   └── soc/    <- optional per-silicon packages, <vendor>/<family> (see doc/soc.md)
├── examples/   <- one runnable main per feature
├── tools/      <- one-command installers: install_stm32_online.py (CubeMX) and
│               install_rpi_online.py (Pico SDK), each with an _offline twin
│               for machines with no internet, over a shared install_common.py
├── CSTYLE.md   <- the C style every file here is written to
├── LICENSE
└── README.md   <- this file
```

One repository, no submodules - a plain clone gives you everything:

```bash
git clone https://github.com/AhuraRTOS/AhuraRTOS.git
cd AhuraRTOS
```

To put the kernel in your own project, see
**[Installation](doc/installation.md)**.

## Contributing

Contributions are welcome. Kernel work, new ports, testing, and documentation
all help. Open an issue or submit a pull request.

## License

GNU General Public License v3.0 or later. See [LICENSE](LICENSE).
