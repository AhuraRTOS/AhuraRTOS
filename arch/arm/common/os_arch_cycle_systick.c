/**
 * @file os_arch_cycle_systick.c
 * @brief Cycle counter synthesized from SysTick, for cores and devices where DWT CYCCNT is not
 *        available.
 *
 * Textually included by the shared port implementations (os_arch_port_v6m.c, _v7m.c, _v8m.c), the
 * same way each variant's os_arch_port.c includes those - it is not a separate compilation unit and
 * must never be added to a build as one.
 *
 * Two ports need it for different reasons:
 *
 *   v6m   ARMv6-M has no DWT at all, so this is the only cycle counter that exists there.
 *   v7m   DWT is OPTIONAL on Cortex-M3/M4/M7 and on ARMv8-M, and even where the unit is present
 *   v8m   CYCCNT itself may not be (DWT_CTRL.NOCYCCNT), may be gated behind debug power, or may
 *         be locked. Those ports use CYCCNT when it really counts and fall back to here when it
 *         does not - see os_arch_cycle_count_get() in each.
 *
 * What it provides is a counter that advances monotonically in CPU cycles, which is exactly what
 * its callers need: os_delay_us(), the busy-wait path of os_delay_ms(), and the self-test suite's
 * benchmark table all measure an interval as the difference between two reads. It is not absolute
 * time - only differences mean anything, and they stay correct across the 32-bit wrap.
 *
 * WHERE THE WHOLE PERIODS COME FROM
 *
 * The SysTick down-counter alone gives a position inside one period; something has to count the
 * periods themselves. An earlier version of this file did that by watching COUNTFLAG on every call,
 * and that is subtly wrong in a way worth recording, because it looks right and passes short tests:
 * COUNTFLAG only says "it wrapped since you last looked". A period that elapses while NOBODY calls
 * this function is never credited, so two reads a few milliseconds apart could report less elapsed
 * time than one period, or - when the second read landed after a wrap - report a value LOWER than
 * the first. On the RP2040 that showed up as a counter running at 1/100 of the CPU clock and as
 * negative intervals wrapping to nearly 2^32.
 *
 * So the periods are counted in the tick interrupt instead, by os_arch_cycle_tick(), which the
 * kernel calls on EVERY core once per tick of that core's own timer. Nothing can be missed, because
 * nothing has to be caught.
 *
 * Per core, and that part is not optional. Every core runs its own SysTick, started when that core
 * entered the scheduler, so their periods do not share a phase - while os_tick_count belongs to
 * core 0 alone (see os_tick_handler). Pairing core 0's tick with core 1's down-counter gives the
 * right average rate with a sawtooth of up to a whole period on top, which is exactly the kind of
 * number that looks plausible and is not.
 *
 * THE LAG, AND WHY COUNTFLAG CANNOT CLOSE IT
 *
 * One window remains: between the hardware wrap and the interrupt that records it, os_tick_get() is
 * still one period behind while the down-counter has already restarted at the top. Reading straight
 * through that gives a value a whole period too LOW.
 *
 * COUNTFLAG looks like the answer and is not. It is set by the wrap and cleared only by a read of
 * SYST_CSR - not by the handler running - so once a wrap has happened it stays set until somebody
 * reads it, long after the tick was counted. Using it added a period that had already been counted,
 * and the next call (which found the flag cleared, because the previous call consumed it) dropped
 * it again: the counter stepped BACKWARDS by a period on alternating reads, which is worse than the
 * bug it was meant to fix.
 *
 * ICSR.PENDSTSET is the signal that actually means it. The hardware sets it on the wrap and clears
 * it when the SysTick handler is ENTERED, so it reads as "a tick is owed and has not been counted",
 * which is exactly the correction needed - and reading it changes nothing.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Cycles credited per call while SysTick is not running - before os_tick_init(), or for the whole
 * run with OS_CONFIG_TICK_SOURCE_EXTERNAL, where SysTick may never be started at all. Deliberately
 * well below the real cost of one call so a busy-wait driven by it only ever comes out LONGER than
 * asked for. It exists so those waits terminate rather than being accurate. */
#define OS_ARCH_CYCLE_FALLBACK_STEP          8U

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/* Whole SysTick periods this core has seen, counted by os_arch_cycle_tick() below. */
static uint32_t os_arch_cycle_periods[OS_CONFIG_CORE_COUNT];

/* Software-only count, used while SysTick is stopped, and kept as the base afterwards so that
 * starting SysTick cannot make the counter jump backwards. */
static uint32_t os_arch_cycle_fallback[OS_CONFIG_CORE_COUNT];

/* Last value handed out on each core, so the counter can be held rather than go backwards. */
static uint32_t os_arch_cycle_last[OS_CONFIG_CORE_COUNT];

/** Raw counter value when a tickless window opened, per core. */
static uint32_t os_arch_cycle_window_mark[OS_CONFIG_CORE_COUNT];

/******************************************************************************************************/
/**
 * @brief The counter's true position, without the monotonic clamp.
 *
 * The clamp exists to protect CALLERS, who subtract two reads. Bracketing a window has to see where
 * the counter really is, held value or not, or the correction below would cancel the wrong amount.
 *
 * Interrupts must already be masked by the caller: this is a read-modify-free sample of two
 * registers plus an accumulator, and a tick landing between them pairs a position with the wrong
 * period count.
 *
 * @return uint32_t  Unclamped counter value.
 */
static uint32_t os_arch_cycle_raw_get(void)
{
    uint32_t core   = os_arch_core_id_get();
    uint32_t reload = OS_ARCH_REG_SYST_RVR & OS_ARCH_SYST_RVR_RELOAD_MSK;
    uint32_t cvr    = OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK;

    return os_arch_cycle_fallback[core] + (os_arch_cycle_periods[core] * (reload + 1U)) +
           (reload - cvr);
}

