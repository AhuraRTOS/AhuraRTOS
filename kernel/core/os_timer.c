/**
 * @file os_timer.c
 * @brief Software timers: expiry detected by the kernel tick, callbacks run on
 *        a kernel timer task at OS_CONFIG_TIMER_PRIORITY (the highest priority by default).
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

#if (OS_CONFIG_TIMER_ENABLE == 1U)

/* Checked here rather than with #if: a priority may be given as an os_task_priority_t name, and an
 * enum constant is not a macro - the preprocessor would read it as 0 and reject a valid setting.
 * OS_TASK_PRIO_IDLE belongs to the idle task, which the timer task must outrank to be dispatched
 * at all. */
_Static_assert((OS_CONFIG_TIMER_PRIORITY >= OS_TASK_PRIO_1_LOWEST) &&
               (OS_CONFIG_TIMER_PRIORITY <= OS_TASK_PRIO_MAX),
               "OS_CONFIG_TIMER_PRIORITY must be OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_MAX");

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(tsk_timer, OS_CONFIG_TIMER_STACK_SIZE);

/* Resolved once in os_timer_system_init: the timer task is never deleted, so
 * this handle stays valid for the kernel's lifetime and the tick-time expiry
 * wake skips the id lookup. */
static void                 *os_timer_task_tcb = NULL;

/* Registry of started timers, advanced on every kernel tick. Fixed slots so
 * tick-time iteration stays safe against concurrent start/stop calls. The slot
 * (the pointer itself) is what the ISR and tasks race on, so __IO sits after
 * the '*' to qualify the slot rather than the pointed-to timer. */
static os_timer_t * __IO    os_timer_registry[OS_CONFIG_MAX_TIMERS];

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void        os_timer_task_entry(void *context);
static bool        os_timer_expired_fetch(os_timer_callback_t *callback_out, void **context_out);
static bool        os_timer_expired_exists(void);
static os_status   os_timer_arm(os_timer_t *timer, bool reload);
static uint32_t    os_timer_registry_slot_find(const os_timer_t *timer);
static uint32_t    os_timer_registry_slot_acquire(const os_timer_t *timer);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a software timer as one-shot or periodic.
 *
 * Re-initializing a started timer behaves like a stop plus the new configuration: the registry
 * slot goes back, so repeated re-init cannot exhaust the registry, and the tick cannot reach the
 * object while its fields are rewritten.
 *
 * @param[in,out] timer         Timer object.
 * @param[in]     period_ticks  Timer period in ticks (see OS_TICKS_FROM_MS).
 * @param[in]     mode          OS_TIMER_MODE_ONE_SHOT or OS_TIMER_MODE_PERIODIC.
 * @param[in]     callback      Expiry callback (runs in the kernel timer task).
 * @param[in]     context       Callback context pointer.
 * @return os_status       Status code.
 */
