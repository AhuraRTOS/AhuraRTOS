/**
 * @file os_tick.h
 * @brief The kernel clock: tick conversion, CPU usage, tickless idle (os_tick.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_TICK_H
#define OS_TICK_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Tick conversion
 * ***********************************************************************************************************
*/

/** Clamp a 64-bit tick count into the uint32_t tick range, one short of the OS_WAIT_FOREVER
 *  sentinel - the same saturation the kernel applies internally to every timeout it converts.
 *  A duration too large for the tick range is a duration the caller cannot have, but truncating
 *  it turns it into a small, plausible-looking one (and, at the sentinel, into "wait forever"),
 *  which no caller can detect. This is what OS_TICKS_FROM_MS below is built on; it expands
 *  its argument twice, so pass a value rather than an expression with side effects. */
#define OS_TICKS_SATURATE(ticks) ((uint32_t)(((ticks) >= (uint64_t)OS_WAIT_FOREVER) ? \
                                             ((uint64_t)OS_WAIT_FOREVER - 1ULL) : (ticks)))

#define OS_TICKS_FROM_MS(ms)    OS_TICKS_SATURATE((((uint64_t)(ms) * (uint64_t)OS_CONFIG_TICK_HZ) + 999ULL) / 1000ULL)


/*
 * ***********************************************************************************************************
 * CPU usage          - OS_CONFIG_CPU_USAGE_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the CPU usage in percent (0..100) since the previous call; one-tick resolution,
 *        so sample at a period well above the tick period (e.g. once per second).
 */
uint32_t os_cpu_usage_get(void);
#endif /* OS_CONFIG_CPU_USAGE_ENABLE */


/*
 * ***********************************************************************************************************
 * Tickless idle      - OS_CONFIG_TICKLESS_ENABLE
 * ***********************************************************************************************************
 *
 * Three kernel-provided control functions, two application-provided hooks and
 * one SoC-provided sleep, all behind the one guard. Calling any of them with
 * tickless idle disabled is a compile error naming the function, not a call
 * that silently does nothing.
*/

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Execute one tickless-idle pass: suppress ticking for the next known-idle duration,
 *        sleep, and announce the real elapsed time on wake.
 */
void os_tickless_idle_process(void);

/******************************************************************************************************/
/**
 * @brief Ticks the kernel would plan to suppress right now, bounded by the earliest kernel
 *        time source (timer expiry, finite-delay sleeper).
 */
uint32_t os_tickless_expected_idle_ticks_get(void);

/******************************************************************************************************/
/**
 * @brief Maximum ticks the active arch port can suppress in one tickless window, given the
 *        platform clock and OS_CONFIG_TICK_HZ (not a fixed constant). Returns 0 when the active
 *        port does not yet suppress ticking for real (see doc/porting.md "Tickless idle").
 */
uint32_t os_tickless_max_suppressed_ticks_get(void);

/******************************************************************************************************/
/**
 * @brief Pre-sleep callback invoked before entering low-power mode. The application must define
 *        it; the kernel provides no default.
 */
void os_tickless_pre_sleep_cb(void);

/******************************************************************************************************/
/**
 * @brief Post-sleep callback invoked after leaving low-power mode, before the kernel accounts for
 *        the sleep. The application must define it; the kernel provides no default.
 */
void os_tickless_post_sleep_cb(void);

/******************************************************************************************************/
/**
 * @brief SoC callback: the sleep instruction itself, inside a suppressed window. Optional; the weak
 *        default is a plain WFI, which is the right answer wherever the wake source keeps running
 *        through it.
 *
 * Distinct from os_arch_soc_idle_cb() and the difference is the whole point: that one waits with
 * NOTHING armed, so only the tick can end it and it may never go deeper than the tick survives.
 * This one is called with the window already open - os_arch_sleep_prepare() has silenced the tick
 * and the package's own timer is counting the wake out - so it may go as deep as THAT timer
 * survives. On an STM32 that is the difference between a WFI and Stop mode.
 *
 * Called with the kernel's interrupt mask held, between os_tickless_pre_sleep_cb() and the elapsed
 * measurement. A WFI - and every HAL Stop entry built on one - still leaves on a pending interrupt
 * while masked, which is exactly how the armed wake ends the window.
 *
 * Whatever the mode costs to leave belongs in os_arch_tick_suppress_min_cb(), so the kernel can
 * refuse a window too short to pay for it. May return spuriously; the idle loop measures what
 * really elapsed and comes back.
 */
void os_arch_soc_sleep_cb(void);

/******************************************************************************************************/
/**
 * @brief Prepare a time-base owner's tickless pass, optionally coordinating peer cores.
 *
 * Called with the local kernel interrupt mask held, with no global kernel lock,
 * before remote kernel entry is closed and before deadlines are sampled. A port
 * may park idle peers here with a bounded wait. Return false to decline this
 * pass; a declining callback must undo everything it acquired before returning.
 * A true return is always paired with os_arch_soc_sleep_finish_cb(), including
 * when the final deadline is too near to sleep. The default returns true.
 * Callbacks must not block on kernel services or wait with a kernel lock held.
 */
bool os_arch_soc_sleep_prepare_cb(void);

/******************************************************************************************************/
/**
 * @brief Release a successful preparation after the entire tickless pass ends.
 *
 * Called without the global lock, after the clock restore, application post-sleep
 * hook, elapsed-time announcement and reopening remote kernel entry. The local
 * kernel mask remains held. Restore any additional mask/state acquired by prepare
 * and release parked peers here. The default does nothing.
 */
void os_arch_soc_sleep_finish_cb(void);

#endif /* OS_CONFIG_TICKLESS_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_TICK_H */
