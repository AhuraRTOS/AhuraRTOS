/**
 * @file os_tick.c
 * @brief Kernel tick management implementation.
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

/* OS_WEAK comes from the port layer (os_arch_port_common.h). */

/** The application's floor in ticks, from the milliseconds it states.
 *
 * Ceiling division, and never below 2. Rounding down could reach 0, and a floor of 0 means
 * "always suppress" - the opposite of what the option is for. Two tick periods is the hardware
 * floor underneath any policy: one is what the tick would have done anyway. */
#define OS_TICKLESS_MIN_IDLE_TICKS                                                                \
    ((((OS_CONFIG_TICKLESS_MIN_IDLE_MS * OS_CONFIG_TICK_HZ) + 999UL) / 1000UL) < 2UL ?             \
     2UL : (((OS_CONFIG_TICKLESS_MIN_IDLE_MS * OS_CONFIG_TICK_HZ) + 999UL) / 1000UL))

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
    /* Core 0 owns the kernel time base (delays, timers): a tick on any other core only
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

#if (OS_CONFIG_TIMER_ENABLE == 1U)
        os_timer_tick_process(1U);
#endif
        os_task_tick_update(1U);
    }

    /* This core's own round-robin quantum is counted down whichever core it is:
     * only the kernel time base belongs exclusively to core 0. */
    os_task_slice_tick(1U);

    /* Same reasoning, for the port's cycle counter. A port that synthesizes one from SysTick needs
     * to know how many times THIS core's timer has wrapped, and os_tick_count cannot tell it -
     * that belongs to core 0 alone, while every core runs its own SysTick on its own phase. This
     * call is the one moment each core knows its own timer just wrapped. Compiles to nothing on
     * ports with a real cycle counter in hardware. */
    os_arch_cycle_tick();

    /* Pend PendSV only when it would actually do something: a wake this tick
     * (timer/delay expiry) or an equal-priority peer whose turn has come
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

/* Tickless idle is single-core only, for now.
 *
 * os_tick_handler above gives core 0 sole ownership of the kernel time base, so suppressing core
 * 0's tick stops delays and timers for EVERY core. os_tickless_idle_process masks interrupts before
 * it plans a window, which closes the race against this core's own ISRs and does nothing at all
 * about another core: core 1 keeps running tasks and can arm a timeout inside a window core 0 has
 * already committed to sleeping through. The four-worker SMP soak shows it as workers that never
 * finish, not as anything that looks like a clock problem.
 *
 * Asking whether the other cores are idle does not fix it - a core can take work the instant after
 * it answers. What does is the core creating the nearer deadline nudging core 0 out of its window,
 * which is a change to the delay-insert path and not yet written.
 *
 * A compile error rather than a silent degrade, for the same reason the sleep hooks are a link
 * error: a build that enables tickless and quietly gets nothing is the outcome worth preventing. */
/** Tick at which a window was last planned, so the idle loop walks the deadline lists once per
 *  tick rather than once per pass. */
static __IO uint32_t os_tickless_last_plan_tick = 0U;

#if (OS_CONFIG_CORE_COUNT > 1U)
/** Raised by core 0 from the moment it starts planning a window until that window has closed.
 *
 *  Read by the other cores, which is the whole point: core 0 cannot know about a deadline that
 *  does not exist yet, so whoever creates one has to say so. */
static __IO bool os_tickless_window_open = false;
#endif

/******************************************************************************************************/
/**
 * @brief Get expected idle ticks for tickless decision.
 *
 * The minimum of the next software-timer expiry, the next finite-delay task sleeper, and
 * the suppressed window must not overrun any of them.
 * Also public (ahura.h) for diagnostics and tests.
 *
 * @return uint32_t  Expected idle duration in ticks.
 */
