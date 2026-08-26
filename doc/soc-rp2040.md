# Raspberry Pi RP2040

[← SoC packages](soc.md)

```cmake
set(AHURA_SOC raspberrypi/rp2040)
```

The RP2040 - the chip in the original Pico and Pico W. Dual Cortex-M0+.

Note this is a **chip** name, not a board: `pico`, `pico_w` and a long tail of
third-party boards all carry an RP2040 and all use this package.

## Installing

Three routes, all on one page - **[Raspberry Pi Pico SDK](pico-sdk.md)**:
[one command](pico-sdk.md#automatic---one-command) ·
[offline](pico-sdk.md#offline---no-internet-on-the-machine) ·
[by hand](pico-sdk.md#manual---step-by-step). The installer detects the board
and selects this package for you.

Everything below is what the package *is*, and what your application owes it.

## Layout

Most of this package is `soc/raspberrypi/common/`, compiled in alongside the file here:

| | |
|---|---|
| `soc/raspberrypi/common/soc_common.c` | Core id, kernel spinlock, `SystemCoreClock`, `isr_systick`, core 1 launch - identical on every RP2 chip, because it is all SIO or plain SDK |
| `soc_cb.c` | The RP2040's inter-core interrupt, carried on the SIO FIFO |

Splitting on the chip rather than `#if`-ing one file is deliberate: the RP2040
and the RP235x are separate silicon generations, not two steppings - Cortex-M0+
against Cortex-M33, no Security Extension against TrustZone, no doorbells
against doorbells. There is no `rp2040_riscv` and never will be, which is why
this one carries no architecture suffix.

## What it supplies

| | |
|---|---|
| **Context-switch vector** | `isr_pendsv`. The SDK's `crt0.S` names entry 14 that, not the CMSIS-Pack `PendSV_Handler` the kernel defaults to. Without it the kernel builds cleanly and then **traps at `os_start()`** |
| **Tick** | `isr_systick` calling `os_tick_handler()` |
| **`SystemCoreClock`** | Defined by the package; the SDK only provides it in its optional CMSIS stub, which `pico_stdlib` does not pull in |
| **Core id** | SIO `CPUID` via `get_core_num()` |
| **IPI** | The inter-core FIFO. The handler drains it and pends PendSV |
| **Spinlock** | The SDK's `spin_lock` API under `PICO_SPINLOCK_ID_OS1`, which it reserves for exactly this use. On this chip that resolves to a real SIO hardware spinlock. The Cortex-M0+ has no exclusive instructions at all, so the kernel's built-in backend could not work here in any case |
| **Start-up** | `os_arch_soc_init_cb()`, which the kernel calls from `os_init()`. Nothing for the application to call |

## Using it

```c
#include "ahura.h"

int main(void)
{
    stdio_init_all();

    os_init();                 /* the package's start-up runs inside here */
    os_start();                /* does not return */
}
```

Setting `OS_CONFIG_CORE_COUNT` to 2 is the whole of enabling the second core.
`os_start()` boots it through `os_arch_core_launch_cb()`, which this package
implements - there is no call for the application to make, and none to forget.

The application still copies `template/os_cb.c` for its own half of the
callback contract - log output, assertions, stack overflow. It must **not** copy
`template/soc_cb.c`: this package is that file for these chips.

Options live in `soc_config.h`: copy `template/soc_config.h` beside your
`os_config.h`. It is required, and so is every option in it - a missing one is
a compile error, never a silent default.

## Notes

**Cache coherency is not a concern here** - these chips have no data cache
between the cores and SRAM - so the second `OS_CONFIG_CORE_COUNT` precondition
in `os_config.h` is satisfied for free. It is listed because it very much is
not, on parts that do.

## Status

The full self-test passes on silicon, single-core and dual-core.

Getting there cost four bugs, all of them in code that had compiled for years
and never executed - which is worth recording, because "it builds" was doing a
lot of unearned work in this package's favour:

- The shared HardFault vector was written in Thumb-2. It cannot assemble for
  ARMv6-M at all, so this package had never been through a compiler.
- The cycle counter synthesized from SysTick only advanced when somebody polled
  it, so a period that elapsed unobserved was lost for good. It ran at a
  hundredth of the CPU clock and could step backwards.
- Core 0 armed its inter-core interrupt before `multicore_launch_core1()`. On
  this chip the IPI shares the FIFO the launch handshake uses, so the handler
  ate the words the launch was still reading and pended a context switch on a
  core with no first task.
- Core 1 armed its own before the kernel had primed PSP, so the first interrupt
  it took drove a context switch through a stack pointer that had never been
  one.

The last two are the same mistake from opposite ends, and neither could appear
on the RP2350: its IPI is a doorbell, entirely separate from the launch FIFO.
