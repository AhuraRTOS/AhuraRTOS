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

/** The most one window may ever be planned for, whatever the workload and the port say.
 *
 * Not a policy and not configurable: it is a fact about a 32-bit tick counter.
 *
 * os_tickless_expected_idle_ticks_get() answers UINT32_MAX when there is genuinely nothing pending -
 * no timer, no finite-delay sleeper - and that is the honest answer. A port whose wake source is 64
 * bits wide reports no ceiling of its own for the same honest reason (soc/raspberrypi/rp235x_riscv
 * returns UINT32_MAX: mtime cannot run out). Put together, those two truths would plan a window
 * spanning the entire range of os_tick_count, and then:
 *
 *   - os_tick_announce() does os_tick_count += elapsed. At UINT32_MAX that lands one tick BELOW
 *     where it started, so the clock runs backwards - the one thing a monotonic counter must not do.
 *   - Every wrap-safe difference in the kernel - os_internal_wait_remaining(), the retry loop in
 *     os_delay_ticks() - reads os_tick_get() - start as a forward elapsed. An unsigned difference
 *     only says "forward" while the real elapsed is under half the range; past that it is
 *     indistinguishable from a small step backwards.
 *
 * Half the range is therefore the bound, and staying strictly under it is what keeps every one of
 * those reads unambiguous. At a 1 kHz tick it is close to 25 days, which is far past any window a
 * real workload asks for - this exists to stop the degenerate case, not to shape ordinary ones.
 *
 * FreeRTOS bounds the same thing with xMaximumPossibleSuppressedTicks, derived in the PORT from the
 * timer's width. This kernel already has that: os_arch_max_suppressed_ticks_get() is exactly it, and
 * every port with a narrow timer answers from its register width. This constant is the other half of
 * the same rule - the limit the KERNEL's own counter imposes once the hardware imposes none. */
#define OS_TICKLESS_MAX_IDLE_TICKS   (UINT32_MAX / 2U)

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static __IO uint32_t os_tick_count = 0U;

#if (OS_CONFIG_TEST_ENABLE == 1U)
/* Defined here rather than in test/os_test.c so the kernel library never depends on the test
 * library to link. Declared in ahura.h, where the reason it exists is written out. */
__IO uint32_t os_test_tick_isr_entries = 0U;
#endif

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
/* Core 0 produces the two counters; any core may sample and reset them.
 * Both writers and the consuming reader use the same nestable kernel critical
 * section so a remote reset cannot split a total/idle update or lose increments. */
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
    uint32_t ticks;
#if (OS_CONFIG_CORE_COUNT > 1U) && (OS_CONFIG_TICKLESS_ENABLE == 1U)
    /* A remote reader must first reconcile an outstanding suppressed window. */
    os_critical_enter();
#endif
    ticks = os_tick_count;
#if (OS_CONFIG_CORE_COUNT > 1U) && (OS_CONFIG_TICKLESS_ENABLE == 1U)
    os_critical_exit();
#endif
    return ticks;
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
#if (OS_CONFIG_TEST_ENABLE == 1U)
        /* The self-test's only window into whether a tickless window actually stopped the tick.
         * See os_test_tick_isr_entries in ahura.h; compiled out of every other build. */
        os_test_tick_isr_entries++;
#endif

        os_tick_count++;

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
        os_critical_enter();
        os_tick_usage_total_ticks++;
        if (os_task_current_is_idle())
        {
            os_tick_usage_idle_ticks++;
        }
        os_critical_exit();
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
    os_critical_enter();
    os_tick_usage_total_ticks += elapsed_ticks;
    os_tick_usage_idle_ticks  += elapsed_ticks;
    os_critical_exit();
#endif

    /* The mask stays held across the three list updates: os_tick_count is the new time the moment
     * it is written, while the lists still describe the old one, and a caller seeing that gap finds
     * a deadline reported as passed before the task waiting on it is woken.
     *
     * Unreachable today only because the one caller holds an outer mask. Costs nothing - the three
     * take their own masks anyway, and none of them runs application code. */

#if (OS_CONFIG_TIMER_ENABLE == 1U)
    os_timer_tick_process(elapsed_ticks);
