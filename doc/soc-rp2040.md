# Raspberry Pi RP2040

```cmake
set(AHURA_SOC raspberrypi/rp2040)
```

The RP2040 - the chip in the original Pico and Pico W. Dual Cortex-M0+.

Note this is a **chip** name, not a board: `pico`, `pico_w` and a long tail of
third-party boards all carry an RP2040 and all use this package.

## Layout

Most of this package is `soc/raspberrypi/common/`, compiled in alongside the file here:

| | |
|---|---|
| `soc/raspberrypi/common/soc_common.c` | Core id, kernel spinlock, `SystemCoreClock`, `isr_systick`, core 1 launch - identical on every RP2 chip, because it is all SIO or plain SDK |
| `soc_cb.c` | The RP2040's inter-core interrupt, carried on the SIO FIFO |

Splitting on the chip rather than `#if`-ing one file is deliberate: the RP2040 and the RP235x are separate silicon generations, not two
steppings - Cortex-M0+ against Cortex-M33, no Security Extension against
TrustZone, no doorbells against doorbells. There is no `rp2040_riscv` and
never will be, which is why this one carries no architecture suffix.

## What it supplies

| | |
|---|---|
| **PendSV vector** | `isr_pendsv`. The SDK's `crt0.S` names entry 14 that, not the CMSIS-Pack `PendSV_Handler` the kernel defaults to. Without it the kernel builds cleanly and then **traps at `os_start()`** |
| **`SystemCoreClock`** | Defined by the package; the SDK only provides it in its optional CMSIS stub, which `pico_stdlib` does not pull in |
| **Core id** | SIO `CPUID` via `get_core_num()` |
| **IPI** | The inter-core FIFO. The handler drains it and pends PendSV |
| **Spinlock** | The SDK's `spin_lock` API under `PICO_SPINLOCK_ID_OS1`, which it reserves for exactly this use. On this chip that resolves to a real SIO hardware spinlock. The Cortex-M0+ has no exclusive instructions at all, so the kernel's built-in backend could not work here in any case |
| **Tick** | `isr_systick` calling `os_tick_handler()` |
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

## Status

Single-core has run the full self-test on silicon. The multi-core glue is
written against the SDK's documented API but **has not run on real silicon**
(the RP2350 package has - see [its page](soc-rp235x-arm.md)), which matches the kernel's own
position: the ARMv6-M SMP paths compile and are exercised in CI, and are
documented as experimental.

Cache coherency is not a concern here - these chips have no data cache between
the cores and SRAM - so the second `OS_CONFIG_CORE_COUNT` precondition in
`os_config.h` is satisfied for free. It is listed because it very much is not,
on parts that do.
