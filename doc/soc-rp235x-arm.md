# Raspberry Pi RP2350 / RP2354 - Arm cores

[← SoC packages](soc.md)

```cmake
set(AHURA_SOC raspberrypi/rp235x_arm)
```

The RP2350 and RP2354 running their Cortex-M33 cores - the chips in the Pico 2
and Pico 2 W. `rp235x` covers both because the SDK has no separate platform for
the RP2354: it is an RP2350 die with stacked flash.

`_arm` because the same silicon can boot Hazard3 RISC-V cores instead. That is a
sibling package, [`raspberrypi/rp235x_riscv`](soc-rp235x-riscv.md), not a switch
inside this one - almost everything the kernel asks of the chip is answered
differently on that core.

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
| `soc_cb.c` | The RP2350's inter-core interrupt, carried on a claimed doorbell. Also where RP2350-only work lands as it arrives - TrustZone first, since the Cortex-M33 has the Security Extension and the RP2040's M0+ does not |

Splitting on the chip rather than `#if`-ing one file is deliberate: the RP2040
and the RP235x are separate silicon generations, not two steppings - Cortex-M0+
against Cortex-M33, no Security Extension against TrustZone, no doorbells
against doorbells.

## What it supplies

| | |
|---|---|
| **Context-switch vector** | `isr_pendsv`. The SDK's `crt0.S` names entry 14 that, not the CMSIS-Pack `PendSV_Handler` the kernel defaults to. Without it the kernel builds cleanly and then **traps at `os_start()`** |
| **Tick** | `isr_systick` calling `os_tick_handler()` |
| **`SystemCoreClock`** | Defined by the package; the SDK only provides it in its optional CMSIS stub, which `pico_stdlib` does not pull in |
| **Core id** | SIO `CPUID` via `get_core_num()` |
| **IPI** | A claimed doorbell, the chip's purpose-built inter-core interrupt, leaving the FIFO free for the SDK. `SOC_CONFIG_IPI_DOORBELL 0` falls back to the FIFO |
| **Spinlock** | The SDK's `spin_lock` API under `PICO_SPINLOCK_ID_OS1`, which it reserves for exactly this use. On this chip the SDK defaults `PICO_USE_SW_SPIN_LOCKS` to 1 because of **errata E2**, so it is a software lock built on `LDAEXB`/`STREXB`. Going through the SDK's API is what makes the kernel inherit that workaround |
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

Both paths have run the full self-test on real silicon: single-core, and
dual-core SMP including the suite's dedicated cross-core stress section
(contention, wake integrity, migration, churn). The RP2040's ARMv6-M glue
has also run on hardware now - see [its page](soc-rp2040.md).
