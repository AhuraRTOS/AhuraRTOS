# STMicroelectronics STM32

```cmake
set(AHURA_SOC st/stm32)
```

**This package contributes no code**, and that is a finding rather than an
omission. Selecting it and leaving `AHURA_SOC` unset produce identical builds
today.

Every STM32 uses CMSIS-Pack startup files, so:

- the PendSV vector already carries `PendSV_Handler`, the kernel's default;
- `SystemCoreClock` is already defined, and kept current by the generated
  `SystemInit()` and `SystemCoreClockUpdate()`;
- single-core parts need no core id, no IPI and no spinlock.

There is nothing left for a package to supply. STM32 is the kernel's primary
bring-up target precisely because it asks so little.

## Then why does it exist

1. **To say so.** "STM32 needs no SoC glue" is worth stating where it can be
   checked, rather than left to be inferred from an absence.
2. **For the installers.** `install_stm32_online.py` and
   `install_rpi_online.py` write the same shape of CMake block, with one
   `AHURA_SOC` line that differs only in its value.
3. **As the landing site** for the STM32-specific work that is genuinely
   coming - see below.

## What STM32 projects do need

Not kernel configuration, but **CubeMX settings**, because the generator
competes for two things the kernel needs:

- stop it emitting its own `PendSV_Handler` (NVIC → Code generation);
- move the HAL timebase off SysTick to a spare timer (SYS → Timebase Source).

Both are project edits rather than kernel ones, which is why they live in
[vendor notes](../../../doc/vendor-notes.md) and are applied by the installer.

## Expected additions

- **An LPTIM or RTC tick** for the low-power L and U families, where SysTick
  stops in STOP mode. This is the same problem Nordic has, and the reason
  `OS_CONFIG_TICK_SOURCE_EXTERNAL` exists; a package can supply
  `os_arch_tick_init_cb` so projects stop writing it by hand.
- **Tickless sleep hooks** that suspend the HAL timebase, so a suppressed sleep
  is not cut short at that timer's period.
- **The dual-core H7 parts** (Cortex-M7 plus Cortex-M4). Note that these are
  asymmetric - two kernels, or one kernel and bare-metal - not the shared ready
  lists `OS_CONFIG_CORE_COUNT` describes, so this needs a design decision
  before it needs code.
