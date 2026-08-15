# AhuraRTOS documentation

[← Back to the project README](../README.md)

## Getting it running

| Page | What is in it |
|---|---|
| **[Installation](installation.md)** | **Start here.** The manual install with CMake: six steps for any vendor, IDE and build system - get the source, copy three files, add them to the build, route the tick, check `PendSV`, boot. Ends with a build-error table |
| **[STM32CubeMX / STM32CubeIDE](stm32cubemx.md)** | On ST tooling, two ways: **[automatic](stm32cubemx.md#automatic---one-command)** - one command does all of it - or **[manual](stm32cubemx.md#manual---step-by-step)**, with the exact CubeMX checkboxes, file paths, CMake block and build commands. Verified end to end on a NUCLEO-H503RB |
| **[Vendor notes](vendor-notes.md)** | The one thing that differs per vendor - whether its code generator emits a competing `PendSV_Handler` - on STM32, Nordic, NXP, TI, Silicon Labs, Renesas, Microchip, Infineon, GD32, and anything else |
| **[Self-test suite](self-test.md)** | Prove a fresh port before writing anything on top of it. How to enable it, how to read the output, why a silent console is usually the libc, and how much flash to budget |

## The project

| Page | What is in it |
|---|---|
| **[Platforms](platforms.md)** | Which cores, vendors and toolchains are supported today, what is planned, and what is experimental |
| **[Roadmap](roadmap.md)** | The three phases, the known gaps tracked for later, and the current status |

## The kernel itself

The source is in this repository under [`kernel/`](../kernel/), and this is the
authoritative reference for everything the kernel *does*:

Start at the **[Kernel reference](kernel.md)** - it says what the kernel is and
points at the five pages below.

| Page | What is in it |
|---|---|
| **[Getting the kernel into a project](integration.md)** | Quick start, adding it to a build, every `os_config.h` option, and the integration contract |
| **[Using the kernel](api.md)** | Every API: tasks, priorities, mutexes, queues, notifications, atomics, timers, deferred calls, the heap, diagnostics and debugging |
| **[How the kernel works](design.md)** | Boot, the scheduler, the context switch, the tick, blocking and waking, priority inheritance, where the RAM goes, and the source layout |
| **[Platform support](porting.md)** | The callbacks a platform must supply, the clock, TrustZone, multi-core and tickless idle |
| **[Testing and examples](testing.md)** | The self-test suite and the runnable examples |

- **[Examples](../examples/README.md)** - one runnable
  `os_main.c` per kernel feature, with a README on how to run them.
