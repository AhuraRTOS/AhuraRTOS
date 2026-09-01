/**
 * @file os_delay.c
 * @brief Delay service implementation: blocking tick delays and precise busy-waits.
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

#include "os_internal.h"

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_DELAY_US_PER_SECOND           1000000ULL
#define OS_DELAY_MS_PER_SECOND           1000ULL
#define OS_DELAY_MAX_CYCLE_CHUNK         0x40000000UL

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void os_delay_forever(void);
static void os_delay_ticks(uint32_t ticks);
static void os_delay_cycle_wait(uint64_t cycle_count);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Delay current execution for the requested milliseconds.
 *
 * Blocks the calling task (yields the CPU) once the scheduler is running;
 * falls back to a busy-wait before os_start or from interrupt context.
 * OS_WAIT_FOREVER parks the calling task permanently (never returns).
 *
 * Returns nothing: a delay either waits, or the request was one the platform cannot express -
 * a configuration error OS_ASSERT reports more usefully than a status nobody reads.
 *
 * @param[in] milliseconds  Delay duration in milliseconds, or OS_WAIT_FOREVER.
 * @return None.
 */
void os_delay_ms(uint32_t milliseconds)
{
    if (milliseconds == OS_WAIT_FOREVER)
    {
        os_delay_forever();
    }
    else
    {
        uint64_t ticks_u64 =
            ((uint64_t)milliseconds * (uint64_t)OS_CONFIG_TICK_HZ + (OS_DELAY_MS_PER_SECOND - 1ULL)) /
            OS_DELAY_MS_PER_SECOND;

        /* Only reachable when OS_CONFIG_TICK_HZ pushes the tick count past 32 bits, which needs a
         * duration of weeks. Clamped to as long as the kernel can represent, rather than the old
         * "report INVALID_ARG and return immediately" - of the two wrong answers, waiting far too
         * long is the one a caller notices.
         *
         * Deliberately NOT asserted: any uint32_t is a legal argument here, so a clamp is a
         * supported outcome rather than a caller's mistake. */
        if (ticks_u64 > (uint64_t)UINT32_MAX)
        {
            ticks_u64 = (uint64_t)UINT32_MAX;
        }

        os_delay_ticks((uint32_t)ticks_u64);
    }
}

/******************************************************************************************************/
/**
 * @brief Busy-wait for the requested microseconds (precise, does not yield).
 *
 * Uses the DWT cycle counter; intended for short, precise waits. Prefer
 * os_delay_ms for anything at or above the tick period.
 *
 * @param[in] microseconds  Delay duration in microseconds.
 * @return None.
 */
/** Tick periods the measurement spans. Both ends are interrupts at the tick rate, so the window is
 *  exact by construction and this only has to be long enough that the counter read inside the ISR
 *  is noise against it. Nothing waits for these ticks; they pass anyway. */
#define OS_DELAY_CALIBRATE_TICKS    8U

/** Measured rate of the cycle counter, or 0 while it has not been measured. Deliberately NOT
 *  initialised from the platform clock: 0 is what lets os_cycle_hz_get() tell "not measured" from
 *  "measured and happens to equal the core clock", which is every part but one so far.
 *
 *  Written by the tick interrupt, read by tasks, so volatile. A 32-bit aligned word cannot tear,
 *  and it goes from 0 to its final value exactly once, so no lock is needed on either side. */
static __IO uint32_t os_delay_cycle_hz  = 0U;

/** Cycle counter as it read on the opening tick of the measurement. */
static uint32_t      os_delay_cal_cycles = 0U;

/** Ticks seen so far, 0 before the opening one. Tick interrupt only. */
static uint32_t      os_delay_cal_ticks  = 0U;

/******************************************************************************************************/
/**
 * @brief Accumulate the cycle-counter measurement. Called from the tick interrupt, once per tick.
 *
 * The tick interrupt is used rather than a busy-wait because it is the one place that runs at the
 * tick rate BY DEFINITION. A loop that waits for the tick instead depends on the tick already
 * advancing, which depends in turn on the application's startup order and interrupt state - and
 * when that assumption is wrong the measurement is simply never made, silently, which is how a
 * counter running at half the core clock went on being reported as agreeing with it.
 *
 * Costs one comparison per tick once the measurement is complete.
 *
 * @return None.
 */
