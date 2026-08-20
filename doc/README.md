# AhuraRTOS documentation

[← Back to the project README](../README.md)

## Install it - one command

Run from the root of your project. Each installer prints the exact diff it wants
to apply and waits for a `y` before touching anything. Python 3.8 or newer and
nothing else - no `pip install`, and nothing is left behind in your project.

**Raspberry Pi Pico SDK** - RP2040, RP2350, RP2354. It reads the chip out of
`PICO_BOARD` / `PICO_PLATFORM`:

```powershell
irm https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_rpi_online.py | python -
```

```bash
curl -fsSL https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_rpi_online.py | python3 -
```

**STM32CubeMX / STM32CubeIDE** - a project generated with the CMake toolchain:

```powershell
irm https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_stm32_online.py | python -
```

```bash
curl -fsSL https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_stm32_online.py | python3 -
```

PowerShell first, then bash / zsh. Options go after the `-`: `--dry-run` shows
the diff and writes nothing, `--yes` skips the prompt, `--uninstall` takes it
back out. Re-running is safe - it fills in only what is missing.

**No internet on the build machine?** Each installer has an offline twin that
only ever reads a copy already on disk. Clone or download the repository
somewhere with a connection, copy it into your project, and run:

```bash
python3 AhuraRTOS/tools/install_rpi_offline.py      # Pico SDK
python3 AhuraRTOS/tools/install_stm32_offline.py    # STM32CubeMX
```

| | Online | Offline | The page |
|---|---|---|---|
| **Raspberry Pi Pico SDK** | `install_rpi_online.py` | `install_rpi_offline.py` | **[Pico SDK](pico-sdk.md)** - [automatic](pico-sdk.md#automatic---one-command) · [offline](pico-sdk.md#offline---no-internet-on-the-machine) · [manual](pico-sdk.md#manual---step-by-step) |
| **STM32CubeMX / CubeIDE** | `install_stm32_online.py` | `install_stm32_offline.py` | **[STM32CubeMX](stm32cubemx.md)** - [automatic](stm32cubemx.md#automatic---one-command) · [offline](stm32cubemx.md#offline---no-internet-on-the-machine) · [manual](stm32cubemx.md#manual---step-by-step) |
| **Everything else** | - | - | **[Installation](installation.md)** - six steps by hand, any vendor, IDE and build system |

## Getting it running

| Page | What is in it |
|---|---|
| **[Installation](installation.md)** | **The general procedure.** Six steps for any vendor, IDE and build system - get the source, copy three files, add them to the build, route the tick, check `PendSV`, boot. Ends with a build-error table |
| **[Raspberry Pi Pico SDK](pico-sdk.md)** | RP2040, RP2350 and RP2354, three ways: **[automatic](pico-sdk.md#automatic---one-command)**, **[offline](pico-sdk.md#offline---no-internet-on-the-machine)**, or **[manual](pico-sdk.md#manual---step-by-step)**. The SoC package supplies the vector names, the tick and the multi-core glue, so there is no PendSV or SysTick step to do by hand. Verified on a Pico 2 |
| **[STM32CubeMX / STM32CubeIDE](stm32cubemx.md)** | On ST tooling, three ways: **[automatic](stm32cubemx.md#automatic---one-command)**, **[offline](stm32cubemx.md#offline---no-internet-on-the-machine)**, or **[manual](stm32cubemx.md#manual---step-by-step)** with the exact CubeMX checkboxes, file paths, CMake block and build commands. Verified end to end on a NUCLEO-H503RB |
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
| **[SoC packages](soc.md)** | The optional per-silicon layer under `kernel/soc/`: who owns which callback, the two configuration values that moved out of `os_config.h`, and how to write a package |
| **[Testing and examples](testing.md)** | The self-test suite and the runnable examples |

- **[Examples](../examples/README.md)** - one runnable
  `os_main.c` per kernel feature, with a README on how to run them.
