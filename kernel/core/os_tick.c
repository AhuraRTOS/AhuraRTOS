/**
 * @file os_tick.c
 * @brief Kernel tick management implementation.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "os_internal.h"

/* OS_WEAK comes from the port layer (os_arch_port_common.h). */

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static __IO uint32_t os_tick_count = 0U;

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
/* CPU load sampling: every tick counts once, and additionally as idle when
 * it interrupted the idle task. os_cpu_usage_get consumes and resets both. */
static __IO uint32_t os_tick_usage_total_ticks = 0U;
static __IO uint32_t os_tick_usage_idle_ticks  = 0U;
#endif

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize system tick source and bookkeeping.
 *
 * @return None.
 */
void os_tick_init(void)
{
    os_tick_count = 0U;
    os_arch_tick_init();
}

/******************************************************************************************************/
/**
 * @brief Get the kernel tick counter (wraps at 32 bits).
 *
 * @return uint32_t  Current tick count.
 */
uint32_t os_tick_get(void)
{
    return os_tick_count;
}

/******************************************************************************************************/
/**
 * @brief Handle periodic tick events. Call from the tick interrupt.
 *
 * @return None.
 */
void os_tick_handler(void)
{
    /* Core 0 owns the kernel time base (delays, timers, work): a tick on any other core only
     * drives that core's preemption and round-robin, or elapsed time would be counted once per
     * core. Always true on a single-core build, where the whole question compiles away.
     *
     * Held in a flag rather than branching with an early return, so this function has one exit
     * (MISRA Rule 15.5) and the preempt check at the bottom - which both paths need, identically -
     * is written once. */
    bool owns_time_base = true;

#if (OS_CONFIG_CORE_COUNT > 1U)
    owns_time_base = (os_arch_core_id_get() == 0U);
#endif

    if (owns_time_base)
    {
        os_tick_count++;

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
        os_tick_usage_total_ticks++;
        if (os_task_current_is_idle())
        {
            os_tick_usage_idle_ticks++;
        }
#endif

#if (OS_CONFIG_WORK_ENABLE == 1U)
        os_work_tick_process(1U);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
        os_timer_tick_process(1U);
#endif
        os_task_tick_update(1U);
    }

    /* This core's own round-robin quantum is counted down whichever core it is:
     * only the kernel time base belongs exclusively to core 0. */
    os_task_slice_tick(1U);

    /* Pend PendSV only when it would actually do something: a wake this tick
     * (work/timer/delay expiry) or an equal-priority peer whose turn has come
     * both show up in os_task_reschedule_possible, which also answers false
     * while the scheduler is locked or the running task still has time slice
     * left - so a tick that would not switch costs one bitmap check instead
     * of a full PendSV round trip. PendSV is the lowest priority, so a real
     * one still runs after all pending interrupts complete. */
    if (os_kernel_is_running() && os_task_reschedule_possible())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }
}

/******************************************************************************************************/
/**
 * @brief Announce elapsed ticks to kernel time base (tickless wakeup path).
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks since previous update.
 * @return None.
 */
void os_tick_announce(uint32_t elapsed_ticks)
{
    /* Unlike os_tick_handler this runs in task context (tickless idle), so
     * the counter updates are guarded against a concurrent tick interrupt. */
    uint32_t mask_state = os_arch_kernel_mask_save();

    os_tick_count += elapsed_ticks;

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
    /* Announced ticks elapsed during a tickless sleep: idle by definition. */
    os_tick_usage_total_ticks += elapsed_ticks;
    os_tick_usage_idle_ticks  += elapsed_ticks;
#endif

    os_arch_kernel_mask_restore(mask_state);

#if (OS_CONFIG_WORK_ENABLE == 1U)
    os_work_tick_process(elapsed_ticks);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    os_timer_tick_process(elapsed_ticks);
#endif
    os_task_tick_update(elapsed_ticks);
    os_task_slice_tick(elapsed_ticks);

    if (os_kernel_is_running() && os_task_reschedule_possible())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }
}

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the CPU usage in percent since the previous call (and restart the window).
 *
 * A tick counts as busy when it interrupted anything but the idle task, so
 * the resolution is one tick: call at a period well above the tick period
 * (e.g. once per second at 1 kHz tick). Returns 0 before the first tick.
 *
 * @return uint32_t  CPU usage 0..100.
 */
uint32_t os_cpu_usage_get(void)
{
    uint32_t total_ticks;
    uint32_t idle_ticks;
    uint32_t usage_percent = 0U;

    os_critical_enter();

    total_ticks = os_tick_usage_total_ticks;
    idle_ticks  = os_tick_usage_idle_ticks;

    os_tick_usage_total_ticks = 0U;
    os_tick_usage_idle_ticks  = 0U;

    os_critical_exit();

    /* 0 before the first tick of a window, which is also what the division could
     * not produce. */
    if (total_ticks != 0U)
    {
        if (idle_ticks > total_ticks)
        {
            idle_ticks = total_ticks;
        }

        usage_percent = ((total_ticks - idle_ticks) * 100U) / total_ticks;
    }

    return usage_percent;
}
#endif /* OS_CONFIG_CPU_USAGE_ENABLE */

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Get expected idle ticks for tickless decision.
 *
 * The minimum of the next software-timer expiry, the next ready work item, the next
 * finite-delay task sleeper, and OS_CONFIG_MAX_SUPPRESSED_TICKS - the suppressed window
 * must not overrun any of them. Also public (ahura.h) for diagnostics and tests.
 *
 * @return uint32_t  Expected idle duration in ticks.
 */
