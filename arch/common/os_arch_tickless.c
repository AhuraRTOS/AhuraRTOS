/**
 * @file os_arch_tickless.c
 * @brief The tickless-idle port contract, written once for every architecture.
 *
 * Textually included by each port - directly on RISC-V, through arch/arm/common/os_arch_tickless.c
 * on Arm. Not a translation unit - no include guard, statics the includer uses - so it opens with
 * a #error unless the wrapper that includes it has claimed OS_ARCH_PORT_TRANSLATION_UNIT.
 *
 * The five entry points the kernel calls, written once for every port. They used to be written
 * three times and the copies disagreed about the guards, which is where A2, A6 and A9 came from.
 *
 * A port overrides three things; the defaults below suit a port that owns no tick register:
 *
 *   OS_ARCH_TICKLESS_TICK_SILENCE()/_RESTORE()  silence the tick INTERRUPT for the window
 *   OS_ARCH_TICKLESS_CYCLE_FROM_TICK            1 if the cycle counter is fed BY the tick, so the
 *                                               window has to be credited back to it
 *   OS_ARCH_TICKLESS_SELF_SUPPRESS              1 if the port can stretch its own tick timer
 *                                               (ARMv8-M only) and supplies os_arch_tickless_self_*
 *
 * Three rules live here and nowhere else: a window needs >= 2 ticks and something able to end it;
 * 0 in os_arch_tickless_planned means no window is open; and the elapsed count is CLAMPED to what
 * the window was promised, because an overshooting wake source would push os_tick_count past
 * deadlines that have not expired - late is recoverable, early is not.
 *
 * Full contract: doc/tickless.md.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_ARCH_PORT_TRANSLATION_UNIT
#error "os_arch_tickless.c is a textual include, not a translation unit. Compile arch/<family>/<core>/os_arch_port.c instead - it defines OS_ARCH_PORT_TRANSLATION_UNIT and includes this. See doc/installation.md."
#endif


#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Port answers - defaults for a port that does not override them
 * ***********************************************************************************************************
*/

#ifndef OS_ARCH_TICKLESS_TICK_SILENCE
/** No periodic tick of this port's own to silence: the SoC owns the tick timer. */
#define OS_ARCH_TICKLESS_TICK_SILENCE()   do { } while (0)
#endif

#ifndef OS_ARCH_TICKLESS_TICK_RESTORE
/** Nothing was silenced, so nothing to restore. */
#define OS_ARCH_TICKLESS_TICK_RESTORE()   do { } while (0)
#endif

#ifndef OS_ARCH_TICKLESS_ELAPSED_ADJUST
#define OS_ARCH_TICKLESS_ELAPSED_ADJUST(n) (n)
#endif

#ifndef OS_ARCH_TICKLESS_CYCLE_FROM_TICK
/** The cycle counter is real hardware, independent of the tick. */
#define OS_ARCH_TICKLESS_CYCLE_FROM_TICK  0
#endif

#ifndef OS_ARCH_TICKLESS_SELF_SUPPRESS
/** This port cannot stretch its own tick timer; a SoC wake source is the only way to open a window. */
#define OS_ARCH_TICKLESS_SELF_SUPPRESS    0
#endif

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/** Ticks the current window was opened for, 0 when none is open. Non-zero is the only reason to
 *  ask a wake source how long a window lasted; the value is the ceiling that answer is clamped to. */
static uint32_t os_arch_tickless_planned = 0U;

#if (OS_ARCH_TICKLESS_SELF_SUPPRESS == 1)
/** Which of the two mechanisms opened the window, so the close uses the same one. Only a port that
 *  has both needs to ask. */
static bool os_arch_tickless_soc_window = false;
#endif

/*
 * ***********************************************************************************************************
 * SoC callbacks - weak defaults
 * ***********************************************************************************************************
 *
 * The entire interface to a package's wake source. Nothing above names a timer, so the same code
 * serves a part nobody has packaged yet. A package that defines none of them suppresses nothing:
 * the ceiling answers 0, the kernel skips the sleep, and idle stays a plain WFI.
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
 * The package's source answers first: it is not bounded by the tick register's width and it is the
 * only kind that survives a sleep deep enough to gate that register's clock. Must agree exactly
 * with os_arch_sleep_prepare(), so both read the same two places in the same order.
 *
 * @return uint32_t  Ceiling in ticks; 0 means this port cannot suppress, and the kernel then skips
 *                    the sleep rather than entering one nothing can end.
 */
uint32_t os_arch_max_suppressed_ticks_get(void)
{
    uint32_t suppressible = os_arch_tick_suppress_max_cb();

#if (OS_ARCH_TICKLESS_SELF_SUPPRESS == 1)
    if ((suppressible == 0U) && os_arch_tickless_self_available())
    {
        suppressible = os_arch_tickless_self_max_ticks();
    }
#endif

    return suppressible;
}

