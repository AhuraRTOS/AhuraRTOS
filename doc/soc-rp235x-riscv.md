# Raspberry Pi RP2350 / RP2354 - Hazard3 RISC-V cores

```cmake
set(AHURA_SOC raspberrypi/rp235x_riscv)
```

The RP2350 and RP2354 running their two Hazard3 RISC-V cores instead of their
two Cortex-M33s. Same die, same package, same board: an RP2350 ships both core
complexes and boots one of them, chosen by `PICO_PLATFORM=rp2350-riscv` at build
time.

The sibling of [`raspberrypi/rp235x_arm`](soc-rp235x-arm.md), and a **separate
package rather than an `#if`** inside it. That is not filing preference: almost
every question the kernel asks the silicon is answered differently here. There
is no PendSV, no SysTick, no `WFI` on the same terms, and the fault report has
nothing in common - only the chip underneath is shared.

The package refuses a build for the wrong core rather than producing one that
links and then misbehaves: selecting it with `PICO_PLATFORM` set to anything but
a RISC-V platform is a `FATAL_ERROR` naming both the mistake and the fix.

## What it supplies

| | |
|---|---|
| **Context-switch vector** | The machine software interrupt, trap cause 3. `crt0_riscv.S` names it `isr_riscv_machine_soft_irq` and declares it weak, so the port's handler simply replaces it at link time - there is no `AHURA_SOC_PENDSV_HANDLER` to set, because that name is already the RISC-V port's default |
| **Yield and IPI** | One register, `SIO_RISCV_SOFTIRQ`. It carries a set bit and a clear bit **per core**, so "reschedule me" and "reschedule the other core" are the same write with a different bit - which is why `os_arch_swi_request_cb()` and `os_arch_core_ipi_request_cb()` are one line each here, where the Arm side needs PendSV plus a doorbell |
| **Tick** | `mtime`/`mtimecmp`, taken as ordinary system IRQ 29 (`SIO_IRQ_MTIMECMP`) rather than on `mip.MTIP` - see below. Installed as a *shared* handler, so an application handler already on that line is not displaced |
| **Core id** | `mhartid`, read inline by the kernel as a single CSR instruction. The package sets `OS_CONFIG_ARCH_CORE_ID_MHARTID` to say the two agree on this chip - the RISC-V spec does not promise it in general - and verifies the claim against SIO `CPUID` once per core at boot. `os_critical_enter()` and `os_critical_exit()` each need this core's index, so every kernel operation that takes a critical section pays for it twice; a CSR read costs one instruction where a callback costs a cross-module call and a bus access |
| **Spinlock** | The SDK's `spin_lock` API under `SOC_CONFIG_SPINLOCK_ID` (`PICO_SPINLOCK_ID_OS1`, which the SDK reserves for exactly this). Going through the SDK is what makes the kernel inherit the errata workarounds it carries for these locks, instead of keeping a second copy in the port's own `lr.w`/`sc.w` |
| **Trap stack** | One per secondary core, sized by `SOC_CONFIG_HANDLER_STACK_SIZE` |
| **CPU clock** | `os_arch_clock_hz_get()` off the live `clk_sys` |
| **Start-up** | `os_arch_soc_init_cb()`, called by the kernel from `os_init()`. Nothing for the application to call |

Most of the package is `soc_cb.c` itself. Unlike the Arm sibling it does **not**
compile in `soc/raspberrypi/common/`: that file is SysTick, CMSIS
`SystemCoreClock` and `isr_pendsv`, none of which exist on this core.

## Two decisions worth knowing

**The context-switch handler is placed in RAM**, and on this chip that is a
link-time requirement rather than a preference. The SDK puts the vectored
`mtvec` table in `.data` unless `PICO_NO_RAM_VECTOR_TABLE` is set, while code
runs from flash - and those two are 256 MB apart. Each table entry is a `JAL`,
which reaches ±1 MB, so a handler left in `.text` does not link:
*relocation truncated to fit*. The package therefore sets

```cmake
OS_CONFIG_ARCH_SWI_SECTION=".time_critical.ahura_switch"
```

`.time_critical` is the SDK's own RAM-code section, the one `__not_in_flash_func()`
uses, so the handler lands near the table. It is also where an RTOS wants its
context switch anyway - the hottest path in the kernel stops paying XIP latency
on every switch.

**The tick is an external IRQ, not `mip.MTIP`.** `mtimecmp` can reach the core
either way, and this package takes IRQ 29 for two reasons. The SDK recommends it
- `crt0_riscv.S` says of MTIP that IRQ 29 "may be a better option, because it
plays nicely with interrupt preemption". And it is what keeps `os_arch_in_isr()`
correct: that function reads Hazard3's `meicontext`, which accounts for external
IRQs and knows nothing about MTIP. A tick arriving on cause 7 would run with the
kernel believing it was in task context.

The comparator is armed per core, because `mtimecmp` is core-local - the
datasheet is explicit that each core gets its own copy, routed to its own
interrupt line - while the timer itself is shared and is enabled once, from
core 0. It counts `clk_sys` rather than the 1 MHz `ticks` reference, so the tick
period derives from the same number `os_arch_clock_hz_get()` reports and the
kernel already uses for its microsecond waits.

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

Build for the RISC-V cores by selecting the platform, which the Pico VS Code
extension and the CMake cache both express the same way:

```bash
cmake -B build -DPICO_PLATFORM=rp2350-riscv -DPICO_BOARD=pico2
```

The application still copies `template/os_cb.c` for its own half of the callback
contract - log output, assertions, stack overflow. It must **not** copy
`template/soc_cb.c`: this package *is* that file for this core.

Options live in `soc_config.h`: copy `template/soc_config.h` beside your
`os_config.h`. It is required, and so is every option in it - a missing one is a
compile error, never a silent default.

`OS_CONFIG_CORE_COUNT` at 2 is the whole of enabling the second Hazard3.
`os_start()` releases it through `os_arch_core_launch_cb()`, which this package
implements; there is no call for the application to make, and none to forget.

## Status

The self-test suite builds and runs on this package on a single core. The
dual-core path has NOT yet been confirmed on silicon: core 1's entry point and
the shared tick-handler registration were both wrong until recently, and the
suite's SMP section is the thing to run first on a board.

Cache coherency is not a
concern on these chips - there is no data cache between the cores and SRAM - so
the second `OS_CONFIG_CORE_COUNT` precondition in `os_config.h` is satisfied for
free, exactly as on the Arm side.

The RISC-V port is newer than the Arm one. Prove a board with the
[self-test suite](self-test.md) before building on it, which is what that suite
is for.
