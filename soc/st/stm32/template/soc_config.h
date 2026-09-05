/**
 * @file soc_config.h
 * @brief Template for the application's soc_config.h - every option of the st/stm32 SoC package
 *        at its default value.
 *
 * NOT included by the package as it sits here: copy it into the application beside os_config.h,
 * as soc_config.h, and change what needs changing. The package finds it on the same include path
 * OS_CONFIG_DIR already puts os_config.h on, so there is nothing further to set. On a CubeMX
 * project that means Core/Inc.
 *
 * REQUIRED, and so is every option in it - the same terms as os_config.h. The file holds ONLY the
 * values a user may need to decide; there are deliberately no built-in defaults, so a missing
 * option is rejected at compile time rather than silently read as 0. Whatever this package can
 * decide for itself is not in the file at all.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef SOC_CONFIG_H
#define SOC_CONFIG_H

/*
 * ***********************************************************************************************************
 * Vendor headers
 * ***********************************************************************************************************
*/

/**
 * The header that brings in this device's CMSIS and HAL declarations.
 *
 * There is no one name to hardcode: it is stm32h5xx_hal.h on an H5, stm32f4xx_hal.h on an F4, and
 * so on for every family. "main.h" is the default because CubeMX generates it for every project
 * and it includes the right family header itself, so one value is correct across the whole STM32
 * range. It also sits in Core/Inc, which is already on the kernel's include path - that is where
 * os_config.h lives.
 *
 * Point it at the family header directly (for instance "stm32h5xx_hal.h") on a project that has
 * no main.h, or one whose main.h drags in more than the SoC layer should see.
 *
 */
#define SOC_CONFIG_HAL_HEADER               "main.h"

/*
 * ***********************************************************************************************************
 * Tick vector
 * ***********************************************************************************************************
*/

/**
 * Whether the package defines SysTick_Handler to call os_tick_handler (1 = yes, the default).
 *
 * The kernel's port owns PendSV and programs SysTick's reload, but the VECTOR has to be defined
 * somewhere. Left to the application it is the same three-line function in every project, rewritten
 * by hand after every CubeMX regeneration; the package writes it once.
 *
 * CubeMX must not generate one of its own, or the two collide as a duplicate symbol:
 *
 *     Pinout & Configuration -> System Core -> NVIC -> Code generation
 *     -> uncheck 'Generate IRQ handler' for 'System tick timer'
 *
 * Uncheck it for 'Pendable request for system service' in the same table while you are there -
 * the kernel's port defines PendSV_Handler, and CubeMX generating one collides in exactly the
 * same way.
 *
 * Set this to 0 to write your own - to add work to the tick, or to count it somewhere. Then define
 * SysTick_Handler in the application and call os_tick_handler() from it; leaving it out entirely
 * gives a board whose kernel clock never advances, because the startup file's weak stub wins.
 *
 * Ignored under OS_CONFIG_TICK_SOURCE_EXTERNAL, where the application owns the timer outright.
 */
#define SOC_CONFIG_SYSTICK_VECTOR           1U

/*
 * ***********************************************************************************************************
 * CPU clock
 * ***********************************************************************************************************
*/

/**
 * Whether os_arch_soc_init_cb() calls SystemCoreClockUpdate() before the kernel starts
 * (1 = yes, the default).
 *
 * The kernel takes the CPU frequency from SystemCoreClock and programs the tick from it, so a
 * stale value gives a tick at the wrong rate and every os_delay_us() busy-wait wrong with it, in
 * a way nothing reports. CubeMX's generated SystemClock_Config() normally leaves the variable
 * correct; this is one call at start-up that makes it true whatever the project did, including a
 * hand-written clock setup that forgot.
 *
 * Set it to 0 only if the application maintains SystemCoreClock itself.
 */
#define SOC_CONFIG_CLOCK_AUTO_UPDATE        1U

/*
 * ***********************************************************************************************************
 * Tickless idle (OS_CONFIG_TICKLESS_ENABLE only)
 * ***********************************************************************************************************
*/

/**
 * Whether the tickless sleep hooks suspend and resume the HAL timebase (1 = yes, the default).
 *
 * This is the option that decides whether tickless idle saves any power on an STM32, so it is
 * worth reading rather than skipping.
 *
 * The kernel takes SysTick, which means CubeMX has been told to run the HAL from a spare timer
 * (SYS -> Timebase Source). That timer keeps interrupting at its own period - 1 kHz by default -
 * and WFI wakes on any pending interrupt whether or not it is masked. So a sleep the kernel
 * planned to run for 500 ms ends after 1 ms, every time, and tickless idle appears to do nothing.
 *
 * HAL_SuspendTick() and HAL_ResumeTick() are ST's own hook for this, and bracketing the sleep with
 * them is what lets it run its full length. HAL_GetTick() does not advance while suspended, which
 * is correct - no time is lost, the counter simply does not tick while the CPU is asleep - but it
 * does mean a HAL driver's own timeout cannot be relied on across the sleep window.
 *
 * Set it to 0 when the HAL timebase must keep running, or when the application brackets the sleep
 * itself by defining the two callbacks (a strong definition in the application replaces the
 * package's weak one).
 */
#define SOC_CONFIG_TICKLESS_HAL_TICK        1U