#endif
    os_task_tick_update(elapsed_ticks);
    os_task_slice_tick(elapsed_ticks);

    os_arch_kernel_mask_restore(mask_state);

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

        /* The busy-tick product runs in 64 bits: a window of more than ~42 million
         * ticks (about 12 hours at 1 kHz) would otherwise overflow the 32-bit
         * multiplication and report a wrong percentage. */
        usage_percent = (uint32_t)((((uint64_t)total_ticks - (uint64_t)idle_ticks) * 100ULL) /
                                   (uint64_t)total_ticks);
    }

    return usage_percent;
}
#endif /* OS_CONFIG_CPU_USAGE_ENABLE */

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/* Only core 0 suppresses the shared timebase. While its window is open,
 * remote outer kernel entries acquire the lock, observe the flag, send an IPI,
 * then release/retry until the elapsed time has been announced. The owner holds
 * no spinlock across sleep. Thus a new relative deadline or tick read cannot
 * be published against the time origin from before the suppressed interval.
 * All flag transitions and tests use the global lock; local masking alone
 * would leave an open-versus-remote-entry race. */
static __IO uint32_t os_tickless_last_plan_tick = 0U;

/** Bumped whenever a new expiry joins a kernel time source. A hint, not a guarantee: written from
 *  any core, read without a lock, and nothing rests on it being current - a stale read costs one
 *  idle pass. It only lets the guard below tell "nothing changed" from "re-planning could now
 *  answer differently". */
static __IO uint32_t os_tickless_plan_generation = 0U;

/** The value of the above as of the last planning pass. */
static __IO uint32_t os_tickless_last_plan_generation = 0U;

#if (OS_CONFIG_CORE_COUNT > 1U)
/** Raised by core 0 from the moment it starts planning a window until that window has closed.
 *
 *  Read by the other cores, which is the whole point: core 0 cannot know about a deadline that
 *  does not exist yet, so whoever creates one has to say so. */
static __IO bool os_tickless_window_open = false;

/* Called with the kernel spinlock held and local scheduling excluded. The
 * caller releases the lock before retrying, allowing core 0 to announce and
 * close. Checking under the lock closes the open-versus-remote-entry race. */
bool os_tickless_remote_window_wait(void)
{
    bool wait = os_tickless_window_open && (os_arch_core_id_get() != 0U);
    if (wait)
    {
        os_arch_core_ipi_request_cb(0U);
    }
    return wait;
}
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
 * time on wake. Called by the idle task on every pass (os_task_idle_entry); also public in
 * ahura.h so the self-test suite can exercise it directly.
 *
 * Does nothing at all on a core other than 0. On core 0 it plans once per tick, plus once more for
 * each expiry armed inside that tick - see the two guards at the top of the body for why each is
 * there.
 *
 * @return None.
 */
