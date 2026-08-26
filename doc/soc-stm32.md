# STMicroelectronics STM32

[← SoC packages](soc.md)

```cmake
set(AHURA_SOC st/stm32)
```

Every STM32, from a C0 to an H7. This package asks less of the silicon than the
Raspberry Pi ones do, because **CMSIS-Pack startup files already give the kernel
most of what it needs**:

- the PendSV vector already carries `PendSV_Handler`, the kernel's default;
- `SystemCoreClock` is already defined, and kept current by the generated
  `SystemInit()` and `SystemCoreClockUpdate()`;
- single-core parts need no core id, no IPI and no spinlock.

So there is no vector name to state and no spinlock backend to force. STM32 is
the kernel's primary bring-up target precisely because it asks so little.

## Installing

Three routes, all on one page - **[STM32CubeMX / STM32CubeIDE](stm32cubemx.md)**:
[one command](stm32cubemx.md#automatic---one-command) ·
[offline](stm32cubemx.md#offline---no-internet-on-the-machine) ·
[by hand](stm32cubemx.md#manual---step-by-step).

Not on ST tooling? The [six generic steps](installation.md) apply unchanged -
this package is orthogonal to how the project was generated.

Everything below is what the package *is*, and what your application owes it.

## Layout

| | |
|---|---|
| `soc_cb.c` | The whole package: a clock refresh at start-up and two tickless sleep hooks. Every entry point is `OS_WEAK`, so an application that wants its own simply defines it |
| `template/soc_config.h` | Four options, copied beside your `os_config.h` |

There is no public header, because the package has nothing for the application
to call - every entry point is a `_cb` the kernel invokes itself.

`AHURA_SOC_ARCH` is deliberately **not** set here. The core varies across the
range - M0+ on C0/G0/L0, M4 on F4/L4/G4, M7 on F7/H7, M33 on H5/U5/WBA - and
CubeMX always puts a `-mcpu` in `CMAKE_C_FLAGS` for the kernel's own detection
to read, so a value here could only be wrong.

## What it supplies

| | |
|---|---|
| **Context-switch vector** | Nothing to supply. CMSIS-Pack startup already names it `PendSV_Handler`, which is the kernel's default - but CubeMX will *generate its own* unless told not to, and that one wins at link time. See [Using it](#using-it) |
| **Tick** | Nothing to supply, but the HAL competes for SysTick and must be moved off it. See [Using it](#using-it) |
| **`SystemCoreClock`** | Already defined by CMSIS-Pack. The package refreshes it in `os_arch_soc_init_cb()` before the kernel programs its tick, so a clock tree brought up after `SystemInit()` is still reflected. `SOC_CONFIG_CLOCK_AUTO_UPDATE 0` turns that off |
| **Core id** | Not needed - single-core parts |
| **IPI** | Not needed - single-core parts |
| **Spinlock** | Not needed - the kernel's own backend is correct on one core |
| **Tickless hooks** | `os_tickless_pre_sleep_cb()` / `os_tickless_post_sleep_cb()` suspend and resume the HAL timebase, so a suppressed sleep is not cut short at that timer's period. `SOC_CONFIG_TICKLESS_HAL_TICK 0` turns that off |
| **HAL include path** | The kernel library compiles `soc_cb.c`, which reaches the HAL through `SOC_CONFIG_HAL_HEADER`. The package links CubeMX's `stm32cubemx` INTERFACE target when it exists, so the kernel sees the same HAL tree the application does |

Everything here degrades to nothing when the HAL is absent, which is what makes
the package safe on a hand-written project.

## Using it

```c
#include "ahura.h"

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    /* MX_*_Init(), and whatever the kernel's callbacks log through */

    os_init();                 /* the package's start-up runs inside here */
    os_start();                /* does not return */
}
```

Options live in `soc_config.h`: copy `template/soc_config.h` beside your
`os_config.h`. It is required, and so is every option in it - a missing one is
a compile error, never a silent default. `SOC_CONFIG_HAL_HEADER` is the one to
check first; on a CubeMX project it is `"main.h"`.

The application still copies `template/os_cb.c` for its own half of the callback
contract - log output, assertions, stack overflow. It must **not** copy
`template/soc_cb.c`: this package is that file for these parts.

**Two CubeMX settings are not optional**, and they are project edits rather than
kernel ones - the generator competes for two things the kernel needs:

| In CubeMX | What to do | Why |
|---|---|---|
| NVIC → Code generation | Untick `PendSV_Handler` | Otherwise CubeMX emits an empty one that wins at link time, and the kernel builds cleanly then traps at `os_start()` |
| SYS → Timebase Source | Move off SysTick to a spare timer | The HAL takes SysTick for `HAL_Delay()`; the kernel needs it for the tick |

Both are applied for you by the [one-command installer](stm32cubemx.md#automatic---one-command),
and spelled out with menu paths in [vendor notes](vendor-notes.md).

## Notes

**Why the package exists at all**, given how little it contributes:

1. **To say so.** "STM32 needs almost no SoC glue" is worth stating where it can
   be checked, rather than left to be inferred from an absence.
2. **For the installers.** `install_stm32_online.py` and `install_rpi_online.py`
   write the same shape of CMake block, with one `AHURA_SOC` line that differs
   only in its value.
3. **As the landing site** for the STM32-specific work that is genuinely coming.

**Expected additions:**

- **An LPTIM or RTC tick** for the low-power L and U families, where SysTick
  stops in STOP mode. This is the same problem Nordic has, and the reason
  `OS_CONFIG_TICK_SOURCE_EXTERNAL` exists; a package can supply
  `os_arch_tick_init_cb` so projects stop writing it by hand.
- **The dual-core H7 parts** (Cortex-M7 plus Cortex-M4). Note that these are
  asymmetric - two kernels, or one kernel and bare-metal - not the shared ready
  lists `OS_CONFIG_CORE_COUNT` describes, so this needs a design decision
  before it needs code.

## Status

Verified end to end on a **NUCLEO-H503RB**: the full self-test passes on
silicon, both from the one-command installer and from the manual route.

Single-core only. The tickless hooks are wired but share the kernel's overall
tickless status - implemented, not yet driven by the idle task.
