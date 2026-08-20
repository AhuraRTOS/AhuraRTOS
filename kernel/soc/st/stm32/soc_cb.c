/**
 * @file soc_cb.c
 * @brief SoC-owned kernel callbacks for STMicroelectronics STM32.
 *
 * STM32 asks less of a SoC package than most parts, and it is worth saying why rather than
 * leaving the short file to look unfinished. Every STM32 uses CMSIS-Pack startup files, so the
 * PendSV vector already carries the kernel's default name and SystemCoreClock already exists and
 * is maintained by the generated SystemInit(). Single-core parts need no core id, no inter-core
 * IPI and no hardware spinlock. What is left is the handful of things below.
 *
 * Everything here is weak, so a strong definition anywhere in the application replaces that one
 * callback and leaves the rest of the package in place. Nothing in this file is mandatory: with
 * the HAL absent, or with the options in soc_config.h turned off, each body compiles to nothing
 * and the kernel behaves exactly as it does with no package at all.
 *
 * Options live in soc_config.h, copied from template/soc_config.h into Core/Inc beside
 * os_config.h. The file and every option in it are required, on the same terms as os_config.h: a
 * missing option is a compile error, never a silent default.
 *
 * NOT in this file, deliberately:
 *
 *   - The PendSV vector name. CubeMX generating a competing PendSV_Handler is a project problem,
 *     fixed in the .ioc rather than in code - see doc/vendor-notes.md, and the installer applies
 *     it for you.
 *   - The tick. OS_CONFIG_TICK_SOURCE_SYSTICK is the right answer on almost every STM32 and the
 *     port programs SysTick itself; os_tick_handler() goes in the generated SysTick_Handler.
 *     The low-power L and U families, where SysTick stops in STOP mode, want an LPTIM or RTC tick
 *     instead - that is os_arch_tick_init_cb(), and it is family-specific enough to belong to the
 *     application until this package grows a per-family layer.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "ahura.h"

/*
 * The application's SoC configuration: copy template/soc_config.h into Core/Inc beside
 * os_config.h. It is found on the include path OS_CONFIG_DIR already provides.
 *
 * REQUIRED, and so is every option in it - the same terms as os_config.h. The file holds ONLY the
 * values a user may need to decide; there are deliberately no built-in defaults, so a missing
 * option is rejected at compile time rather than silently read as 0. Whatever this package can
 * decide for itself is not in the file at all.
 */
#if defined(__has_include)
#if !__has_include("soc_config.h")
#error "No soc_config.h found: copy kernel/soc/st/stm32/template/soc_config.h into Core/Inc beside os_config.h."
#endif
#endif

#include "soc_config.h"

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Reject an incomplete configuration rather than filling the gap: a missing option reads as 0 in
 * an #if, silently turning off the clock refresh or the timebase handling. */
#if !defined(SOC_CONFIG_HAL_HEADER) || !defined(SOC_CONFIG_HAL_ENABLE) ||                             \
    !defined(SOC_CONFIG_CLOCK_AUTO_UPDATE) || !defined(SOC_CONFIG_TICKLESS_HAL_TICK)
#error "soc_config.h is incomplete: it must define every option listed in kernel/soc/st/stm32/template/soc_config.h."
#endif

/* How many cores this package supports scheduling on. The STM32 range is overwhelmingly
 * single-core, and the dual-core parts that exist (H7 dual, WB, WL, MP1) pair a Cortex-M7 with a
 * Cortex-M4, or an M4 with an A7 - asymmetric designs that run two separate images rather than
 * the shared ready lists OS_CONFIG_CORE_COUNT describes. */
#define SOC_CORE_COUNT          1U

#if (OS_CONFIG_CORE_COUNT > SOC_CORE_COUNT)
#error "OS_CONFIG_CORE_COUNT is above 1, which the st/stm32 package does not support: STM32 dual-core parts are asymmetric (M7+M4), not the SMP model the kernel's core count describes."
#endif

/* A bare-CMSIS project with no HAL is a perfectly good STM32 project, so the HAL is opt-in rather
 * than assumed - but it is the application that says so, in soc_config.h, not the build guessing
 * from whether a header happens to be reachable. With it off, the clock refresh and the timebase
 * handling below compile away and the kernel behaves as it does with no package at all. */
#if (SOC_CONFIG_HAL_ENABLE != 0U)
#include SOC_CONFIG_HAL_HEADER
#endif

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Prepare the SoC for the kernel. Called by os_init(), first thing.
 *
 * One job on this vendor: make sure SystemCoreClock describes the clock tree the application just
 * configured, because os_init() is about to compute the tick period from it and every
 * os_delay_us() busy-wait reads it afterwards. A stale value gives a tick at the wrong rate with
 * nothing anywhere reporting it, which is a slow and unpleasant thing to find.
 *
 * CubeMX's generated SystemClock_Config() normally leaves the variable correct already, so this
 * is insurance rather than a fix - it costs one call at start-up and makes the guarantee hold for
 * a hand-written clock setup too.
 *
 * Weak here, unlike the RP2 package's strong definition, because it has nothing it must displace:
 * this is the only definition besides the kernel's own weak default, so the linker has no choice
 * to get wrong. The application may still replace it outright.
 */
OS_WEAK void os_arch_soc_init_cb(void)
{
#if (SOC_CONFIG_HAL_ENABLE != 0U) && (SOC_CONFIG_CLOCK_AUTO_UPDATE != 0U)
    SystemCoreClockUpdate();
#endif
}

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Called right before the idle sleep: stop the HAL timebase so the sleep runs its length.
 *
 * This is the one thing that makes tickless idle worth enabling on an STM32, and the reason it
 * needs saying is that leaving the hook empty fails silently rather than loudly.
 *
 * The kernel owns SysTick, which means CubeMX has been told to run the HAL from a spare timer
 * (SYS -> Timebase Source). That timer keeps firing at its own period, 1 kHz by default, and WFI
 * wakes on any pending interrupt whether or not it is masked. So a sleep the kernel planned for
 * 500 ms ends after 1 ms, every time, and tickless idle looks like it simply does not work.
 *
 * HAL_SuspendTick() is ST's own hook for this. HAL_GetTick() does not advance while suspended,
 * which loses no time - the counter just does not run while the CPU is asleep - but it does mean
 * a HAL driver's own timeout cannot be relied on across the sleep window.
 *
 * Deliberately does NOT select a deeper mode. Empty of that, the core takes a plain SLEEP: the
 * CPU clock stops and every peripheral clock keeps running, so nothing needs saving here and the
 * post-sleep hook has nothing to restore. STOP mode stops SysTick itself, which the kernel cannot
 * yet measure a sleep against - see the tickless section of doc/porting.md.
 */
OS_WEAK void os_tickless_pre_sleep_cb(void)
{
#if (SOC_CONFIG_HAL_ENABLE != 0U) && (SOC_CONFIG_TICKLESS_HAL_TICK != 0U)
    HAL_SuspendTick();
#endif
}

/******************************************************************************************************/
/**
 * @brief Called right after wakeup: restart the HAL timebase.
 *
 * Runs with the kernel's interrupts still masked and before the sleep has been announced, so it
 * must stay short: its whole duration is added to interrupt latency.
 */
OS_WEAK void os_tickless_post_sleep_cb(void)
{
#if (SOC_CONFIG_HAL_ENABLE != 0U) && (SOC_CONFIG_TICKLESS_HAL_TICK != 0U)
    HAL_ResumeTick();
#endif
}

#endif /* OS_CONFIG_TICKLESS_ENABLE */