uint32_t os_tickless_expected_idle_ticks_get(void)
{
    /* The suppressed-tick window must not overrun ANY kernel time source:
     * the earliest software timer expiry and the earliest finite-delay task
     * sleeper both bound it.
     *
     * Starts unbounded. This answers how long the KERNEL has no work, which is a fact about the
     * workload and nothing to do with what the timer hardware can count to - that ceiling is the
     * port's, and os_tickless_idle_process applies it to the window it actually arms. */
    uint32_t idle_ticks = UINT32_MAX;
    uint32_t candidate;

#if (OS_CONFIG_TIMER_ENABLE == 1U)
    candidate = os_timer_next_expiry_ticks_get();
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
 *                    ticking for real (see doc/porting.md "Tickless idle" for which ports currently do).
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
 * time on wake. Not yet called by the idle task (see doc/porting.md "Tickless idle" section) -
 * public in ahura.h so it can be exercised directly (e.g. by the self-test suite) ahead of
 * that wiring.
 *
 * @return None.
 */
void os_tickless_idle_process(void)
{
    uint32_t mask_state;
    uint32_t planned_idle_ticks;
    uint32_t suppress_ceiling;
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

    /* At most one planning pass per tick, and this is not an optimisation - it is what keeps the
     * idle loop from starving the other core.
     *
     * os_tickless_expected_idle_ticks_get walks the delay list and the timer list, both under the
     * cross-core spinlock. The idle task calls this every time round, and a WFE returns as soon as
     * anything is pending, which on a busy second core is immediately and forever. Without this
     * guard core 0's idle becomes a spin loop holding and releasing the very lock the other core's
     * tasks need, and they crawl.
     *
     * Twice in one tick cannot answer differently: every deadline those walks read is counted in
     * ticks. A window that does open moves the clock far past this guard on its own. */
    if (owns_time_base)
    {
        uint32_t now = os_tick_get();

        owns_time_base = (now != os_tickless_last_plan_tick);
        os_tickless_last_plan_tick = now;
    }

    if (owns_time_base)
    {
        /* Interrupts off BEFORE deciding how long to sleep, and kept off until the sleep has been
         * accounted for.
         *
         * Every input to that decision - the next timer expiry, the earliest sleeping task - is
         * something an ISR can change. Reading them with interrupts live leaves a
         * window in which an ISR registers a nearer deadline than the one just computed, and the sleep
         * then runs straight past it. Waking a task in that window is harmless, because that pends
         * PendSV and a pending exception cuts the WFI short, but starting a timer pends nothing at
         * all: there would be no wake-up event, and the timer would fire late by the whole
         * remaining window.
         *
         * Masking first closes it. A WFI still wakes on a pending interrupt while masked, so anything
         * arriving from here on shortens the sleep rather than being missed. */
        mask_state = os_arch_kernel_mask_save();

#if (OS_CONFIG_CORE_COUNT > 1U)
        /* Raised BEFORE the deadlines are read, not before the sleep. Anything another core arms
         * from here on is either already visible to the read below or answered by the IPI in
         * os_tickless_deadline_armed - and there is no instant that is neither. */
        os_tickless_window_open = true;
#endif

        planned_idle_ticks = os_tickless_expected_idle_ticks_get();

        /* The port's own ceiling, applied here rather than inside the expected-idle calculation.
         * Only when it is non-zero: a port that answers 0 cannot suppress anything, but it still
         * gets the whole pass below - deadlines honoured, the application's sleep hooks called,
         * and a plain WFI for the sleep. Clamping to 0 would skip all of that. */
        suppress_ceiling = os_arch_max_suppressed_ticks_get();

        if ((suppress_ceiling != 0U) && (planned_idle_ticks > suppress_ceiling))
        {
            planned_idle_ticks = suppress_ceiling;
        }

        /* Too short to be worth suppressing: the mask is handed straight back and this
         * idle pass behaves like a plain WFI. */
        if (planned_idle_ticks >= OS_TICKLESS_MIN_IDLE_TICKS)
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

#if (OS_CONFIG_CORE_COUNT > 1U)
        /* Lowered before the mask, so a deadline armed while this core is still masked still finds
         * the window closed and skips an IPI that would wake nobody. */
        os_tickless_window_open = false;
#endif

        /* Releases the mask taken before the sleep was planned. Nesting is deliberate: the port's own
         * mask (taken in os_arch_sleep_prepare, released by os_arch_sleep_finish above) sits inside
         * this one, and both are save/restore rather than unconditional enables, so the interrupt
         * state the idle task arrived with is what it leaves with. */
        os_arch_kernel_mask_restore(mask_state);
    }
}

/******************************************************************************************************/
/**
 * @brief Tell core 0 that a deadline nearer than its suppressed window has just been armed.
 *
 * Called from every path that puts a new expiry on a kernel time source: a task joining the delay
 * list, a timer joining the running list. On a single-core build there is no window anyone else
 * could be sleeping through and this compiles away entirely.
 *
 * The IPI is what ends the window: a WFI wakes on a pending interrupt even with the kernel mask
 * held, so core 0 leaves the sleep, measures what actually elapsed and announces it. That is the
 * same path an ordinary early wake takes, so nothing new has to be correct for this to work.
 *
 * Cheap where it does not apply: one flag read on a path that is already inside a critical
 * section, and an IPI only while a window is genuinely open somewhere else.
 *
 * @return None.
 */
void os_tickless_deadline_armed(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    if (os_tickless_window_open && (os_arch_core_id_get() != 0U))
    {
        os_arch_core_ipi_request_cb(0U);
    }
#endif
}

/* os_tickless_pre_sleep_cb() and os_tickless_post_sleep_cb() are deliberately NOT defined here.
 *
 * They describe the board, not the kernel: which sleep mode to enter, which clocks to gate, which
 * peripherals must be flushed or quiesced first. None of that is anything the kernel could guess,
 * and a weak empty default HERE would make every part look alike.
 *
 * The SoC package answers instead, weakly, because it is the layer that knows what its chip can do
 * and what the sensible default costs - see os_tickless_pre_sleep_cb in
 * soc/raspberrypi/common/soc_common.c, which documents its empty body as selecting a plain SLEEP
 * rather than a deeper mode the kernel could not measure a sleep against.
 *
 * An application that needs more - a UART flushed before the clock stops, a sensor parked - defines
 * either one strongly and displaces the package's default for that hook alone. It does NOT need to
 * define them just to enable tickless idle. */

#endif /* OS_CONFIG_TICKLESS_ENABLE */
