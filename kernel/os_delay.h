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

/* Nonblocking delays require an IRQ-independent counter: DWT, mcycle, or
 * os_arch_reference_clock_*_cb supplied by the SoC. The RP packages supply
 * their hardware TIMER. A port with neither a CPU counter nor a reference
 * timer calls os_arch_config_fault_trap on a nonzero busy-wait, including
 * when assertions are disabled; it never substitutes an ISR-fed estimate.
 * Resolution is one counter unit, and the timer frequency must remain stable
 * for the duration of a busy-wait. */

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
