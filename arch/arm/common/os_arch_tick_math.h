/**
 * @file os_arch_tick_math.h
 * @brief Pure integer timing arithmetic shared by the Cortex-M tickless paths.
 *
 * Split out of the tickless code so the v8m self-SysTick window and the shared LIGHT-sleep
 * adapter reach the same answer from the same arithmetic, and so a host regression can exercise
 * that arithmetic without a target. Nothing here touches a register or the kernel: every input
 * is a value the caller already sampled.
 *
 * Deliberately self-contained - <stdint.h> and nothing else. os_arch_tickless.c includes it
 * before the port header is in scope, and a host test includes it on its own, so it cannot reach
 * for OS_INLINE or any other port macro.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_ARCH_TICK_MATH_H
#define OS_ARCH_TICK_MATH_H

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Count the whole tick boundaries a suppressed window crossed.
 *
 * SysTick stays on its original grid: the counter position supplies the exact fractional phase and
 * the independent reference timer only disambiguates whole wraps. The combined sampling error must
 * therefore stay below half a SysTick period, or the rounding below lands on the wrong boundary.
 *
 * @param[in] reference_cycles  Elapsed reference time, converted to CPU cycles.
 * @param[in] start_cvr         SysTick CVR sampled when the window opened.
 * @param[in] end_cvr           SysTick CVR sampled when the window closed.
 * @param[in] period            Reload period in cycles, never zero.
 * @return uint32_t  Whole tick boundaries crossed, zero when the window did not reach one.
 */
static inline uint32_t os_arch_tick_wraps(uint64_t reference_cycles,
                                          uint32_t start_cvr, uint32_t end_cvr,
                                          uint32_t period)
{
    int64_t whole;

    /* SysTick pends on the transition to zero, one cycle BEFORE the reload. Giving zero the new
     * period's phase hands that boundary to the IRQ it belongs to rather than counting it here. */
    start_cvr = (start_cvr == 0U) ? period : start_cvr;
    end_cvr   = (end_cvr == 0U) ? period : end_cvr;

    whole = (int64_t)reference_cycles + (int64_t)end_cvr - (int64_t)start_cvr;

    return (whole <= 0) ? 0U : (uint32_t)(((uint64_t)whole + (period / 2U)) / period);
}

/******************************************************************************************************/
/**
 * @brief Cycles left in the tick that was already running, for the reload after an early wake.
 *
 * Only the FIRST interval after a window is partial: the window did not begin on a boundary, so
 * restoring the full period here would silently stretch that tick and lose the phase.
 *
 * @param[in] elapsed_cycles  Cycles the window actually ran.
 * @param[in] head_cycles     Cycles that remained of the running tick when the window opened.
 * @param[in] period          Reload period in cycles, never zero.
 * @return uint32_t  Cycles until the next boundary.
 */
static inline uint32_t os_arch_tick_remaining(uint32_t elapsed_cycles,
                                              uint32_t head_cycles, uint32_t period)
{
    return (elapsed_cycles < head_cycles) ? (head_cycles - elapsed_cycles) :
           (period - ((elapsed_cycles - head_cycles) % period));
}

#ifdef __cplusplus
}
#endif

#endif /* OS_ARCH_TICK_MATH_H */