uint32_t os_tickless_expected_idle_ticks_get(void)
{
    /* The suppressed-tick window must not overrun ANY kernel time source:
     * the earliest software timer expiry, the earliest delayed work item,
     * and the earliest finite-delay task sleeper all bound it. */
    uint32_t idle_ticks = OS_CONFIG_MAX_SUPPRESSED_TICKS;
    uint32_t candidate;

#if (OS_CONFIG_TIMER_ENABLE == 1U)
    candidate = os_timer_next_expiry_ticks_get();
    if (candidate < idle_ticks)
    {
        idle_ticks = candidate;
    }
#endif

#if (OS_CONFIG_WORK_ENABLE == 1U)
    candidate = os_work_next_ready_ticks_get();
    if (candidate < idle_ticks)
    {
        idle_ticks = candidate;
    }
#endif

    candidate = os_task_next_delay_ticks_get();
    if (candidate < idle_ticks)
    {
        idle_ticks = candidate;
    }

    return idle_ticks;
}

/******************************************************************************************************/
/**
 * @brief Maximum ticks the active arch port can suppress in one tickless window.
 *
 * Register-width limited (e.g. SysTick's 24-bit reload), so it depends on both the platform
 * clock and OS_CONFIG_TICK_HZ rather than being a fixed constant. Callers that must work
 * across platforms and clock speeds (tests, demos) derive their sleep horizon from this
 * instead of assuming any particular tick count.
 *
 * @return uint32_t  Maximum suppressible ticks; 0 when the active port does not yet suppress
 *                    ticking for real (see README "Tickless idle" for which ports currently do).
 */
uint32_t os_tickless_max_suppressed_ticks_get(void)
{
    return os_arch_max_suppressed_ticks_get();
}

/******************************************************************************************************/
/**
 * @brief Execute tickless idle flow.
 *
 * Suppresses ticking for the planned idle duration, sleeps, then announces the real elapsed
 * time on wake. Not yet called by the idle task (see the README "Tickless idle" section) -
 * public in ahura.h so it can be exercised directly (e.g. by the self-test suite) ahead of
 * that wiring.
 *
 * @return None.
 */
void os_tickless_idle_process(void)
{
    uint32_t mask_state;
    uint32_t planned_idle_ticks;
    uint32_t elapsed_ticks = 0U;

    /* Core 0 owns the kernel time base, the same rule os_tick_handler enforces. Announcing a
     * suppressed window from another core would add its idle time to counters core 0's tick
     * interrupt is already advancing, so every sleep would be counted twice and the clock would
     * run fast. Other cores still idle, they just do it in a plain WFI without announcing.
     *
     * A flag rather than an early return, for the single exit (MISRA Rule 15.5); always true on a
     * single-core build, where the test compiles away entirely. */
    bool owns_time_base = true;

#if (OS_CONFIG_CORE_COUNT > 1U)
    owns_time_base = (os_arch_core_id_get() == 0U);
#endif

    if (owns_time_base)
    {
        /* Interrupts off BEFORE deciding how long to sleep, and kept off until the sleep has been
         * accounted for.
         *
         * Every input to that decision - the next timer expiry, the next ready work item, the earliest
         * sleeping task - is something an ISR can change. Reading them with interrupts live leaves a
         * window in which an ISR registers a nearer deadline than the one just computed, and the sleep
         * then runs straight past it. Waking a task in that window is harmless, because that pends
         * PendSV and a pending exception cuts the WFI short, but starting a timer or submitting
         * delayed work pends nothing at all: there would be no wake-up event, and the timer would fire
         * late by the whole remaining window.
         *
         * Masking first closes it. A WFI still wakes on a pending interrupt while masked, so anything
         * arriving from here on shortens the sleep rather than being missed. */
        mask_state = os_arch_kernel_mask_save();

        planned_idle_ticks = os_tickless_expected_idle_ticks_get();

        /* Too short to be worth suppressing: the mask is handed straight back and this
         * idle pass behaves like a plain WFI. */
        if (planned_idle_ticks >= OS_CONFIG_TICKLESS_MIN_IDLE)
        {
            os_tickless_pre_sleep_cb();

            OS_ARCH_SLEEP(planned_idle_ticks);

            /* Wake path, in this order for a reason: measure while the counter still holds the sleep,
             * let the application restore its hardware, announce so the clock catches up, and only
             * then release the mask. Announcing after the release, or before the restore, both break -
             * os_tick_count is short by the whole sleep until step 3, and the switch os_tick_announce
             * can pend would otherwise be taken while the idle task still has SLEEPDEEP set. */
            elapsed_ticks = os_arch_elapsed_ticks_get();

            os_tickless_post_sleep_cb();
            os_tick_announce(elapsed_ticks);
            os_arch_sleep_finish();
        }

        /* Releases the mask taken before the sleep was planned. Nesting is deliberate: the port's own
         * mask (taken in os_arch_sleep_prepare, released by os_arch_sleep_finish above) sits inside
         * this one, and both are save/restore rather than unconditional enables, so the interrupt
         * state the idle task arrived with is what it leaves with. */
        os_arch_kernel_mask_restore(mask_state);
    }
}

/* os_tickless_pre_sleep_cb() and os_tickless_post_sleep_cb() are deliberately NOT defined here.
 *
 * They describe the board, not the kernel: which sleep mode to enter, which clocks to gate, which
 * peripherals must be flushed or quiesced first. A weak empty default in the kernel would let a
 * project enable OS_CONFIG_TICKLESS_ENABLE and link cleanly while silently doing none of that -
 * and on a part whose HAL runs its own tick source, an unwritten pre-sleep hook cuts every
 * suppressed sleep short at that source's period, which looks like tickless idle simply not
 * working. Requiring real definitions turns that into a link error naming the missing function.
 *
 * Write them in the application's callback file (see template/os_cb.c) whenever tickless idle is
 * enabled. */

#endif /* OS_CONFIG_TICKLESS_ENABLE */
