# Vendor notes

[← Documentation index](README.md) · [Installation](installation.md)

Nothing in the kernel is vendor-specific. The only device-specific symbol
anywhere is CMSIS `SystemCoreClock`, and that has a documented one-line fallback
for devices whose startup code omits it.

What differs between vendors is their *tooling*, in exactly one way that
matters: **whether the code generator emits an interrupt file that also defines
`PendSV_Handler`.** That is what this page is about.

---

## STM32 (STM32CubeMX / CubeIDE)

CubeMX generates non-weak `PendSV_Handler` and `SysTick_Handler` stubs into
`Core/Src/stm32<family>_it.c`, and the kernel defines both itself - the port owns
PendSV, and the `st/stm32` SoC package owns the tick vector. Two definitions of one
symbol is a link error. Deleting them by hand works until the next code generation
puts them back, so turn them off at the source instead:

> **CubeMX → System Core → NVIC → Code generation tab → clear "Generate IRQ
> handler" for *Pendable request for system service* and for *System tick timer*.**

That setting is stored in the `.ioc`, so regeneration keeps honouring it. The
installer checks both and refuses to write anything until they are cleared, naming
the checkbox rather than leaving you to read a linker error.

Nothing is lost by clearing them: both are CubeMX's own empty stubs. An application
that genuinely needs work on the tick sets `SOC_CONFIG_SYSTICK_VECTOR` to `0` in
`soc_config.h` and writes `SysTick_Handler` itself, calling `os_tick_handler()`
from it.

Clear *System service call via SWI instruction* (`SVC_Handler`) as well if you
intend to run the [self-test suite](self-test.md). The kernel never uses `SVC`,
but the suite installs its own handler to reach ISR context from a task, and on
CMSIS-Pack that handler carries the same name - so a generated one collides
exactly like `PendSV_Handler`.

Then move the HAL off SysTick, or the HAL and the kernel will fight over it:

> **CubeMX → System Core → SYS → Timebase Source → any spare timer** (TIM6,
> TIM7 and TIM17 are common picks).

CubeMX adds `stm32<family>_hal_timebase_tim.c` to the project, and from then on
`HAL_Delay()` and `HAL_GetTick()` run off that timer while SysTick belongs to
the kernel. The package's `SysTick_Handler` deliberately does not call
`HAL_IncTick()`, so the two time bases never share the interrupt.

Note that `HAL_Delay()` still busy-waits - it does not yield. Use
`os_delay_ms()` in task code and keep `HAL_Delay()` for driver init paths that
run before `os_start()`.

📖 The full walkthrough on real hardware, with every checkbox and command:
**[AhuraRTOS on STM32](stm32.md)**.

## Nordic (nRF5x, nRF53, nRF91)

Nordic's MDK startup files use the standard CMSIS names, so `PendSV_Handler`
binds with no configuration and nothing needs disabling.

The thing to get right on these parts is the tick. SysTick does not run when the
CPU sleeps, so anything using the low-power modes these devices are chosen for
will lose time. Set `OS_CONFIG_TICK_SOURCE_EXTERNAL` and drive
`os_tick_handler()` from an RTC peripheral instead, as Nordic's own software
does - `template/os_cb.c` has the skeleton.

If a SoftDevice is present, note that it owns the top of the vector table and
forwards the lower SVC range to the application. That is not a problem here,
because the kernel does not use SVC at all.

## NXP, TI, Silicon Labs, Renesas, Microchip, Infineon, GD32

All use CMSIS-Pack startup files with the standard names, and none of their
generators emits a competing `PendSV_Handler`. Copy the three files, route the
tick, build. If a vendor RTOS abstraction is enabled in the project (MCUXpresso
with FreeRTOS selected, for instance), disable it - two RTOSes cannot both own
PendSV.

## Anything else

Open the startup file, find the vector table, and read the name at entry 14
(offset `0x38`). If it is `PendSV_Handler`, there is nothing to do. If it is
something else, set `OS_CONFIG_ARCH_PENDSV_HANDLER` to that name:

```c
#define OS_CONFIG_ARCH_PENDSV_HANDLER  my_pendsv_vector_name
```

The kernel's boot-time vector check will confirm it either way - it reads the
live table through VTOR and traps at the cause if entry 14 does not point at its
own handler, rather than hanging silently at `os_start()`.
