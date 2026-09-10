/* Pure integer timing arithmetic shared by the Cortex-M tickless paths. */
#ifndef OS_ARCH_TICK_MATH_H
#define OS_ARCH_TICK_MATH_H
#include <stdint.h>

/* SysTick continues on its original grid. Counter position supplies the exact
 * fractional phase; the independent timer only disambiguates whole wraps.
 * Reference/snapshot error must be less than half a SysTick period. */
static inline uint32_t os_arch_tick_wraps(uint64_t reference_cycles,
                                         uint32_t start_cvr, uint32_t end_cvr,
                                         uint32_t period)
{
    /* SysTick pends on the transition to zero, one cycle BEFORE reload.
     * Give zero the new period's phase so the boundary belongs to that IRQ. */
    start_cvr = (start_cvr == 0U) ? period : start_cvr;
    end_cvr = (end_cvr == 0U) ? period : end_cvr;
    int64_t whole = (int64_t)reference_cycles + (int64_t)end_cvr - (int64_t)start_cvr;
    return (whole <= 0) ? 0U : (uint32_t)(((uint64_t)whole + period / 2U) / period);
}

/* Preserve the position within the current tick after an early wake. */
static inline uint32_t os_arch_tick_remaining(uint32_t elapsed_cycles,
                                             uint32_t head_cycles, uint32_t period)
{
    return (elapsed_cycles < head_cycles) ? (head_cycles - elapsed_cycles) :
           (period - ((elapsed_cycles - head_cycles) % period));
}
#endif
