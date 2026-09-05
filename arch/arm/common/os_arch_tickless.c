/**
 * @file os_arch_tickless.c
 * @brief Tickless idle for Arm ports whose own tick timer cannot safely be suppressed.
 *
 * Textually included by the shared port implementations, the same way os_arch_cycle_systick.c and
 * os_arch_atomic.c are - it is not a separate compilation unit and must never be added to a build
 * as one.
 *
 * There are two ways to suppress a tick, and which one a port can use is decided by something
 * outside the tick: whether the port has a cycle counter that does not come FROM the tick.
 *
 *   DWT present     Reprogram SysTick's reload for the window. os_arch_port_v8m.c does this, and
 *                   it is the cheaper answer because it needs no other hardware at all.
 *   DWT absent      Reprogramming the reload would break os_arch_cycle_systick.c, which counts
 *                   whole SysTick periods in the tick interrupt and multiplies them by the reload
 *                   it reads live - and that counter is what os_delay_us() and the busy-wait half
 *                   of os_delay_ms() run on. ARMv6-M has no DWT at all, so this is not an edge
 *                   case there, it is the only case.
 *
 * This file is the second answer. SysTick's reload is never touched; only its INTERRUPT is masked
 * for the window, and an independent timer the SoC package owns is what ends it. The counter keeps
 * running at its usual cadence, so the tick grid keeps its phase and the cycle counter keeps its
 * arithmetic - all that is missed are the interrupts, and those are handed back as a count at the
 * end.
 *
 * Which timer that is stays entirely inside the SoC package: an alarm on the RP2040's always-on
 * microsecond timer, an LPTIM or RTC on an STM32, whatever a future part offers. Nothing above the
 * three callbacks below names one, which is what lets the same code serve a part nobody has ported
 * to yet.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/** Ticks the current window was opened for, and 0 when none is open. */
static uint32_t os_arch_tickless_planned = 0U;

/** SysTick CSR as it was before the window masked the tick interrupt. */
static uint32_t os_arch_tickless_saved_csr = 0U;

/*
 * ***********************************************************************************************************
 * SoC callbacks - weak defaults
 * ***********************************************************************************************************
 *
 * A package that supplies none of these suppresses nothing: os_arch_max_suppressed_ticks_get()
 * answers 0, the kernel still runs its whole tickless pass, still honours every deadline and still
 * calls the application's sleep hooks, and the sleep itself is the plain WFI the idle task did
 * before. Nothing is claimed that is not delivered.
*/

/******************************************************************************************************/
/**
 * @brief Weak default: this SoC package has no timer to suppress with.
 *
 * @return uint32_t  0.
 */
OS_WEAK uint32_t os_arch_tick_suppress_max_cb(void)
{
    return 0U;
}

/******************************************************************************************************/
/**
 * @brief Weak default: this package has nothing to say about how short a window may be.
 *
 * @return uint32_t  0.
 */
OS_WEAK uint32_t os_arch_tick_suppress_min_cb(void)
{
    return 0U;
}

/******************************************************************************************************/
/**
 * @brief Weak default: nothing to arm.
 *
 * @param[in] ticks  Ignored.
 * @return None.
 */
OS_WEAK void os_arch_tick_suppress_cb(uint32_t ticks)
{
    (void)ticks;
}

/******************************************************************************************************/
/**
 * @brief Weak default: no window was opened, so none elapsed.
 *
 * @return uint32_t  0.
 */
