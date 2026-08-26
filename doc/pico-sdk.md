# AhuraRTOS on the Raspberry Pi Pico SDK

[← Documentation index](README.md) · [Installation](installation.md) ·
[SoC packages](soc.md)

The [six general installation steps](installation.md) carried out on a Pico SDK
project - except that two of them are already done for you, because the SoC
package supplies the PendSV vector name and the tick. **Three ways to do it -
pick one:**

| | Route | Good for |
|---|---|---|
| **A** | **[Automatic](#automatic---one-command)** - one command | Any Pico SDK project. Seconds, shows the diff first, safe to re-run |
| **B** | **[Offline](#offline---no-internet-on-the-machine)** - one command, no network | The same project on a machine with no route out: an air-gapped lab, a locked-down corporate network, a CI agent |
| **C** | **[Manual](#manual---step-by-step)** - five steps by hand | When you want to make every edit yourself, or your project does not look like the SDK's template |

All three end in the same place. **No project yet?**
[Starting from a new project](#starting-from-a-new-project) lists the
*New C/C++ Project* wizard settings the kernel is verified against.

## Which chip, which package

One SDK, two packages - the RP2040 and the RP235x are separate silicon
generations, not two steppings, so the choice is real rather than cosmetic. The
installer reads it out of `PICO_PLATFORM`, then `PICO_BOARD`, and stops with
both names if it cannot tell:

| `PICO_BOARD` / `PICO_PLATFORM` contains | Chip | `AHURA_SOC` | Status |
|---|---|---|---|
| `pico2`, `pico_2`, `rp2350`, `rp2354` | RP2350, RP2354 (Cortex-M33) | `raspberrypi/rp235x_arm` | Verified on silicon, single-core **and** dual-core SMP |
| `pico`, `rp2040` | RP2040 (Cortex-M0+) | `raspberrypi/rp2040` | Verified on silicon, single-core and dual-core SMP |

`--soc raspberrypi/rp235x_arm` overrides the detection if your project names its
board some other way.

The commands below were run end to end on a **Pico 2** (RP2350) with Pico SDK
2.3.0 and `arm-none-eabi-gcc` 15.2.

## Starting from a new project

If you are creating the project rather than adding the kernel to one you
already have, the **Raspberry Pi Pico** VS Code extension's *New C/C++ Project*
wizard is the shortest route. These are the settings the kernel was verified
against - the defaults, with two deliberate choices:

**Basic Settings**

| Setting | Value | Why |
|---|---|---|
| Name | anything | It becomes the `add_executable()` target, which is what the installer links `ahura_kernel` to |
| Board type | **Pico 2** | Sets `PICO_BOARD`, which is where the installer reads the chip from. **Pico** gives an RP2040 and the `raspberrypi/rp2040` package instead |
| Architecture (Pico 2) | **RISC-V unchecked** | The kernel has no RISC-V port. Ticking it selects the Hazard3 cores, and the package stops the build with a message saying so - see [below](#the-two-that-are-not-optional) |
| Pico SDK version | **v2.3.0** | What the kernel is tested against |

**Features** - leave all nine unchecked (SPI, I2C, UART, PIO, DMA, HW
interpolation, HW watchdog, HW timer, HW clocks). They only paste example
snippets into `main.c`, and the installer has to brace-match `main()` to place
the boot calls. Add the peripherals you need afterwards, the ordinary way.

**Stdio support** - tick **Console over UART**. Nothing in the kernel needs it,
but `printf` is how `OS_LOG_*` and the whole [self-test suite](self-test.md)
report, so a project without a console has no way to tell you it is working.
Console over USB works just as well; it is `pico_enable_stdio_usb()` instead,
and it disables other USB use.

**Code generation options** - leave all five unchecked:

| Option | Leave off because |
|---|---|
| Run the program from RAM rather than flash | Nothing in the kernel cares. Either works |
| Use project name as entry point file name | Off gives you `main.c`, which is what every path in this page names. On gives `<project>.c`, and the installer still finds it - it searches for the `.c` that defines `main()`, not for a file name |
| Generate C++ code | Nothing stops you - `ahura.h` carries `extern "C"` and its compile-time assertions and type checks are spelled both ways, so a C++ translation unit including it compiles and links against the C kernel. C is simply what the templates and examples are written in |
| Enable C++ RTTI / exceptions | Only meaningful with the above, and both cost memory |

**Debugger** - **DebugProbe (CMSIS-DAP)**, the default. The kernel does not care
which probe you use; this only decides what the extension writes into
`launch.json`.

**CMake Tools** - leave *Enable CMake-Tools extension integration* unchecked.
The kernel block goes at the end of the top-level `CMakeLists.txt` either way,
but the plain layout is the one the installer's anchors were written against.

That gives you exactly the project this page assumes: `CMakeLists.txt`,
`pico_sdk_import.cmake` and `main.c` in one directory, `PICO_BOARD` set to
`pico2`, and a `printf` that comes out of UART0 at 115200 8N1. Now run the
[one command](#automatic---one-command).

### The two that are not optional

Everything above is a preference except these:

- **RISC-V must stay off.** `raspberrypi/rp235x_arm` is the Arm side of the
  chip, and almost everything the kernel touches would change on Hazard3 -
  `isr_pendsv` and `isr_systick` are Cortex-M vectors with no RISC-V
  counterpart, and the IPI is armed through the NVIC. The package refuses the
  build rather than producing one that misbehaves:

  ```text
  CMake Error: AHURA_SOC=raspberrypi/rp235x_arm selected with PICO_PLATFORM='rp2350-riscv'.
  This package is the Arm side of the RP235x, and the kernel has no RISC-V port yet in any case.
  Build for the Cortex-M33 side instead (PICO_PLATFORM=rp2350-arm-s).
  ```

- **The board type has to match the chip.** Selecting `Pico` and then forcing
  `--soc raspberrypi/rp235x_arm` (or the reverse) is caught the same way, at
  configure time, because the two packages arm different inter-core hardware.

---

## Automatic - one command

Run it from the root of your project - the directory holding the top-level
`CMakeLists.txt` and `pico_sdk_import.cmake`.

**Windows** (PowerShell):

```powershell
irm https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_rpi_online.py | python -
```

**Linux and macOS** (bash, zsh):

```bash
curl -fsSL https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_rpi_online.py | python3 -
```

That is the whole thing. It prints the exact diff it wants to apply and waits
for a `y` before touching anything. Python 3.8+ and nothing else - no
`pip install`, and the script runs straight out of the pipe, so no installer
file is left behind in your project.

It opens by saying what it worked out, and that is worth reading before you
answer `y`:

```text
AhuraRTOS installer
  project   /home/me/MyPicoProject
  board     pico2
  soc       raspberrypi/rp235x_arm
  target    my_firmware
  sources   main.c
```

### What it does

| Step | |
|---|---|
| 1 | Puts the AhuraRTOS repository at `AhuraRTOS/` in your project |
| 2 | Copies `os_config.h`, `os_cb.c` and `os_main.c` next to your `main.c`, plus the package's `soc_config.h` |
| 3 | Appends the kernel block to the top-level `CMakeLists.txt`, with `set(AHURA_SOC ...)` chosen from the board |
| 4 | Calls `os_init()` / `os_start()` in `main()`, ahead of its first loop |
| 5 | Leaves the build to you |

**There is no PendSV step and no tick step**, which is why this is shorter than
the [STM32 procedure](stm32cubemx.md). CubeMX generates a competing
`PendSV_Handler` that has to be disabled; the SDK's `crt0.S` declares its vector
entry `isr_pendsv` **weak**, so the kernel's port simply replaces it at link
time. The kernel does have to be *told* that name, and `set(AHURA_SOC ...)` in
step 3 is what tells it. The tick goes the same way: the SDK generates no
`SysTick_Handler` to patch, so the package supplies `isr_systick` instead.

### Options

| Option | Effect |
|---|---|
| `--dry-run` | Print the diff and stop. Writes nothing |
| `--yes`, `-y` | Skip the confirmation |
| `--soc PKG` | Force the package: `raspberrypi/rp2040` or `raspberrypi/rp235x_arm` |
| `--ref REF` | Branch or tag to download (default: `main`) |
| `--project DIR` | Project root (default: the current directory) |
| `--source PATH` | Use this checkout instead of downloading |
| `--update` | Replace an `AhuraRTOS/` already in the project with the current version |
| `--force-templates` | Overwrite an existing `os_config.h` / `soc_config.h` / `os_cb.c` / `os_main.c` |
| `--uninstall` | Take the whole integration back out |

They go after the `-`, which is where the pipe leaves room for the script's own
arguments:

```bash
curl -fsSL .../tools/install_rpi_online.py | python3 - --dry-run
```

A saved copy takes them the same way, without the `-`:

```bash
python3 install_rpi_online.py --dry-run
```

### Running it twice

Nothing happens twice. Each run works out what is already in place and fills in
only what is missing:

| Already there | What the next run does |
|---|---|
| `AhuraRTOS/` | Left exactly as it is - `--update` replaces it |
| `os_config.h`, `soc_config.h`, `os_cb.c`, `os_main.c` | Kept, never overwritten - they are yours the moment they exist |
| The CMake block, the boot calls | Rebuilt at the correct anchor, so a call that was moved or lost comes back |

With everything in place it does no work and no network access at all, and says
so. That last row is also the repair: if a block gets deleted by hand, run the
command again.

### What it will not do

**It never overwrites your files.** Once `os_config.h`, `soc_config.h`,
`os_cb.c` and `os_main.c` exist they are yours, edits and all.

**It never copies `template/soc_cb.c`.** That file is the SoC half of the
callback contract, and the selected `raspberrypi` package *is* that file for
these chips. A second definition of every SoC callback in the application would
silently displace the package's real one - an empty weak stub compiled into the
executable beats a real one sitting in an archive - and on SMP that reads as
every core reporting id 0. See [SoC packages](soc.md#the-one-definition-rule).

**It never guesses the chip.** An unrecognised board is an error naming both
packages, because picking the wrong one is not a near miss: it would arm the
wrong inter-core interrupt and build against the wrong core.

**It never writes a build that would not start.** A Pico `main.c` is written by
hand, so there is no `USER CODE` marker to aim at. The installer brace-matches
`main()`, puts the boot calls in front of its first loop, and then *checks* that
`os_init()` and `os_start()` really landed inside `main()`, in that order, and
above the loop - refusing to write if not.

It stops with an explanation, **before writing anything**, if the project does
not call `pico_sdk_init()`, if FreeRTOS is already in it, or if `main()` cannot
be brace-matched.

Installed? Go straight to **[Build and flash](#5-build-and-flash)**.

---

## Offline - no internet on the machine

Same installation, same result - the only difference is where the kernel comes
from. `install_rpi_offline.py` never touches the network: it uses a copy of the
repository already on the machine, and it does not import `urllib`, `tarfile`
or `socket` to do it, so "offline" is a property of the file rather than a
promise in a comment.

Use it on an air-gapped lab machine, behind a corporate proxy that blocks
GitHub, or on a build agent with no route out.

### 1. Get the repository, on a machine that has a connection

Either a clone or the green **Code → Download ZIP** button:

```bash
git clone https://github.com/AhuraRTOS/AhuraRTOS.git
```

### 2. Copy it into your project and run it

```text
MyPicoProject/
├── CMakeLists.txt        <- the top-level one
├── pico_sdk_import.cmake
├── main.c
└── AhuraRTOS/            <- the repository you copied in
    ├── ahura.h
    ├── kernel/  arch/  soc/  template/
    └── tools/install_rpi_offline.py
```

```bash
cd MyPicoProject
python3 AhuraRTOS/tools/install_rpi_offline.py
```

On Windows:

```powershell
cd MyPicoProject
python AhuraRTOS\tools\install_rpi_offline.py
```

It names where it found the kernel, on a line the online installer has no need
for, and is otherwise identical:

```text
AhuraRTOS installer (offline)
  project   /home/me/MyPicoProject
  kernel    AhuraRTOS
  board     pico2
  soc       raspberrypi/rp235x_arm
  target    my_firmware
  sources   main.c
```

That is the whole procedure. It finds the kernel beside itself, reads the
project, prints the same diff and waits for a `y` - exactly like the online
installer, because it *is* the online installer: everything that reads the
project, computes the edits and writes them with rollback is imported from
`install_rpi_online.py`. Only the "where does the kernel come from" step
differs, so the two cannot drift apart.

A ZIP download unpacks as `AhuraRTOS-main/` rather than `AhuraRTOS/`. That works
too - it is found under either name, and installed to `AhuraRTOS/` so the CMake
block matches what the online installer would have written.

### Where it looks for the kernel

In this order, most explicit first, stopping at the first real checkout:

| | Location |
|---|---|
| 1 | `--source PATH`, if given. A path that is not a checkout is an **error**, never a reason to keep looking - silently installing some other copy is how the wrong kernel version ends up in a build |
| 2 | The checkout this script is running from |
| 3 | `<project>/AhuraRTOS` |
| 4 | `<project>/AhuraRTOS*` - a ZIP still under its branch name |
| 5 | Next to the project, one level up, under either name - for when several projects share one downloaded copy |

If none of them holds a kernel it says so and stops. It will not download one;
that is the entire point of this script.

### Already in the right place

Offline, the source and the destination are routinely the **same directory**:
the repository is at `<project>/AhuraRTOS`, which is exactly where the installer
would otherwise copy it. Copying a directory onto itself is not a copy - the
destination is cleared first, so it would destroy the source.

So "already in place" is treated as nothing to copy, and any copy whose source
and destination resolve to one path is refused. Symlinks, junctions and Windows
paths differing only in case are all resolved before that comparison, so none of
them slips past as a different directory.

```text
  note: the kernel is already at AhuraRTOS/ - installed in place, nothing copied
```

### Offline options

The same as the online installer, minus `--ref` - there is nothing to fetch:

| Option | Effect |
|---|---|
| `--dry-run` | Print the diff and stop. Writes nothing |
| `--yes`, `-y` | Skip the confirmation |
| `--soc PKG` | Force the package: `raspberrypi/rp2040` or `raspberrypi/rp235x_arm` |
| `--source PATH` | Use this checkout, wherever it is |
| `--project DIR` | Project root (default: the current directory) |
| `--update` | Refresh `AhuraRTOS/` in the project from `--source` |
| `--force-templates` | Overwrite an existing `os_config.h` / `soc_config.h` / `os_cb.c` / `os_main.c` |
| `--uninstall` | Take the whole integration back out |

`--uninstall` needs no kernel at all - it only removes managed blocks and the
installed directory - so it keeps working on a machine where the original
download has since been deleted.

---

## Manual - step by step

The same result by hand: five steps, not six, because the SoC package answers
the PendSV and tick steps for you.

### 1. Add the kernel to the project

From the project root, copy a clone of AhuraRTOS in as `AhuraRTOS/`:

```bash
git clone https://github.com/AhuraRTOS/AhuraRTOS.git AhuraRTOS
rm -rf AhuraRTOS/.git      # or keep it, and update with git pull
```

A submodule works the same way - see
[Installation → step 1](installation.md#step-1---get-the-source).

### 2. Copy four files

Three are the usual ones; the fourth is the package's own options. Put them
beside your `main.c`:

| Copy this template | into your project as |
|---|---|
| `AhuraRTOS/template/os_config.h` | `os_config.h` |
| `AhuraRTOS/template/os_cb.c` | `os_cb.c` |
| `AhuraRTOS/template/os_main.c` | `os_main.c` |
| `AhuraRTOS/soc/raspberrypi/<chip>/template/soc_config.h` | `soc_config.h` |

`<chip>` is `rp235x_arm` or `rp2040`, from
[the table above](#which-chip-which-package).

**Do not copy `template/soc_cb.c`.** The package is that file for these
chips - see [what it will not do](#what-it-will-not-do) above.

`soc_config.h` is required, and so is every option in it: a missing one is a
compile error rather than a silent default. It is short, and every option
arrives at a sensible value:

| Option | What it decides |
|---|---|
| `SOC_CONFIG_SPINLOCK_ID` | Which SDK lock id the kernel takes - `PICO_SPINLOCK_ID_OS1`, which the SDK reserves for exactly this |
| `SOC_CONFIG_SYSTICK_VECTOR` | Whether the package defines `isr_systick` for you. `0` to write your own |
| `SOC_CONFIG_CLOCK_AUTO_UPDATE` | Whether start-up reads the live `clk_sys` into `SystemCoreClock` - which is what makes a `set_sys_clock_khz()` in `main()` come out right in the tick period |
| `SOC_CONFIG_IPI_DOORBELL` | RP2350 only: carry the inter-core nudge on a claimed doorbell, or `0` for the SIO FIFO. The RP2040 has no doorbells, so its package has no such option |
| `SOC_CONFIG_FAULT_REPORT` | Whether the package installs its own HardFault vector, printing the faulting core, the address and the fault status. The SDK's default is a breakpoint, which with no debugger attached stops the core silently - and a core that faulted looks exactly like a core that never started |

### 3. Add the block to `CMakeLists.txt`

At the **end** of the top-level `CMakeLists.txt`, after `pico_sdk_init()` and
after your `add_executable()`. `target_sources()` and `target_link_libraries()`
both *append*, so the only ordering requirement is that the target already
exists:

```cmake
# The SoC package: this is what tells the kernel the SDK's vector table calls
# entry 14 isr_pendsv, not PendSV_Handler. Without it the build links cleanly
# and then traps at os_start(). It also supplies isr_systick, SystemCoreClock,
# the core id, the inter-core doorbell and the SIO spinlocks.
set(AHURA_SOC raspberrypi/rp235x_arm)

# OS_CONFIG_DIR must be set BEFORE add_subdirectory, so the kernel library and
# the application compile against the same os_config.h.
set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(AhuraRTOS)

# Editing os_config.h then re-runs CMake by itself.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OS_CONFIG_DIR}/os_config.h)

# Application-owned kernel files: the APPLICATION build, never the kernel library.
target_sources(my_firmware PRIVATE
    os_cb.c
    os_main.c
)

target_link_libraries(my_firmware ahura_kernel)
```

Replace `my_firmware` with the name in your `add_executable()`, and
`raspberrypi/rp235x_arm` with your chip's package.

To build the [self-test suite](self-test.md) as well, add
`add_subdirectory(AhuraRTOS/test)` and link `os_test`. The installer
writes that conditionally, driven by `OS_CONFIG_TEST_ENABLE` in `os_config.h`,
so there is only one switch to flip.

### 4. Boot the kernel in `main.c`

One include, two calls, above whatever loop you already have:

```c
#include <stdio.h>
#include "pico/stdlib.h"
#include "ahura.h"

int main(void)
{
    stdio_init_all();

    os_init();     /* the package's start-up runs inside here */
    os_start();    /* never returns */
}
```

There is nothing SoC-specific to call. The package hooks `os_init()` through
`os_arch_soc_init_cb()`, so the CPU clock, the hardware spinlock and this core's
inter-core interrupt are all ready before the tick is programmed.

Your application code goes in `os_main()`, which is already running as a task:

```c
void os_main(void)
{
    while (1)
    {
        printf("tick\n");
        os_delay_ms(500U);
    }
}
```

### 5. Build and flash

With the Raspberry Pi Pico VS Code extension, the usual **Compile** button is
enough. From a shell, with `cmake`, `ninja` and the SDK's toolchain on `PATH`:

```bash
cmake -B build -G Ninja
cmake --build build
```

The configure log should name the package, the port and the vector it took:

```text
-- Ahura SoC package: raspberrypi/rp235x_arm (Raspberry Pi RP2350 / RP2354, Arm cores)
-- Ahura kernel arch: arm/cortex_m33
-- Ahura PendSV vector: isr_pendsv
```

`isr_pendsv` on that last line is the whole reason the package exists. If it
says `PendSV_Handler`, `AHURA_SOC` did not reach the kernel and the build will
trap at `os_start()`.

`pico_add_extra_outputs()` gives you `build/my_firmware.uf2`. Hold **BOOTSEL**,
plug the board in, and copy it to the mass-storage device that appears - or,
with `picotool` installed:

```bash
picotool load -f build/my_firmware.uf2
```

Output goes wherever `pico_enable_stdio_uart()` / `pico_enable_stdio_usb()` sent
it, at 115200 8N1.

---

## Running both cores

`OS_CONFIG_CORE_COUNT` at `2` in `os_config.h` is the whole of it. `os_start()`
boots the second core through `os_arch_core_launch_cb()`, which the package
implements - there is no call for the application to make, and none to forget.

The package validates that choice against `SOC_CORE_COUNT`, the fact about the
chip, so asking for more cores than the silicon has is a compile error. Running
single-core on a dual-core chip stays entirely reasonable, and is the default.

Both paths have run the full [self-test suite](self-test.md) on RP2350 silicon,
including its dedicated cross-core stress section - contention, wake integrity,
migration and churn.

## If it does not build - or builds and does nothing

| Symptom | Cause |
|---|---|
| Builds, links, then **traps at `os_start()`** | `set(AHURA_SOC ...)` is missing, so the kernel is still looking for `PendSV_Handler` while the SDK's vector says `isr_pendsv` |
| `No os_config.h found` | Step 2 missed, or `OS_CONFIG_DIR` does not point at it |
| `soc_config.h: No such file` | The package's `soc_config.h` was not copied - it is required |
| `multiple definition of 'os_arch_core_id_get_cb'` (or another SoC callback) | `template/soc_cb.c` was copied as well. Delete it: the package is that file |
| Every core reports id 0 on SMP | The same thing, but with a *weak* stub, so it linked silently. Delete `soc_cb.c` |
| `undefined reference to 'os_main'` | `os_main.c` is not in the application build (step 3) |
| Runs, but the tick never fires | `SOC_CONFIG_SYSTICK_VECTOR` is `0` and nothing defines `isr_systick` |

The general table in
[Installation → if it does not build](installation.md#if-it-does-not-build)
covers the vendor-independent half.

## Next step

Prove the integration before writing anything on top of it:
**[Run the self-test suite](self-test.md)**. Set `OS_CONFIG_TEST_ENABLE` to `1`
in `os_config.h`, rebuild, and read the console.