/******************************************************************************************************/
/**
 * @brief Shortest window worth opening (contract in os_arch_port_common.h).
 *
 * No port adds to what the package says: entering and leaving is a WFI, the same cost everywhere.
 *
 * @return uint32_t  Floor in ticks; 0 when the package has no opinion.
 */
uint32_t os_arch_min_suppressed_ticks_get(void)
{
    return os_arch_tick_suppress_min_cb();
}

/******************************************************************************************************/
/**
 * @brief Open a suppressed window: silence the tick and arrange for something to end it.
 *
 * The SoC path takes no mask: the kernel holds its own, and a WFI wakes on a pending interrupt even
 * masked, which is what the wake source relies on. A port reprogramming its own tick does mask
 * (_self_open), because a real tick against half-written registers is a different problem.
 *
 * @param[in] planned_ticks  Tick periods the kernel expects to be idle for.
 * @return None.
 */
void os_arch_sleep_prepare(uint32_t planned_ticks)
{
    os_arch_tickless_planned = 0U;   /* not armed until something below proves it can be */

#if (OS_ARCH_TICKLESS_SELF_SUPPRESS == 1)
    os_arch_tickless_soc_window = false;
#endif

    if ((planned_ticks >= 2U) && (os_arch_tick_suppress_max_cb() != 0U))
    {
        /* Only the INTERRUPT is silenced; the counter keeps running, so the tick grid keeps its
         * phase and the missed interrupts come back as a count from the close. */
        OS_ARCH_TICKLESS_TICK_SILENCE();

#if (OS_ARCH_TICKLESS_CYCLE_FROM_TICK == 1)
        /* Nothing feeds the synthesized cycle counter now; the close credits it back. */
        os_arch_cycle_window_open();
#endif

        os_arch_tick_suppress_cb(planned_ticks);

        os_arch_tickless_planned = planned_ticks;

#if (OS_ARCH_TICKLESS_SELF_SUPPRESS == 1)
        os_arch_tickless_soc_window = true;
#endif
    }
#if (OS_ARCH_TICKLESS_SELF_SUPPRESS == 1)
    else if ((planned_ticks >= 2U) && os_arch_tickless_self_available())
    {
        /* Returns what it actually armed, which can be less than asked: the port's own timer is
         * register-width limited where a package's rarely is. 0 means it declined. */
        os_arch_tickless_planned = os_arch_tickless_self_open(planned_ticks);
    }
#endif
    else
    {
        /* Nothing can end a window, so none is opened. Belt to the kernel's braces: it tests the
         * same thing before calling OS_ARCH_SLEEP(), and between them a package's deep-sleep hook
         * can never be entered with no wake source armed. */
    }
}

/******************************************************************************************************/
/**
 * @brief Close the window: ask how long it really was, restore the tick, and account for it.
 *
 * @return uint32_t  Whole tick periods that elapsed while the tick was suppressed; 0 if no window
 *                    was ever armed.
 */
uint32_t os_arch_elapsed_ticks_get(void)
{
    uint32_t elapsed = 0U;

    if (os_arch_tickless_planned != 0U)
    {
#if (OS_ARCH_TICKLESS_SELF_SUPPRESS == 1)
        if (os_arch_tickless_soc_window)
#endif
        {
            elapsed = os_arch_tick_resume_cb();

            if (elapsed > os_arch_tickless_planned)
            {
                elapsed = os_arch_tickless_planned;   /* the window was never promised more */
            }

            OS_ARCH_TICKLESS_TICK_RESTORE();
            elapsed = OS_ARCH_TICKLESS_ELAPSED_ADJUST(elapsed);

#if (OS_ARCH_TICKLESS_CYCLE_FROM_TICK == 1)
            /* Exactly what is about to be announced, not a wrap count guessed from it: per-tick
             * crediting measures THIS timer's periods against the SoC's count and drifts. */
            os_arch_cycle_window_close(elapsed);
#endif
        }
#if (OS_ARCH_TICKLESS_SELF_SUPPRESS == 1)
        else
        {
            elapsed = os_arch_tickless_self_close(os_arch_tickless_planned);
        }

        os_arch_tickless_soc_window = false;
#endif

        os_arch_tickless_planned = 0U;
    }

    return elapsed;
}

/******************************************************************************************************/
/**
 * @brief Close out a tickless window: release anything the open took.
 *
 * Empty on most ports - the SoC path takes nothing - but called every pass, because the kernel
 * cannot know which path a port took and must not have to.
 *
 * @return None.
 */
void os_arch_sleep_finish(void)
{
#if (OS_ARCH_TICKLESS_SELF_SUPPRESS == 1)
    os_arch_tickless_self_finish();
#endif
}

#endif /* OS_CONFIG_TICKLESS_ENABLE */