void os_tickless_idle_process(void)
{
    /* This entry is public: freeze migration before identifying the owner. */
    uint32_t mask_state = os_arch_kernel_mask_save();
    uint32_t planned_idle_ticks;
    uint32_t suppress_ceiling;
    uint32_t suppress_floor;
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

    /* One planning pass per tick, plus one per deadline armed inside that tick.
     *
     * The tick test stops core 0's idle loop taking the cross-core spinlock on every pass and
     * starving core 1. The generation test puts back the one re-plan that would answer differently
     * - a task blocking for 500 ticks inside the current tick otherwise waits for the next SysTick
     * before anyone may look, losing a tick of sleep per event. Both are needed. */
    if (owns_time_base)
    {
        uint32_t now = os_tick_get();
        uint32_t generation = os_tickless_plan_generation;

        owns_time_base = (now != os_tickless_last_plan_tick) ||
                         (generation != os_tickless_last_plan_generation);

        os_tickless_last_plan_tick       = now;
        os_tickless_last_plan_generation = generation;
    }

    if (owns_time_base && os_arch_soc_sleep_prepare_cb())
    {
        /* A SoC may first park peer cores before we close remote kernel entry.
         * No global lock is held across preparation or its matching finish.
         * Plan again after preparation: a peer may have published a deadline
         * before acknowledging that it is parked. */
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

#if (OS_CONFIG_CORE_COUNT > 1U)
        /* Serialize the opening with remote kernel entry before reading any
         * deadlines. Remote calls now wake us and wait for reconciliation. */
        os_critical_enter();
        os_tickless_window_open = true;
        os_critical_exit();
#endif

        planned_idle_ticks = os_tickless_expected_idle_ticks_get();

        /* The port's own ceiling, applied here rather than inside the expected-idle calculation.
         * Only clamped when it is non-zero, because 0 does not mean "a window of no ticks" - it
         * means this port cannot suppress at all, which the test below treats as its own case
         * rather than as a very short window. */
        suppress_ceiling = os_arch_max_suppressed_ticks_get();

        if ((suppress_ceiling != 0U) && (planned_idle_ticks > suppress_ceiling))
        {
            planned_idle_ticks = suppress_ceiling;
        }

        /* And the kernel's own ceiling, which no port can raise: see OS_TICKLESS_MAX_IDLE_TICKS for
         * why a 32-bit tick counter cannot be asked to jump further than this in one announcement.
         * Applied after the port's, because either can be the binding one - a narrow timer clamps
         * first on most parts, and this clamps first on a part whose timer never runs out. */
        if (planned_idle_ticks > OS_TICKLESS_MAX_IDLE_TICKS)
        {
            planned_idle_ticks = OS_TICKLESS_MAX_IDLE_TICKS;
        }

        /* Two floors, and the higher one wins. OS_TICKLESS_MIN_IDLE_TICKS is what the
         * application asked for; os_arch_min_suppressed_ticks_get() is the wake source and the
         * sleep mode saying what a window costs to arm and to leave. The second is a fact about the
         * chip,
         * so it can raise the bar but the application cannot configure its way under it.
         *
         * Too short to be worth suppressing: the mask is handed straight back and this idle pass
         * behaves like a plain WFI. */
        suppress_floor = os_arch_min_suppressed_ticks_get();

        if (suppress_floor < OS_TICKLESS_MIN_IDLE_TICKS)
        {
            suppress_floor = OS_TICKLESS_MIN_IDLE_TICKS;
        }

        /* suppress_ceiling is part of the condition, not just of the clamp above it: a port that
         * answers 0 can arm nothing, so there is no window here to open, to measure, or to describe
         * to the sleep hooks - and above all none to SLEEP through.
         *
         * That last part is what makes this a correctness test rather than an optimisation.
         * OS_ARCH_SLEEP() ends in os_arch_soc_sleep_cb(), which a package is entitled to define as
         * its deepest mode: on an STM32 under OS_CONFIG_SLEEP_MODE_DEEP it is a Stop entry. Entered
         * with no wake source armed it also stops SysTick, so the core waits on whatever unrelated
         * interrupt happens along, and os_arch_elapsed_ticks_get() - correctly, having armed nothing
         * - reports 0. The whole sleep is then missing from os_tick_count, and every delay, timeout
         * and software timer overruns by it with nothing anywhere to say so. That is what a v7m
         * target with an STM32 DEEP build did before this test existed.
         *
         * Nothing replaces the sleep here, deliberately. os_task_idle_entry calls
         * os_arch_soc_idle_cb() on the very next line, OUTSIDE this mask, which is where an ordinary
         * idle belongs - and it is the call the packages define as WFE rather than WFI where that
         * matters (a core another core must be able to wake). Doing it from in here instead would
         * put a latching WFE inside a masked region, and would call the sleep hooks around a window
         * that does not exist. */
        if ((suppress_ceiling != 0U) && (planned_idle_ticks >= suppress_floor))
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
        /* Publish the reconciled clock and lists before remote kernel callers
         * may proceed. No spinlock was held while the hardware slept. */
        os_critical_enter();
        os_tickless_window_open = false;
        os_critical_exit();
#endif

        /* Release peers only after clocks, tick state and deadline lists are
         * restored and remote kernel entry is open again. Preparation must be
         * unwound even when the plan above was too short to enter sleep. */
        os_arch_soc_sleep_finish_cb();

        /* Releases the mask taken before the sleep was planned. Nesting is deliberate: the port's own
         * mask (taken in os_arch_sleep_prepare, released by os_arch_sleep_finish above) sits inside
         * this one, and both are save/restore rather than unconditional enables, so the interrupt
         * state the idle task arrived with is what it leaves with. */
    }
    os_arch_kernel_mask_restore(mask_state);
}

/******************************************************************************************************/
/**
 * @brief A new expiry has joined a kernel time source: re-open the planning guard, and on a
 *        multi-core build tell core 0 if it is already asleep past it.
 *
 * Called from every path that puts a new expiry on a kernel time source: a task joining the delay
 * list, a timer joining the running list. On a single-core build there is no window anyone else
 * could be sleeping through and this compiles away entirely.
 *
 * The IPI is what ends the window: a WFI wakes on a pending interrupt even with the kernel mask
 * held, so core 0 leaves the sleep, measures what actually elapsed and announces it. That is the
 * same path an ordinary early wake takes, so nothing new has to be correct for this to work.
 *
 * Cheap where it does not apply: one increment and one flag read on a path that is already inside
 * a critical section, and an IPI only while a window is genuinely open somewhere else.
 *
 * @return None.
 */
void os_tickless_deadline_armed(void)
{
    /* Tells the guard in os_tickless_idle_process that the deadline lists moved, so a window
     * planned before this expiry can be re-planned without waiting for the next tick. Every build;
     * the IPI below is the multi-core half. */
    os_tickless_plan_generation++;

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