OS_WEAK uint32_t os_arch_tick_resume_cb(void)
{
    return 0U;
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Largest number of ticks this port can suppress in one window.
 *
 * Whatever the SoC package can wake from, since the window is its timer's to measure. SysTick's
 * 24-bit reload does not bound anything here - it is never reprogrammed.
 *
 * @return uint32_t  Ceiling in ticks; 0 when the package supplies no timer, and a sleep then saves
 *                    latency rather than power.
 */
uint32_t os_arch_max_suppressed_ticks_get(void)
{
    return os_arch_tick_suppress_max_cb();
}

/******************************************************************************************************/
/**
 * @brief Shortest window worth opening on this port (see os_arch_port_common.h for the contract).
 *
 * Nothing to add above what the package says: entering and leaving the sleep is a WFI, which costs
 * the same everywhere and is already well inside the two ticks the kernel insists on regardless.
 *
 * @return uint32_t  Floor on one window, in ticks; 0 when the package has no opinion.
 */
uint32_t os_arch_min_suppressed_ticks_get(void)
{
    return os_arch_tick_suppress_min_cb();
}

/******************************************************************************************************/
/**
 * @brief Open a suppressed window: silence the tick interrupt and let the SoC's timer end it.
 *
 * The COUNTER is deliberately left running. Stopping it would move the tick grid's phase and strand
 * os_arch_cycle_systick.c's period arithmetic; masking only the interrupt leaves both exactly as
 * they were, and the interrupts that do not arrive are handed back as a count by
 * os_arch_elapsed_ticks_get().
 *
 * No interrupt mask is taken here. The kernel holds its own across the whole window, and a WFI
 * wakes on a pending interrupt while masked, which is what the SoC's timer relies on.
 *
 * @param[in] planned_ticks  Tick periods the kernel expects to be idle for.
 * @return None.
 */
void os_arch_sleep_prepare(uint32_t planned_ticks)
{
    os_arch_tickless_planned = 0U;   /* not armed until the package proves it can be */

    if ((planned_ticks >= 2U) && (os_arch_tick_suppress_max_cb() != 0U))
    {
        os_arch_tickless_saved_csr = OS_ARCH_REG_SYST_CSR;
        os_arch_tickless_planned   = planned_ticks;

        /* Interrupt off, everything else as it was. Reading CSR above cleared COUNTFLAG, which
         * nothing here depends on - see os_arch_cycle_systick.c for why that flag is not the wrap
         * signal it looks like. */
        OS_ARCH_REG_SYST_CSR = os_arch_tickless_saved_csr & ~OS_ARCH_SYST_CSR_TICKINT_MSK;

        /* From here the tick interrupt is silent, so nothing feeds the cycle counter this port
         * synthesizes from it. Mark where it stands; the close puts back exactly what the window
         * costs. */
        os_arch_cycle_window_open();

        os_arch_tick_suppress_cb(planned_ticks);
    }
}

/******************************************************************************************************/
/**
 * @brief Close the window: ask how long it really was, restore the tick, and account for it.
 *
 * The cycle counter is credited with the same number of periods, because that is literally what it
 * missed: os_arch_cycle_tick() counts one per tick interrupt, and the reload those periods are
 * measured against never changed. Skipping this would leave os_delay_us() short by the whole
 * window.
 *
 * @return uint32_t  Whole tick periods that elapsed while the interrupt was masked.
 */
uint32_t os_arch_elapsed_ticks_get(void)
{
    uint32_t elapsed = 0U;

    if (os_arch_tickless_planned != 0U)
    {
        elapsed = os_arch_tick_resume_cb();

        if (elapsed > os_arch_tickless_planned)
        {
            elapsed = os_arch_tickless_planned;   /* the window was never promised more than this */
        }

        OS_ARCH_REG_SYST_CSR = os_arch_tickless_saved_csr;

        /* Exactly the elapsed the kernel is about to announce, not a wrap count guessed from it.
         * Calling os_arch_cycle_tick() once per tick was the near miss: it credits whole periods
         * of THIS timer against a count measured by the SoC's, and the leftover accumulated until
         * the counter's monotonic guard held it still. */
        os_arch_cycle_window_close(elapsed);

        os_arch_tickless_planned = 0U;
    }

    return elapsed;
}

/******************************************************************************************************/
/**
 * @brief Close out a tickless window.
 *
 * Nothing to do: os_arch_elapsed_ticks_get() already restored the tick interrupt, and no interrupt
 * mask was taken. Kept because the kernel calls it on every pass and the suppressing ports do have
 * work here.
 *
 * @return None.
 */
void os_arch_sleep_finish(void)
{
}

#endif /* OS_CONFIG_TICKLESS_ENABLE */
