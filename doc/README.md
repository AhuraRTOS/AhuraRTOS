# AhuraRTOS documentation

[← Back to the project README](../README.md)

## Getting it running

| Page | What is in it |
|---|---|
| **[Installation](installation.md)** | Both routes to the same six things. **[Automatic](installation.md#automatic---stm32cubemx)** - one command, on an STM32CubeMX CMake project. **[Manual](installation.md#manual---any-vendor-any-toolchain)** - by hand, for any vendor, IDE and build system: get the source, copy three files, add them to the build, route the tick, check `PendSV`, boot. Ends with a build-error table |
| **[Vendor notes](vendor-notes.md)** | The one thing that differs per vendor - whether its code generator emits a competing `PendSV_Handler` - on STM32, Nordic, NXP, TI, Silicon Labs, Renesas, Microchip, Infineon, GD32, and anything else |
| **[STM32CubeMX / STM32CubeIDE](stm32cubemx.md)** | The same steps on real hardware, with the exact CubeMX checkboxes, file paths, CMake block and build commands. Verified end to end on a NUCLEO-H503RB |
| **[Self-test suite](self-test.md)** | Prove a fresh port before writing anything on top of it. How to enable it, how to read the output, why a silent console is usually the libc, and how much flash to budget |

## The project

| Page | What is in it |
|---|---|
| **[Platforms](platforms.md)** | Which cores, vendors and toolchains are supported today, what is planned, and what is experimental |
| **[Roadmap](roadmap.md)** | The three phases, the known gaps tracked for later, and the current status |

## The kernel itself

The source is in this repository under [`kernel/`](../kernel/), and this is the
authoritative reference for everything the kernel *does*:

- **[Kernel reference](kernel.md)** -
  what the kernel is, **how it works** (boot sequence, scheduler, context
  switch, tick, blocking and waking, priority inheritance, the three barriers,
  memory), every API, every `os_config.h` option, platform support, and
  internals.
- **[Examples](../examples/README.md)** - one runnable
  `os_main.c` per kernel feature, with a README on how to run them.