void os_delay_calibrate_sample(void)
{
    if (os_delay_cycle_hz == 0U)
    {
        uint32_t now;

        /* A counter built FROM the tick already has an exactly known rate, so measuring it gains
         * nothing - and costs something real, because the read below lands inside the tick
         * interrupt where such a counter is momentarily inconsistent. Settle it from the platform
         * clock, which for that kind IS the rate, and never sample again. */
        if (!os_arch_cycle_is_independent())
        {
            os_delay_cycle_hz = os_arch_clock_hz_get();
        }
        else
        {
            now = os_arch_cycle_count_get();

            if (os_delay_cal_ticks == 0U)
            {
                /* First tick seen: this is the opening edge, and it is a real one - an interrupt, not
                 * a polled guess at where the boundary was. */
                os_delay_cal_cycles = now;
                os_delay_cal_ticks  = 1U;
            }
            else
            {
                os_delay_cal_ticks++;

                /* The opening sample was tick 1, so tick N+1 closes exactly N whole periods. */
                if (os_delay_cal_ticks > OS_DELAY_CALIBRATE_TICKS)
                {
                    uint32_t elapsed = now - os_delay_cal_cycles;

                    os_delay_cycle_hz = (uint32_t)(((uint64_t)elapsed * (uint64_t)OS_CONFIG_TICK_HZ) /
                                                   (uint64_t)OS_DELAY_CALIBRATE_TICKS);
                }
            }
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Rate the cycle counter advances at, which is not always the core clock.
 *
 * Use this, not os_arch_clock_hz_get(), for anything converting between cycles and time: the two
 * differ wherever the counter is clocked separately from the core, and on such a part the core
 * clock produces an answer that is wrong by exactly that ratio.
 *
 * @return uint32_t  Measured rate in Hz; the platform clock while no measurement was possible.
 */
uint32_t os_cycle_hz_get(void)
{
    return (os_delay_cycle_hz != 0U) ? os_delay_cycle_hz : os_arch_clock_hz_get();
}

void os_delay_us(uint32_t microseconds)
{
    uint32_t clock_hz = os_cycle_hz_get();

    /* No clock reading means no way to measure a microsecond, so this cannot wait at all. The
     * platform's clock callback is not answering - see doc/porting.md "Platform clock". */
    OS_ASSERT((microseconds == 0U) || (clock_hz != 0U));

    if ((microseconds != 0U) && (clock_hz != 0U))
    {
        uint64_t cycle_count =
            ((uint64_t)microseconds * (uint64_t)clock_hz + (OS_DELAY_US_PER_SECOND - 1ULL)) /
            OS_DELAY_US_PER_SECOND;

        os_delay_cycle_wait(cycle_count);
    }
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Park the calling task permanently (OS_WAIT_FOREVER delay). Never returns when it can.
 *
 * @return None.
 */
static void os_delay_forever(void)
{
    /* Nothing to park from an ISR or before the scheduler runs, and a permanent busy-wait there
     * would hang the system outright - so this returns at once instead, which is the one case
     * where a "forever" delay does not. */
    OS_ASSERT(os_internal_can_block());

    /* Re-arm on any spurious wake (forced os_task_wake aimed at a kernel
     * service task): forever really is forever. The loop condition doubles as
     * the can-block guard, so the ISR/pre-scheduler case falls straight out
     * without a second exit. */
    while (os_internal_can_block())
    {
        os_task_sleep_ticks(OS_WAIT_FOREVER);
    }
}

/******************************************************************************************************/
/**
 * @brief Delay execution by a finite number of scheduler ticks: block when possible,
 *        busy-wait otherwise.
 *
 * @param[in] ticks  Number of ticks to delay.
 * @return None.
 */
static void os_delay_ticks(uint32_t ticks)
{
    /* MISRA Rule 17.8 (do not modify a parameter): the sentinel adjustment below works
     * on a local copy rather than on ticks itself. */
    uint32_t wait_ticks = ticks;

    if (wait_ticks != 0U)
    {
        /* The callers route an intentional OS_WAIT_FOREVER to os_delay_forever
         * before converting, so the sentinel value can only be reached here as
         * a FINITE duration whose tick conversion collides with it numerically.
         * One tick short keeps it out of the sleep primitive's "until woken"
         * meaning at a cost of 1 tick in ~49 days (at 1 kHz). */
        if (wait_ticks == OS_WAIT_FOREVER)
        {
            wait_ticks--;
        }

        /* Preferred path: yield the CPU to other tasks until the delay expires.
         * The sleep is re-armed until the duration has really elapsed: a forced
         * os_task_wake aimed at a kernel service task (a new timer expiry
         * while its handler delays) must not cut the delay short. */
        if (os_internal_can_block())
        {
            uint32_t start_tick = os_tick_get();
            uint32_t elapsed    = 0U;

            while (elapsed < wait_ticks)
            {
                os_task_sleep_ticks(wait_ticks - elapsed);
                elapsed = os_tick_get() - start_tick; /* wrap-safe unsigned diff */
            }
        }
        else
        {
            /* Pre-scheduler or interrupt context: precise busy-wait. Cycles again, so the
             * counter's own rate rather than the core's. */
            uint32_t clock_hz = os_cycle_hz_get();

            /* Same as os_delay_us: without a clock reading there is no way to time the wait, so
             * this delays not at all. The platform's clock callback is not answering. */
            OS_ASSERT(clock_hz != 0U);

            if (clock_hz != 0U)
            {
                uint64_t cycle_count =
                    ((uint64_t)wait_ticks * (uint64_t)clock_hz) / (uint64_t)OS_CONFIG_TICK_HZ;

                os_delay_cycle_wait(cycle_count);
            }
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Busy wait for a cycle-count duration using the DWT cycle counter.
 *
 * @param[in] cycle_count  Number of core cycles to wait.
 * @return None.
 */
static void os_delay_cycle_wait(uint64_t cycle_count)
{
    /* Chunked so 32-bit counter wraparound stays unambiguous. */
    while (cycle_count > 0ULL)
    {
        uint32_t chunk = (cycle_count > (uint64_t)OS_DELAY_MAX_CYCLE_CHUNK) ?
                         OS_DELAY_MAX_CYCLE_CHUNK : (uint32_t)cycle_count;
        uint32_t start = os_arch_cycle_count_get();

        while ((uint32_t)(os_arch_cycle_count_get() - start) < chunk)
        {
        }

        cycle_count -= (uint64_t)chunk;
    }
}
