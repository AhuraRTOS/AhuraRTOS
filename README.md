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
kernel allocates dynamically: task blocks, ready lists, timer and work
registries, the log ring and the optional heap are all static arrays, so what a
build costs is decided at compile time and visible in the map file.

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
several contended mutexes at once.

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
software timers and a deferrable work queue, each on its own service task so
callbacks run in task context · an optional first-fit kernel heap with
coalescing · stack watermarking, stack-overflow detection and CPU-load sampling
· buffered logging that never stalls the caller · experimental multi-core
scheduling and tickless idle.

Every one of these is described in full, with the mechanism behind it, in the
[kernel reference](doc/kernel.md).

## Install

On an **STM32CubeMX project generated with the CMake toolchain**, one command
from the project root - the directory holding `CMakeLists.txt` and the `.ioc` -
does the whole integration:

**Windows** (PowerShell):

```powershell
irm https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_stm32.py | python -
```

**Linux and macOS** (bash, zsh):

```bash
curl -fsSL https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_stm32.py | python3 -
```

It prints the exact diff first and asks before writing anything; add
`--dry-run` to stop after the diff, or `--yes` to skip the question. Nothing is
saved into your project but the integration itself - the script runs straight
out of the pipe. Python 3.8+ and nothing else.

Running it twice is free: it checks what is already in place and only fills in
what is missing, so it is also the repair when CubeMX regenerates over the
integration. It never touches your `.ioc`, and never overwrites your
`os_config.h`, `os_cb.c` or `os_main.c` once they exist. `--uninstall` takes it
all back out.

Any other vendor, IDE or build system - and the same six steps by hand - is
**[Installation](doc/installation.md)**.

## Documentation

**[📖 Documentation index](doc/README.md)**

| Getting it running | |
|---|---|
| [Installation](doc/installation.md) | The six-step procedure, for any vendor, IDE and build system - and the [one command](doc/installation.md#on-stm32cubemx-in-one-command) that does all six on a CubeMX project |
| [Vendor notes](doc/vendor-notes.md) | The one thing that differs per vendor, and what to do about it |
| [STM32CubeMX / CubeIDE](doc/stm32cubemx.md) | The same steps on real hardware, checkbox by checkbox |
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
├── examples/   <- one runnable main per feature
├── tools/      <- install_stm32.py, the one-command CubeMX installer
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

MIT. See [LICENSE](LICENSE).