/*
 * ***********************************************************************************************************
 * Tickless wake source (OS_CONFIG_TICKLESS_ENABLE only)
 * ***********************************************************************************************************
 *
 * There is nothing to pick: what ends a suppressed window follows from how deep the core sleeps,
 * which is SOC_CONFIG_SLEEP_MODE below.
 *
 *   LIGHT   the core clock keeps running, so SysTick is still counting and the port suppresses
 *           against it directly. This package supplies no timer at all and the LPTIM settings
 *           here are not read.
 *   DEEP    Stop mode gates the core clock and SysTick with it, so the LPTIM - clocked from LSI
 *           or LSE - takes the window instead. That is what the settings below describe.
 *
 * It used to be two flags plus the mode, with an arithmetic rule saying exactly one flag had to
 * be 1 and another refusing deep sleep against a source that dies in Stop. Neither state can be
 * expressed any more, so neither rule exists.
 *
 * An RTC wake-up timer is the one thing that would break the tie - it survives Stop as the LPTIM
 * does, and exists on the F series that have no LPTIM at all. It is not implemented here, and it
 * is not listed as an option that would quietly do nothing.
*/

/* Which LPTIM, what its NVIC entry and vector are called, and what its source clock runs at. Only
 * read when SOC_CONFIG_SLEEP_MODE is DEEP.
 *
 * Three names rather than an instance number, because across the STM32 range they are not
 * derivable from one another: the G0 folds this very timer into TIM6_DAC_LPTIM1_IRQn. Copy them
 * from the project you have - the handle from the generated lptim.c or main.c, the other two from
 * the family's stm32<fam>xx.h and startup_stm32<fam>.s.
 *
 * The rate is the clock feeding the LPTIM - 32768 for LSE, about 32000 for LSI. The package pins
 * the prescaler at /1 (see soc_cb.c for why), so whatever CubeMX shows for it is overwritten and
 * this is also what the counter counts at. Read it off the CubeMX clock configuration rather than
 * assuming; a wrong value makes every window the wrong length and nothing reports it.
 * Values: three identifiers as the project spells them; source clock in Hz. */
#define SOC_CONFIG_TICKLESS_LPTIM_HANDLE    hlptim1
#define SOC_CONFIG_TICKLESS_LPTIM_IRQN      LPTIM1_IRQn
#define SOC_CONFIG_TICKLESS_LPTIM_VECTOR    LPTIM1_IRQHandler
#define SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ  32768U

/* How deep the core sleeps inside a window.
 *   OS_CONFIG_SLEEP_MODE_LIGHT   core stops, clocks keep running. Works with every source.
 *   OS_CONFIG_SLEEP_MODE_DEEP    Stop mode. Saves far more, and needs the LPTIM under it.
 * Values: one of the two above. */
#define SOC_CONFIG_SLEEP_MODE               OS_CONFIG_SLEEP_MODE_LIGHT

/*
 * ***********************************************************************************************************
 * Deep sleep entry (SOC_CONFIG_SLEEP_MODE == OS_CONFIG_SLEEP_MODE_DEEP only)
 * ***********************************************************************************************************
*/

/* The Stop entry for this series - the HAL call itself, because ST has no single one across the
 * range and any enum would only be translated back into exactly this. Everything the mode costs
 * besides entering it, the clock restore above all, is done in os_arch_soc_sleep_cb().
 *
 * WFI and never WFE: the kernel arms the wake as an interrupt and holds its own mask, and a WFI
 * leaves on a pending interrupt whether or not it is masked. A WFE would sit through that wake.
 *
 * NOT Standby and NOT Shutdown - those lose SRAM and the core registers, so they do not return to
 * the idle task at all, they reset. Stop is the deepest a kernel can sleep and wake up itself.
 *
 * Anything else meant to end a window early - an EXTI line, a UART - has to be configured as a
 * Stop-mode wake-up in CubeMX; only the LPTIM is arranged for you.
 *
 * Pick the line for your series and confirm it against that family's stm32<fam>xx_hal_pwr.h and
 * _hal_pwr_ex.h - the header on the include path is the authority, not this list:
 *
 *   F0 F1 F2 F3 F4 F7 L0 L1 C0 H5
 *       HAL_PWR_EnterSTOPMode(PWR_MAINREGULATOR_ON,     PWR_STOPENTRY_WFI)
 *       HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI)   lower still
 *       On the H5 both regulator values are 0 and the argument is ignored, so either reads right.
 *       The F4 has one deeper again: HAL_PWREx_EnterUnderDriveSTOPMode(PWR_LOWPOWERREGULATOR_ON,
 *       PWR_STOPENTRY_WFI).
 *
 *   L4 L4+ L5 G0 G4 U5 WB WL WBA
 *       HAL_PWREx_EnterSTOP0Mode(PWR_STOPENTRY_WFI)     Stop 0, shallowest and quickest to leave
 *       HAL_PWREx_EnterSTOP1Mode(PWR_STOPENTRY_WFI)     Stop 1
 *       HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI)     Stop 2, what a battery application wants
 *       HAL_PWREx_EnterSTOP3Mode(PWR_STOPENTRY_WFI)     Stop 3, U5 only
 *       How far the numbering goes differs by family - the ones that exist are the ones declared
 *       in that family's stm32<fam>xx_hal_pwr_ex.h.
 *
 *   H7
 *       HAL_PWREx_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFI, PWR_D1_DOMAIN)
 *       PWR_D2_DOMAIN and PWR_D3_DOMAIN for the other domains.
 *
 * Values: one HAL call with its arguments, no trailing semicolon. Left at the H5's - correct for
 * exactly one series, not for yours. */
#define SOC_CONFIG_DEEP_SLEEP()             HAL_PWR_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFI)

#endif /* SOC_CONFIG_H */
