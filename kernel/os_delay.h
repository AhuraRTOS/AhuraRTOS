/**
 * @file os_delay.h
 * @brief Blocking and busy-wait delays (os_delay.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_DELAY_H
#define OS_DELAY_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Time and delays
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Get the kernel tick counter (wraps at 32 bits).
 */
uint32_t os_tick_get(void);

/******************************************************************************************************/
/**
 * @brief Advance the kernel clock by one tick. Call this, and nothing else, from the tick
 *        interrupt, OS_CONFIG_TICK_HZ times per second.
 *
 * The one kernel call the tick interrupt must make, and on a supported SoC the package has
 * already made it: st/stm32 defines SysTick_Handler and raspberrypi defines isr_systick, each
 * behind SOC_CONFIG_SYSTICK_VECTOR, so those projects route nothing by hand. What the vector
 * looks like where a package does not supply one:
 *
 *     void SysTick_Handler(void) { os_tick_handler(); }
 *
 * Nothing else belongs there; on STM32 do not also call HAL_IncTick(). With EXTERNAL the
 * application's own timer ISR calls it. Give that interrupt the lowest priority the device offers.
 */
void os_tick_handler(void);

/*
 * The three delays return nothing. A delay either waits or the request was one the platform
 * cannot express - an unreadable CPU clock, or a duration too long for a 32-bit tick count - and
 * both of those are programming or configuration errors that OS_ASSERT reports where they happen,
 * rather than a status every call site would have to cast away.
 */

/******************************************************************************************************/
/**
 * @brief Block the calling task for the requested milliseconds (busy-waits before os_start).
 *        OS_WAIT_FOREVER parks the calling task permanently (never returns).
 */
void os_delay_ms(uint32_t milliseconds);

/******************************************************************************************************/
/**
 * @brief Busy-wait for the requested microseconds (precise, does not yield).
 */
void os_delay_us(uint32_t microseconds);

#ifdef __cplusplus
}
#endif

#endif /* OS_DELAY_H */