os_status os_timer_init(os_timer_t *timer, uint32_t period_ticks, os_timer_mode_t mode, os_timer_callback_t callback, void *context)
{
    os_status status = OS_STATUS_INVALID_ARG;

    if ((timer != NULL) && (period_ticks != 0U) && (callback != NULL) &&
        ((mode == OS_TIMER_MODE_ONE_SHOT) || (mode == OS_TIMER_MODE_PERIODIC)))
    {
        uint32_t slot;

        /* The critical section is what makes this safe on a live timer: clearing active alone
         * would still leave os_timer_tick_process reading this object from the tick ISR while the
         * fields below are half-rewritten. */
        os_critical_enter();

        /* Leaving the timer registered would keep the slot occupied (nothing else releases it
         * until a later start or stop) and hand the tick a timer the caller has just
         * reconfigured. */
        slot = os_timer_registry_slot_find(timer);
        if (slot < OS_CONFIG_MAX_TIMERS)
        {
            os_timer_registry[slot] = NULL;
        }

        timer->period_ticks    = period_ticks;
        timer->remaining_ticks = period_ticks;
        timer->mode            = mode;
        timer->active          = false;
        timer->paused          = false;
        timer->expired         = false;
        timer->callback        = callback;
        timer->context         = context;

        os_critical_exit();

        status = OS_STATUS_OK;
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Start a software timer, or resume one that os_timer_pause halted.
 *
 * A paused timer continues with the time it had left, which is the whole point of pausing; any
 * other timer starts a full period. os_timer_restart is the call that reloads unconditionally.
 *
 * @param[in,out] timer  Timer object (must be initialized).
 * @return os_status  OK on start, FULL when no registry slot is free.
 */
os_status os_timer_start(os_timer_t *timer)
{
    return os_timer_arm(timer, false);
}

/******************************************************************************************************/
/**
 * @brief Restart a software timer from a full period.
 *
 * Unlike os_timer_start this ignores whatever the timer was doing: running, paused or stopped, it
 * ends up counting a whole period from now. This is the call for pushing a deadline back on
 * activity - a watchdog kick, an inactivity timeout, a debounce - where resuming a part-elapsed
 * period would be exactly wrong.
 *
 * @param[in,out] timer  Timer object (must be initialized).
 * @return os_status  OK on restart, FULL when no registry slot is free.
 */
os_status os_timer_restart(os_timer_t *timer)
{
    return os_timer_arm(timer, true);
}

/******************************************************************************************************/
/**
 * @brief Halt a running timer, keeping the time it had left.
 *
 * The countdown stops where it is and os_timer_start resumes from there. The registry slot is
 * kept, so a resume can never be refused - a paused timer still costs a slot, unlike a stopped
 * one. An expiry the tick already noted is still owed and runs; only os_timer_stop discards it.
 * Pausing a timer that is not running reports OS_STATUS_ERROR rather than quietly doing nothing,
 * since a later start would then begin a full period instead of the expected resume.
 *
 * @param[in,out] timer  Timer object.
 * @return os_status  OK when paused (or already paused), OS_STATUS_ERROR when not running.
 */
os_status os_timer_pause(os_timer_t *timer)
{
    os_status status = OS_STATUS_INVALID_ARG;

    if (timer != NULL)
    {
        os_critical_enter();

        if (timer->active)
        {
            timer->active = false;
            timer->paused = true;
            status        = OS_STATUS_OK;
        }
        else
        {
            /* Already paused is success - the timer is in the state asked for. Anything else
             * (never started, stopped, or a one-shot that has fired) had no countdown to halt. */
            status = timer->paused ? OS_STATUS_OK : OS_STATUS_ERROR;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Stop a software timer, discarding any not-yet-delivered expiry.
 *
 * @param[in,out] timer  Timer object.
 * @return os_status Status code.
 */
os_status os_timer_stop(os_timer_t *timer)
{
    os_status status = OS_STATUS_INVALID_ARG;

    if (timer != NULL)
    {
        uint32_t slot;

        os_critical_enter();

        timer->active  = false;
        timer->paused  = false;
        timer->expired = false;
        slot           = os_timer_registry_slot_find(timer);
        if (slot < OS_CONFIG_MAX_TIMERS)
        {
            os_timer_registry[slot] = NULL;
        }

        os_critical_exit();

        status = OS_STATUS_OK;
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Stop a timer and leave the object needing os_timer_init before it can be used again.
 *
 * Everything os_timer_stop does, plus clearing what makes the object a timer: period, mode and
 * callback go, so start and restart refuse it with OS_STATUS_INVALID_ARG until it is initialized
 * afresh. Stop is "not now", delete is "done with this one" - which turns a use-after-delete into
 * a status code instead of a timer that quietly fires again.
 *
 * Safe to call while the timer task is delivering this timer's callback (that call copied what it
 * needs first), though a delivery already running still completes.
 *
 * @param[in,out] timer  Timer object.
 * @return os_status  OS_STATUS_OK, or OS_STATUS_INVALID_ARG for NULL.
 */
os_status os_timer_delete(os_timer_t *timer)
{
    os_status status = OS_STATUS_INVALID_ARG;

    if (timer != NULL)
    {
        uint32_t slot;

        os_critical_enter();

        timer->active          = false;
        timer->paused          = false;
        timer->expired         = false;

        slot = os_timer_registry_slot_find(timer);
        if (slot < OS_CONFIG_MAX_TIMERS)
        {
            os_timer_registry[slot] = NULL;
        }

        timer->period_ticks    = 0U;
        timer->remaining_ticks = 0U;
        timer->callback        = NULL;
        timer->context         = NULL;

        os_critical_exit();

        status = OS_STATUS_OK;
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Create and start the kernel timer service task. Called from os_init.
 *
 * @return os_status  Status code.
 */
os_status os_timer_system_init(void)
{
    uint32_t  slot;
    os_status status;

    os_task_config_t config =
    {
        os_timer_task_entry,
        NULL,
        OS_CONFIG_TIMER_PRIORITY,
        OS_CONFIG_TIMER_CORE_AFFINITY
    };

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        os_timer_registry[slot] = NULL;
    }

    status = os_task_create_system(&tsk_timer, &config);

    /* Each step only runs once the previous one succeeded, and the first failure
     * is what gets reported. */
    if (status == OS_STATUS_OK)
    {
        status = os_task_start(&tsk_timer);
    }

    if (status == OS_STATUS_OK)
    {
        /* Resolved once: the timer task is never deleted, so the tick-time
         * expiry wake can skip the id lookup from here on. */
        os_timer_task_tcb = os_task_tcb_resolve(tsk_timer.id);
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Advance all registered timers by elapsed ticks; hand expiries to the timer task.
 *
 * Called from the tick interrupt. Periods are reloaded here so periodic
 * timers do not drift with callback latency; expiries arriving faster than
 * the timer task can run them coalesce into one callback invocation.
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks.
 * @return None.
 */
void os_timer_tick_process(uint32_t elapsed_ticks)
{
    uint32_t mask_state;
    uint32_t slot;
    bool     wake_needed = false;

    if (elapsed_ticks > 0U)
    {

        /* The kernel mask is raised so a preempting ISR starting or stopping
         * timers cannot interleave with the active-check/expired-write pair
         * below (a stop landing in between would be silently undone). Also
         * covers the tickless announce path, which calls this from task context.
         * On multi-core builds the cross-core spinlock additionally excludes the
         * other cores' os_timer_start/os_timer_stop callers, who hold it via
         * os_critical_enter - the local mask alone only stops this core's own
         * interrupts. Both are held across the os_task_wake_tcb below, which is
         * exactly what that call requires of its caller (unlike os_task_wake,
         * which takes the same non-recursive lock itself and so could not be
         * called from in here). */
        mask_state = os_arch_kernel_mask_save();
        os_critical_multicore_lock();

        for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
        {
            os_timer_t *timer = os_timer_registry[slot];

            if ((timer == NULL) || (!timer->active))
            {
                continue;
            }

            if (timer->remaining_ticks > elapsed_ticks)
            {
                timer->remaining_ticks -= elapsed_ticks;
                continue;
            }

            if (timer->mode == OS_TIMER_MODE_PERIODIC)
            {
                timer->remaining_ticks = timer->period_ticks;
            }
            else
            {
                /* One-shot: keep the registry slot until the callback has run. */
                timer->active = false;
            }

            timer->expired = true;
            wake_needed    = true;
        }

        if (wake_needed)
        {
            /* Direct-handle wake: skips both the id lookup and the nested
             * critical section os_task_wake would pay on every expiring tick;
             * safe here because the kernel mask and (multi-core) spinlock this
             * function already holds are exactly what os_task_wake_tcb
             * requires the caller to provide. */
            os_task_wake_tcb(os_timer_task_tcb);
        }

        os_critical_multicore_unlock();
        os_arch_kernel_mask_restore(mask_state);
    }
}

/******************************************************************************************************/
/**
 * @brief Return ticks until next active timer expiry.
 *
 * @return uint32_t  Minimum ticks until next timer, or UINT32_MAX if none.
 */
uint32_t os_timer_next_expiry_ticks_get(void)
{
    uint32_t slot;
    uint32_t minimum = UINT32_MAX;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        const os_timer_t *timer = os_timer_registry[slot];

        if ((timer != NULL) && timer->active && (timer->remaining_ticks < minimum))
        {
            minimum = timer->remaining_ticks;
        }
    }

    os_critical_exit();
    return minimum;
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Timer task body: run expiry callbacks, sleep until woken otherwise.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_timer_task_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_timer_callback_t callback;
        void                *callback_context;

        /* The callback is invoked through the copies taken inside os_timer_expired_fetch, not
         * through the timer object: the timer may be stopped, deleted or re-initialized by anyone
         * the moment that critical section ends, and this call must not depend on it any more. */
        if (os_timer_expired_fetch(&callback, &callback_context))
        {
            callback(callback_context);
            continue;
        }

        /* The emptiness check and the block form one atomic unit (outer
         * critical section), so an expiry arriving in between cannot be
         * lost: it is seen here, or its wake lands after the block. */
        os_critical_enter();

        if (!os_timer_expired_exists())
        {
            os_task_sleep_ticks(OS_WAIT_FOREVER);
        }

        os_critical_exit();
    }
}

/******************************************************************************************************/
/**
 * @brief Take one expired timer out of the pending set, returning what to call.
 *
 * A finished one-shot releases its registry slot before the callback runs, so the callback may
 * safely restart its own timer.
 *
 * The callback and context are copied out inside the critical section rather than the timer
 * pointer being handed back: by the time it is released the kernel no longer needs the object, so
 * a delete racing with delivery cannot turn into a call through a NULL callback.
 *
 * @param[out] callback_out  Callback to invoke, written only when true is returned.
 * @param[out] context_out   Context to pass it, written only when true is returned.
 * @return bool  true when an expiry was taken.
 */
static bool os_timer_expired_fetch(os_timer_callback_t *callback_out, void **context_out)
{
    bool     found = false;
    uint32_t slot;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        os_timer_t *candidate = os_timer_registry[slot];

        if ((candidate != NULL) && candidate->expired)
        {
            candidate->expired = false;

            if (!candidate->active)
            {
                os_timer_registry[slot] = NULL;
            }

            *callback_out = candidate->callback;
            *context_out  = candidate->context;
            found         = true;
            break;
        }
    }

    os_critical_exit();
    return found;
}

/******************************************************************************************************/
/**
 * @brief Put a timer on the registry and set it counting; the body of os_timer_start/_restart.
 *
 * @param[in,out] timer   Timer object (must be initialized).
 * @param[in]     reload  true to count a full period, false to resume a pause where it left off.
 * @return os_status  OK on start, FULL when no registry slot is free.
 */
static os_status os_timer_arm(os_timer_t *timer, bool reload)
{
    os_status status = OS_STATUS_INVALID_ARG;

    if ((timer != NULL) && (timer->callback != NULL) && (timer->period_ticks != 0U))
    {
        uint32_t slot;

        os_critical_enter();

        /* Single pass: re-arming an already-registered timer takes its own
         * slot rather than a free one, and both the match and the free-slot
         * fallback are found in one walk instead of two. */
        slot = os_timer_registry_slot_acquire(timer);

        if (slot >= OS_CONFIG_MAX_TIMERS)
        {
            status = OS_STATUS_FULL;
        }
        else
        {
            os_timer_registry[slot] = timer;

            /* Resuming keeps remaining_ticks; everything else counts a whole period. A paused timer is the
             * only one whose remaining_ticks means anything, which is why the flag rather than the caller
             * decides what a plain start does.
             *
             * An expiry the tick already noted but the timer task has not drained yet belongs to the period
             * being discarded here, so it goes with it. Without this, restarting a timer whose expiry is
             * still in flight leaves active=1 with expired=1 and a full period on the clock, and the next
             * os_timer_expired_fetch runs the callback at once - a whole period early, at the very moment
             * the caller asked for the deadline to be pushed back. Only this branch clears it: the resume
             * path must not, because os_timer_pause documents that a noted expiry is still owed. */
            if (reload || (!timer->paused))
            {
                timer->remaining_ticks = timer->period_ticks;
                timer->expired         = false;
            }

            timer->paused = false;
            timer->active = true;

            status = OS_STATUS_OK;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Check whether any registered timer has a pending expiry. Caller holds the critical section.
 *
 * @return bool  True when at least one expiry awaits delivery.
 */
static bool os_timer_expired_exists(void)
{
    uint32_t slot;
    bool     exists = false;

    for (slot = 0U; (slot < OS_CONFIG_MAX_TIMERS) && (!exists); slot++)
    {
        if ((os_timer_registry[slot] != NULL) && os_timer_registry[slot]->expired)
        {
            exists = true;
        }
    }

    return exists;
}

/******************************************************************************************************/
/**
 * @brief Find the registry slot holding the given timer pointer (NULL finds a free slot).
 *
 * @param[in] timer  Timer pointer to search for, or NULL.
 * @return uint32_t  Slot index, or OS_CONFIG_MAX_TIMERS when not found.
 */
static uint32_t os_timer_registry_slot_find(const os_timer_t *timer)
{
    uint32_t slot;
    uint32_t found = OS_CONFIG_MAX_TIMERS;

    for (slot = 0U; (slot < OS_CONFIG_MAX_TIMERS) && (found == OS_CONFIG_MAX_TIMERS); slot++)
    {
        if (os_timer_registry[slot] == timer)
        {
            found = slot;
        }
    }

    return found;
}

/******************************************************************************************************/
/**
 * @brief Find timer's existing slot, or the first free slot if it has none - in one pass.
 *
 * Used by os_timer_start, where restarting an already-registered timer must take that
 * timer's own slot rather than a free one; a match found mid-scan still wins even if a free
 * slot was noticed earlier, since the loop keeps searching for it until either the match is
 * found or the whole registry has been seen.
 *
 * @param[in] timer  Timer pointer to search for.
 * @return uint32_t  timer's existing slot, else the first free slot, else OS_CONFIG_MAX_TIMERS.
 */
static uint32_t os_timer_registry_slot_acquire(const os_timer_t *timer)
{
    uint32_t free_slot = OS_CONFIG_MAX_TIMERS;
    uint32_t slot;

    for (slot = 0U; slot < OS_CONFIG_MAX_TIMERS; slot++)
    {
        if (os_timer_registry[slot] == timer)
        {
            /* Its own slot wins over any free one, so the walk stops here. */
            free_slot = slot;
            break;
        }

        if ((os_timer_registry[slot] == NULL) && (free_slot >= OS_CONFIG_MAX_TIMERS))
        {
            free_slot = slot;
        }
    }

    return free_slot;
}

#endif /* OS_CONFIG_TIMER_ENABLE */