/******************************************************************************************************/
/**
 * @brief Mark the counter's position as a tickless window opens.
 *
 * @return None.
 */
void os_arch_cycle_window_open(void)
{
    uint32_t primask = os_arch_primask_get();

    OS_ARCH_IRQ_DISABLE();

    os_arch_cycle_window_mark[os_arch_core_id_get()] = os_arch_cycle_raw_get();

    if (primask == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Close the window, having the counter advance by exactly `elapsed` tick periods.
 *
 * Not "credit elapsed wraps": the wrap count inside a window cannot be known here, and guessing it
 * from a tick count measured by another clock is what drifted. This measures whatever the
 * free-running position actually did, cancels it, and puts the intended advance in its place - so
 * the counter and the kernel clock come out of every window agreeing exactly.
 *
 * @param[in] elapsed  Whole tick periods the kernel is about to announce for this window.
 * @return None.
 */
void os_arch_cycle_window_close(uint32_t elapsed)
{
    uint32_t primask = os_arch_primask_get();

    OS_ARCH_IRQ_DISABLE();

    {
        uint32_t core    = os_arch_core_id_get();
        uint32_t reload  = OS_ARCH_REG_SYST_RVR & OS_ARCH_SYST_RVR_RELOAD_MSK;
        uint32_t drifted = os_arch_cycle_raw_get() - os_arch_cycle_window_mark[core];
        uint32_t wanted  = elapsed * (reload + 1U);

        /* Unsigned throughout and deliberately so: both terms wrap at 2^32 exactly as the counter
         * does, so the correction stays right across the counter's own wrap. */
        os_arch_cycle_fallback[core] += (wanted - drifted);
    }

    if (primask == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }
}

/*
 * ***********************************************************************************************************
 * Function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Read the SysTick-derived cycle count. See the file header for what it does and does not
 *        guarantee.
 *
 * @return uint32_t  Current cycle count.
 */
static uint32_t os_arch_cycle_systick_get(void)
{
    uint32_t primask = os_arch_primask_get();
    uint32_t core;
    uint32_t reload;
    uint32_t csr;
    uint32_t value;

    /* The accumulator is read-modify-written against a down-counter that keeps running, so the
     * whole sample has to be indivisible - a tick landing in the middle would double-count or
     * skip a wrap. PRIMASK rather than the kernel mask: this must also be correct when called
     * from an interrupt above OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY. */
    OS_ARCH_IRQ_DISABLE();

    core   = os_arch_core_id_get();
    reload = OS_ARCH_REG_SYST_RVR & OS_ARCH_SYST_RVR_RELOAD_MSK;

    /* Only the enable bit is wanted. Reading CSR also clears COUNTFLAG, which nothing depends on
     * any more - see the file header for why that flag is not the wrap signal it appears to be. */
    csr = OS_ARCH_REG_SYST_CSR;

    if (((csr & OS_ARCH_SYST_CSR_ENABLE_MSK) == 0U) || (reload == 0U))
    {
        os_arch_cycle_fallback[core] += OS_ARCH_CYCLE_FALLBACK_STEP;
        value = os_arch_cycle_fallback[core];
    }
    else
    {
        uint32_t cvr;
        uint32_t pending;
        uint32_t recheck;
        uint32_t periods;

        /* Sample the down-counter and the pending bit as one consistent pair. Interrupts are
         * masked, but the COUNTER is not: a wrap landing between the two reads would pair a
         * position from the old period with a pending bit from the new one, and the value would be
         * a whole period out. A down-counter that reads HIGHER the second time is exactly that
         * wrap, and the retry costs three register reads. */
        do
        {
            cvr     = OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK;
            pending = OS_ARCH_REG_ICSR & OS_ARCH_ICSR_PENDSTSET_MSK;
            recheck = OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK;
        } while (recheck > cvr);

        periods = os_arch_cycle_periods[core] + ((pending != 0U) ? 1U : 0U);

        /* The fallback base is carried over so the first read after SysTick starts cannot be lower
         * than the last read before it. */
        value = os_arch_cycle_fallback[core] + (periods * (reload + 1U)) + (reload - cvr);

        /* Belt and braces, and deliberately kept even though the pair above should make it
         * unnecessary: callers subtract two reads and interpret the result as unsigned, so a single
         * backwards step does not read as a small error - it reads as roughly 2^32. Holding the
         * previous value turns any residual corner into a slightly UNDERCOUNTED interval, which is
         * wrong in a way that can be noticed and reasoned about rather than one that produces
         * nonsense. The comparison is signed on the difference, so the counter's own wrap at 2^32
         * passes through untouched. */
        if ((int32_t)(value - os_arch_cycle_last[core]) < 0)
        {
            value = os_arch_cycle_last[core];
        }

        os_arch_cycle_last[core] = value;
    }

    if (primask == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }

    return value;
}

/******************************************************************************************************/
/**
 * @brief Reset the synthesized counter's state. Called from os_arch_init().
 *
 * @return None.
 */
static void os_arch_cycle_systick_reset(void)
{
    uint32_t core = os_arch_core_id_get();

    /* Per core, and called from os_arch_init(), which every core runs as it enters the scheduler,
     * so each clears its own slot and none disturbs another's. */
    os_arch_cycle_periods[core]  = 0U;
    os_arch_cycle_fallback[core] = 0U;
    os_arch_cycle_last[core]     = 0U;
}

/******************************************************************************************************/
/**
 * @brief Close one SysTick period on this core. Called by the kernel from every core's tick.
 *
 * @return None.
 */
void os_arch_cycle_tick(void)
{
    os_arch_cycle_periods[os_arch_core_id_get()]++;
}
