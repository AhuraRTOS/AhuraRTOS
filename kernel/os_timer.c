/**
 * @file os_timer.c
 * @brief Software timers: expiry is detected by the kernel tick, and callbacks run in FIFO order on
 *        one kernel timer task at OS_CONFIG_TIMER_PRIORITY (the highest priority by default).
 *
 * There is no work queue and no deferred-call API, because a "run this soon" request IS a one-shot
 * timer. The caller declares one with OS_TIMER_DEFINE_ONESHOT and starts it with the context and value the
 * callback should receive - the same object, the same tick, the same delivery queue. Nothing is
 * owned by the kernel, so there is no pool to size and nothing that can be exhausted.
 *
 * Delivery is FIFO. Expiries join os_timer_ready_list in the order the tick notices them and the
 * task takes them from the front, so callbacks run in the order they became ready and never
 * overlap. That ordering is why the list exists at all: scanning a registry for a flag, as this
 * file used to, runs callbacks in slot order, which is not the order they were asked for.
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
 * Macros
 * ***********************************************************************************************************
*/

/*
 * The gate every public call in this file passes through first. It rules out three things:
 *
 *   1. Memory that is not a timer. The link state lives inside the object, so a hand-declared
 *      os_timer_t hands the kernel two nodes of garbage, and os_list_remove would store
 *      through node->prev - a write to an address nobody chose, from a plain os_timer_stop.
 *      Only the DEFINE macros set self, so anything else is refused.
 *
 *   2. A COPY of a real timer, whose list nodes still point into the original. A fixed
 *      signature could not see this; self can, because the copy no longer lives there.
 *
 *   3. A pool entry, which IS a valid timer and would otherwise pass. Arming a free one links
 *      its running_node into the running list while its ready_node is still on the pool's
 *      free list, and the next expiry overwrites the links the free list holds it by.
 *
 * Four bytes per timer and one comparison per call, and it subsumes the NULL check these
 * functions needed anyway. Being a marker it is a strong heuristic, not a proof -
 * os_timer_unlink_locked is what makes the unlink path provable.
 *
 * Failing it is NOT asserted. Every public call here documents OS_ERR_INVALID_ARG as its answer
 * to a bad argument, which makes a bad argument a defined input with a defined result rather than
 * a programming error. An assert would contradict that contract and would make the documented
 * return impossible to test. The asserts left in this file are internal invariants only -
 * conditions no caller can produce, which mean the kernel's own state has gone wrong.
 */
#define OS_TIMER_VALID(timer)                                                             \
    (((timer) != NULL) && ((timer)->self == (void *)(timer)) &&                           \
     ((timer)->mode != OS_TIMER_MODE_SUBMIT))

/* The same question for a pool, answered the same way. */
#define OS_TIMER_POOL_VALID(pool)                                                                 \
    (((pool) != NULL) && ((pool)->self == (void *)(pool)) &&                                      \
     ((pool)->entries != NULL) && ((pool)->count != 0U) && ((pool)->callback != NULL) &&           \
     ((pool)->delay_ticks != OS_WAIT_FOREVER))

/* A submit entry from the timer embedded in it. Licensed by OS_TIMER_MODE_SUBMIT and nothing else:
 * only OS_TIMER_DEFINE_SUBMIT's entries ever carry that mode, so only they are ever converted. */
#define OS_TIMER_ENTRY_FROM_TIMER(t)                                                              \
    ((os_timer_entry_t *)(void *)((uint8_t *)(t) - offsetof(os_timer_entry_t, timer)))

/* Timer back-references from its embedded list nodes. */
#define OS_TIMER_FROM_READY_NODE(node)     ((os_timer_t *)(void *)((uint8_t *)(node) - offsetof(os_timer_t, ready_node)))
#define OS_TIMER_FROM_RUNNING_NODE(node)   ((os_timer_t *)(void *)((uint8_t *)(node) - offsetof(os_timer_t, running_node)))

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

/* Timers the tick counts down: every started timer is linked in here and nothing else is.
 *
 * A list rather than a fixed array of slots. The tick walks only timers that are actually
 * RUNNING, so its cost follows what the application is doing rather than what it was compiled
 * to allow, and there is no capacity to run out of - os_timer_start cannot fail.
 *
 * Starting and stopping SEARCH this list rather than reading a timer's own link pointers,
 * which costs O(running timers) instead of O(1). That is deliberate: it is what lets
 * os_timer_unlink_locked promise the kernel never writes through a pointer an object supplied.
 *
 * The price is two pointers per os_timer_t, and a timer must be zero-initialized before first
 * use - which the DEFINE macros guarantee by giving it static storage. */
static os_list_t            os_timer_running_list;

/* Expiries waiting to be delivered, oldest first. A timer is in here exactly while its queued flag
 * is set, so the flag and this list are two views of one fact. Both directions of that invariant
 * are what lets os_timer_stop detach an object by looking only at the flag. */
static os_list_t            os_timer_ready_list;

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void        os_timer_task_entry(void *context);
static bool        os_timer_expired_fetch(os_timer_callback_t *callback_out, void **context_out, uint32_t *value_out);
static os_err_t   os_timer_arm(os_timer_t *timer, bool reload, void *context, uint32_t value);
static void        os_timer_detach_locked(os_timer_t *timer);
static bool        os_timer_is_running_linked(const os_timer_t *timer);
static bool        os_timer_member_locked(const os_list_t *list, const os_list_node_t *node);
static bool        os_timer_unlink_locked(os_list_t *list, os_list_node_t *node);
static void        os_timer_pool_prepare_locked(os_timer_pool_t *pool);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Start a software timer, or resume one that os_timer_pause halted.
 *
 * A paused timer continues with the time it had left, which is the whole point of pausing; any
 * other timer starts a full period. os_timer_restart is the call that reloads unconditionally.
 *
 * context and value are what the callback receives on every expiry of THIS run, which is what
 * lets one callback serve many timers and an ISR schedule work and its data in one call.
 *
 * @param[in,out] timer    Timer object.
 * @param[in]     context  Pointer passed to the callback (not copied, so it must outlive the run).
 * @param[in]     value    Number passed to the callback.
 * @return os_err_t  OK, or OS_ERR_INVALID_ARG for a timer that did not come from a DEFINE
 *                    macro. Nothing is reserved, so there is no other failure.
 */
os_err_t os_timer_start(os_timer_t *timer, void *context, uint32_t value)
{
    return os_timer_arm(timer, false, context, value);
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
 * @param[in,out] timer    Timer object.
 * @param[in]     context  Pointer passed to the callback, as for os_timer_start.
 * @param[in]     value    Number passed to the callback, as for os_timer_start.
 * @return os_err_t  OK on restart, or OS_ERR_INVALID_ARG for a timer that did not come from
 *                    one of the DEFINE macros.
 */
os_err_t os_timer_restart(os_timer_t *timer, void *context, uint32_t value)
{
    return os_timer_arm(timer, true, context, value);
}

/******************************************************************************************************/
/**
 * @brief Halt a running timer, keeping the time it had left.
 *
 * The countdown stops where it is and os_timer_start resumes from there - and since a resume
 * reserves nothing, it can never be refused. An expiry the tick already noted is still owed and
 * runs; only os_timer_stop discards it.
 * Pausing a timer that is not running reports OS_ERR_ERROR rather than quietly doing nothing,
 * since a later start would then begin a full period instead of the expected resume.
 *
 * @param[in,out] timer  Timer object.
 * @return os_err_t  OK when paused (or already paused), OS_ERR_ERROR when not running.
 */
os_err_t os_timer_pause(os_timer_t *timer)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (OS_TIMER_VALID(timer))
    {
        os_critical_enter();

        if (timer->active)
        {
            timer->active = false;
            timer->paused = true;
            status        = OS_ERR_NONE;
        }
        else
        {
            /* Already paused is success - the timer is in the state asked for. Anything else
             * (never started, stopped, or a one-shot that has fired) had no countdown to halt. */
            status = timer->paused ? OS_ERR_NONE : OS_ERR_ERROR;
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
 * @return os_err_t Status code.
 */
os_err_t os_timer_stop(os_timer_t *timer)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    /* The validity gate matters most here: this is the call that unlinks, and unlinking a garbage
     * node is the write-through-a-wild-pointer that OS_TIMER_VALID exists to prevent. */
    if (OS_TIMER_VALID(timer))
    {
        os_critical_enter();

        timer->active  = false;
        timer->paused  = false;
        os_timer_detach_locked(timer);

        os_critical_exit();

        status = OS_ERR_NONE;
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Change a timer's period, in milliseconds.
 *
 * Sets the period and nothing else. A countdown already under way keeps the time it had, so a
 * periodic timer finishes its current cycle before the new period takes effect, and a stopped or
 * paused one picks it up at its next start. os_timer_restart applies it from now instead.
 *
 * Deliberately not two behaviours in one call - os_timer_restart is the other one.
 *
 * @param[in,out] timer      Timer object.
 * @param[in]     period_ms  New period in milliseconds. Rounded UP to whole ticks, so any nonzero
 *                           request stays at least one tick.
 * @return os_err_t  OK, or OS_ERR_INVALID_ARG for NULL, 0, or OS_WAIT_FOREVER.
 */
os_err_t os_timer_period_set(os_timer_t *timer, uint32_t period_ms)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (OS_TIMER_VALID(timer) && (period_ms != 0U) && (period_ms != OS_WAIT_FOREVER))
    {
        /* The tick reads period_ticks when it reloads a periodic timer, so the write is made where
         * the tick cannot be between reading it and using it. */
        os_critical_enter();

        timer->period_ticks = OS_TICKS_FROM_MS(period_ms);

        os_critical_exit();

        status = OS_ERR_NONE;
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Change what a timer calls.
 *
 * Takes effect from the next expiry. A delivery already taken for this timer runs what was copied
 * out when it was taken, so a callback cannot be swapped out from under itself mid-call.
 *
 * The context is not touched: it belongs to the run, and os_timer_start sets a run's arguments.
 *
 * @param[in,out] timer     Timer object.
 * @param[in]     callback  New expiry callback.
 * @return os_err_t  OK, or OS_ERR_INVALID_ARG for an undefined timer or a NULL callback.
 */
os_err_t os_timer_callback_set(os_timer_t *timer, os_timer_callback_t callback)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (OS_TIMER_VALID(timer) && (callback != NULL))
    {
        /* The tick reads this when it queues an expiry, so the write is made where the tick cannot
         * be between reading it and using it. */
        os_critical_enter();

        timer->callback = callback;

        os_critical_exit();

        status = OS_ERR_NONE;
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Change the value a timer's callback receives.
 *
 * Takes effect from the next expiry, like os_timer_callback_set: a delivery already taken runs with
 * what was copied out then.
 *
 * @param[in,out] timer  Timer object.
 * @param[in]     value  Passed to the callback on each expiry.
 * @return os_err_t  OK, or OS_ERR_INVALID_ARG for a NULL timer.
 */
os_err_t os_timer_value_set(os_timer_t *timer, uint32_t value)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (OS_TIMER_VALID(timer))
    {
        os_critical_enter();

        timer->value = value;

        os_critical_exit();

        status = OS_ERR_NONE;
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Run a pool's callback later, once per call.
 *
 * The deferred-call form, and the one os_timer_start deliberately is not: starting a pending
 * timer RESCHEDULES it, so one callback runs carrying only the later event. Right for a
 * debounce, wrong for deferring an interrupt. Every submission here takes its own slot.
 *
 * The slots are the caller's, so FULL means "this pool's entries are all in flight" and
 * nothing else. The delay comes from the pool's definition, already in ticks, so nothing is
 * computed here; 0 skips the countdown and joins the delivery queue at once.
 *
 * The entry returns to the pool as delivery STARTS, so a callback may submit again.
 *
 * Nothing is handed back, so a submission cannot be cancelled or altered afterwards - the
 * entry is the kernel's until it runs. Work that needs cancelling wants a named timer.
 *
 * @param[in,out] pool     Pool declared with OS_TIMER_DEFINE_SUBMIT.
 * @param[in]     context  Pointer passed to the callback (not copied).
 * @param[in]     value    Number passed to the callback.
 * @return os_err_t  OK; INVALID_ARG for a pool that did not come from OS_TIMER_DEFINE_SUBMIT;
 *                    FULL when every one of this pool's entries is in flight.
 */
os_err_t os_timer_submit(os_timer_pool_t *pool, void *context, uint32_t value)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (OS_TIMER_POOL_VALID(pool))
    {
        os_list_node_t *node;

        os_critical_enter();

        /* Threaded on first use rather than at os_init, so a pool needs no registration and the
         * kernel needs no list of pools. */
        os_timer_pool_prepare_locked(pool);

        node = os_list_pop_front(&pool->free_list);

        if (node == NULL)
        {
            status = OS_ERR_FULL;
        }
        else
        {
            os_timer_t *entry = OS_TIMER_FROM_READY_NODE(node);

            /* A free entry is out of both live lists by construction; asserted rather than assumed,
             * because handing the same object out twice is how a list gets corrupted. */
            OS_ASSERT(!entry->queued);
            OS_ASSERT(!os_timer_is_running_linked(entry));

            entry->context = context;
            entry->value   = value;

            if (pool->delay_ticks == 0U)
            {
                /* Nothing to count: join the delivery queue now, so the call runs at the next
                 * scheduling point rather than waiting up to a whole tick to be noticed. Never
                 * linked into the running list at all, so there is nothing to unlink later. */
                entry->active          = false;
                entry->paused          = false;
                entry->remaining_ticks = 0U;
                entry->queued          = true;
                os_list_push_back(&os_timer_ready_list, &entry->ready_node);

                /* Direct-handle wake: skips the id lookup os_task_wake would do; safe because
                 * os_critical_enter above already holds the kernel mask and, on multi-core builds,
                 * the same spinlock os_task_wake_tcb requires of its caller. */
                os_task_wake_tcb(os_timer_task_tcb);
            }
            else
            {
                entry->remaining_ticks = pool->delay_ticks;
                entry->paused          = false;
                entry->active          = true;
                os_list_push_back(&os_timer_running_list, &entry->running_node);
            }

            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Thread a pool's entries onto its free list, once. Caller holds the critical section.
 *
 * Each entry is made into a valid one-shot timer carrying OS_TIMER_MODE_SUBMIT, which is what lets
 * delivery find its way back to this pool. The entries link through their timer's ready_node: an
 * entry waiting to be handed out is by definition not waiting to be delivered, so those two uses of
 * the node can never overlap.
 *
 * @param[in,out] pool  Pool object, already validated by the caller.
 * @return None.
 */
static void os_timer_pool_prepare_locked(os_timer_pool_t *pool)
{
    uint32_t index;

    if (pool->ready)
    {
        return;
    }

    os_list_init(&pool->free_list);

    for (index = 0U; index < pool->count; index++)
    {
        os_timer_entry_t *entry = &pool->entries[index];

        entry->pool           = pool;
        entry->timer.self     = &entry->timer;
        entry->timer.mode     = OS_TIMER_MODE_SUBMIT;
        entry->timer.callback = pool->callback;
        entry->timer.active   = false;
        entry->timer.paused   = false;
        entry->timer.queued   = false;

        /* period_ticks is what makes an object count as armable, so a pool with no delay still
         * gets a nonzero period its entries will never actually count down. */
        entry->timer.period_ticks = (pool->delay_ticks == 0U) ? 1U : pool->delay_ticks;

        os_list_push_back(&pool->free_list, &entry->timer.ready_node);
    }

    pool->ready = true;
}

/******************************************************************************************************/
/**
 * @brief Create and start the kernel timer service task. Called from os_init.
 *
 * @return os_err_t  Status code.
 */
os_err_t os_timer_system_init(void)
{
    os_err_t status;

    os_task_config_t config =
    {
        os_timer_task_entry,
        NULL,
        OS_CONFIG_TIMER_PRIORITY,
        OS_CONFIG_TIMER_CORE_AFFINITY
    };

    os_list_init(&os_timer_ready_list);
    os_list_init(&os_timer_running_list);

    status = os_task_create_system(&tsk_timer, &config);

    /* Each step only runs once the previous one succeeded, and the first failure
     * is what gets reported. */
    if (status == OS_ERR_NONE)
    {
        status = os_task_start(&tsk_timer);
    }

    if (status == OS_ERR_NONE)
    {
        /* Resolved once: the timer task is never deleted, so the tick-time
         * expiry wake can skip the id lookup from here on. */
        os_timer_task_tcb = os_task_tcb_resolve(tsk_timer.id);
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Advance all registered timers by elapsed ticks; queue expiries for the timer task.
 *
 * Called from the tick interrupt. Periods are reloaded here so periodic
 * timers do not drift with callback latency; expiries arriving faster than
 * the timer task can run them coalesce into one callback invocation, which is
 * what the queued flag guards - a timer already waiting for delivery must not
 * be pushed onto the queue a second time.
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks.
 * @return None.
 */
void os_timer_tick_process(uint32_t elapsed_ticks)
{
    uint32_t        mask_state;
    os_list_node_t *node;
    bool            wake_needed = false;

    if (elapsed_ticks > 0U)
    {

        /* The kernel mask is raised so a preempting ISR starting or stopping
         * timers cannot interleave with the active-check/queue-push pair
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

        for (node = os_timer_running_list.head; node != NULL; node = node->next)
        {
            os_timer_t *timer = OS_TIMER_FROM_RUNNING_NODE(node);

            /* Linked but halted: os_timer_pause keeps a timer here so a resume needs nothing back. */
            if (!timer->active)
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

            /* The queued flag and the delivery queue are two views of one fact, so they are
             * written together. A timer already waiting coalesces: expiries arriving faster than
             * the timer task can run them become one callback invocation, not a backlog. */
            if (!timer->queued)
            {
                timer->queued = true;
                os_list_push_back(&os_timer_ready_list, &timer->ready_node);
                wake_needed = true;
            }
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
 * @brief Return ticks until the next timer expiry or deferred call.
 *
 * @return uint32_t  Minimum ticks until the next one, 0 when something is already waiting to be
 *                   delivered, or UINT32_MAX when there is nothing pending at all.
 */
uint32_t os_timer_next_expiry_ticks_get(void)
{
    uint32_t minimum = UINT32_MAX;

    os_critical_enter();

    /* Something already queued means work is owed right now, so there is no idle period to
     * suppress ticks over - report 0 rather than the next countdown. */
    if (!os_list_is_empty(&os_timer_ready_list))
    {
        minimum = 0U;
    }
    else
    {
        const os_list_node_t *node;

        for (node = os_timer_running_list.head; node != NULL; node = node->next)
        {
            const os_timer_t *timer = OS_TIMER_FROM_RUNNING_NODE(node);

            if (timer->active && (timer->remaining_ticks < minimum))
            {
                minimum = timer->remaining_ticks;
            }
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
 * @brief Timer task body: run queued callbacks in order, sleep until woken otherwise.
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
        uint32_t            callback_value;

        /* The callback is invoked through the copies taken inside os_timer_expired_fetch, not
         * through the timer object: the timer may be stopped, deleted or re-initialized by anyone
         * the moment that critical section ends, and this call must not depend on it any more. */
        if (os_timer_expired_fetch(&callback, &callback_context, &callback_value))
        {
            callback(callback_context, callback_value);
            continue;
        }

        /* The emptiness check and the block form one atomic unit (outer
         * critical section), so an expiry arriving in between cannot be
         * lost: it is seen here, or its wake lands after the block. */
        os_critical_enter();

        if (os_list_is_empty(&os_timer_ready_list))
        {
            os_task_sleep_ticks(OS_WAIT_FOREVER);
        }

        os_critical_exit();
    }
}

/******************************************************************************************************/
/**
 * @brief Take the oldest queued expiry, returning what to call.
 *
 * FIFO: the front of the queue is the expiry that has been waiting longest, so callbacks run in
 * the order they became ready.
 *
 * A finished one-shot leaves the running list before its callback runs, so the callback may
 * restart its own timer; a SUBMIT entry goes back to its pool at the same moment.
 *
 * The callback, context and value are copied out inside the critical section rather than the
 * timer pointer being handed back, so anything done to the object afterwards cannot affect
 * the call already in flight.
 *
 * @param[out] callback_out  Callback to invoke, written only when true is returned.
 * @param[out] context_out   Context to pass it, written only when true is returned.
 * @param[out] value_out     Value to pass it, written only when true is returned.
 * @return bool  true when an expiry was taken.
 */
static bool os_timer_expired_fetch(os_timer_callback_t *callback_out, void **context_out, uint32_t *value_out)
{
    bool            found = false;
    os_list_node_t *node;

    os_critical_enter();

    node = os_list_pop_front(&os_timer_ready_list);

    if (node != NULL)
    {
        os_timer_t *timer = OS_TIMER_FROM_READY_NODE(node);

        /* The invariant this module rests on, checked where it would first go wrong rather than
         * reasoned about in a comment; assertion builds only. Everything in the delivery queue was
         * put there by the tick, which sets queued in the same breath. A node here without the flag
         * means the list and the flag have diverged, which is how a timer ends up queued twice. */
        OS_ASSERT(timer->queued);

        timer->queued = false;

        *callback_out = timer->callback;
        *context_out  = timer->context;
        *value_out    = timer->value;

        /* Still active means periodic: it keeps its slot and counts on. A PAUSED timer keeps its
         * slot too, which is the whole promise of os_timer_pause - "a resume can never be refused".
         * Delivering the expiry it was already owed must not quietly cost it that slot, or a resume
         * could come back FULL once other timers had taken the space. Anything else has genuinely
         * finished, so its slot goes back now rather than after the callback. */
        if ((!timer->active) && (!timer->paused))
        {
            /* Finished, so it stops being something the tick counts down. One unlink, no search:
             * that is the whole reason this is a list. */
            (void)os_timer_unlink_locked(&os_timer_running_list, &timer->running_node);

            if (timer->mode == OS_TIMER_MODE_SUBMIT)
            {
                /* Back to its own pool, before its own callback has even started - which is what
                 * lets a callback submit again and always find room. The ready node was just
                 * popped off the delivery queue, so it is detached and free to carry it there.
                 *
                 * The context goes with it: nothing depends on it any more, since it was copied
                 * out above, but a free entry must not leave the kernel holding a pointer into
                 * whatever the caller passed. */
                os_timer_entry_t *entry = OS_TIMER_ENTRY_FROM_TIMER(timer);

                timer->context = NULL;
                timer->value   = 0U;

                os_list_push_back(&entry->pool->free_list, &timer->ready_node);
            }
        }

        found = true;
    }

    os_critical_exit();
    return found;
}

/******************************************************************************************************/
/**
 * @brief Put a timer on the registry and set it counting; the body of os_timer_start/_restart.
 *
 * @param[in,out] timer    Timer object.
 * @param[in]     reload   true to count a full period, false to resume a pause where it left off.
 * @param[in]     context  Handed to the callback on each expiry of this run.
 * @param[in]     value    Handed to the callback on each expiry of this run.
 * @return os_err_t  OK on start, or OS_ERR_INVALID_ARG for a timer that did not come from
 *                    one of the DEFINE macros or has no callback.
 */
static os_err_t os_timer_arm(os_timer_t *timer, bool reload, void *context, uint32_t value)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (OS_TIMER_VALID(timer) && (timer->callback != NULL) && (timer->period_ticks != 0U))
    {
        os_critical_enter();

        /* Already linked when re-arming a running or paused timer, and pushing a node twice
         * would corrupt the list - so the check, not a blind push. There is nothing to run out
         * of here, which is why this cannot report OS_ERR_FULL. */
        if (!os_timer_is_running_linked(timer))
        {
            os_list_push_back(&os_timer_running_list, &timer->running_node);
        }

        /* This run's arguments. Written inside the critical section because the tick may be about
         * to queue an expiry that will be delivered with them. */
        timer->context = context;
        timer->value   = value;

        /* Resuming keeps remaining_ticks; everything else counts a whole period. A paused timer is the
         * only one whose remaining_ticks means anything, which is why the flag rather than the caller
         * decides what a plain start does.
         *
         * An expiry the tick already queued but the timer task has not delivered yet belongs to the
         * period being discarded here, so it goes with it. Without this, restarting a timer whose
         * expiry is still in flight leaves it queued with a full period on the clock, and the next
         * delivery runs the callback at once - a whole period early, at the very moment the caller
         * asked for the deadline to be pushed back. Only this branch withdraws it: the resume path
         * must not, because os_timer_pause documents that a noted expiry is still owed. */
        if (reload || (!timer->paused))
        {
            timer->remaining_ticks = timer->period_ticks;

            if (timer->queued)
            {
                (void)os_timer_unlink_locked(&os_timer_ready_list, &timer->ready_node);
                timer->queued = false;
            }
        }

        timer->paused = false;
        timer->active = true;

        status = OS_ERR_NONE;

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Take a timer out of the registry and out of the delivery queue. Caller holds the
 *        critical section.
 *
 * Both are done together, and only for a timer the registry actually holds. That guard is what
 * makes this safe to call on an object that has never been initialized: a fresh os_timer_t's
 * queued flag and list node are whatever the memory happened to contain, and removing a garbage
 * node from a live list would corrupt it. A timer can only be queued while it is registered, so
 * "not in the registry" is also proof that there is no queue entry to withdraw.
 *
 * @param[in,out] timer  Timer object.
 * @return None.
 */
static void os_timer_detach_locked(os_timer_t *timer)
{
    (void)os_timer_unlink_locked(&os_timer_running_list, &timer->running_node);
    (void)os_timer_unlink_locked(&os_timer_ready_list, &timer->ready_node);

    timer->queued = false;
}

/******************************************************************************************************/
/**
 * @brief Whether a timer is currently linked into the running list.
 *
 * Answered by searching the list, not by reading the timer's own neighbour pointers. Believing the
 * object is what the old O(1) version did, and it is exactly the trust that a never-initialized
 * os_timer_t abuses: its pointers are whatever the memory held, so it could claim either answer.
 * The list, by contrast, is the kernel's own and is always true.
 *
 * @param[in] timer  Timer object.
 * @return bool  true when the tick is counting this timer down.
 */
static bool os_timer_is_running_linked(const os_timer_t *timer)
{
    return os_timer_member_locked(&os_timer_running_list, &timer->running_node);
}

/******************************************************************************************************/
/**
 * @brief Whether a node is really a member of a list. Caller holds the critical section.
 *
 * Walks from the head. That is O(list length) rather than the O(1) of reading the node's own
 * pointers, and the length here is the number of timers actually RUNNING - not the number defined,
 * and not a configured maximum.
 *
 * @param[in] list  List to search.
 * @param[in] node  Node to look for.
 * @return bool  true when the list actually contains the node.
 */
static bool os_timer_member_locked(const os_list_t *list, const os_list_node_t *node)
{
    const os_list_node_t *cursor;
    bool                  found = false;

    for (cursor = list->head; cursor != NULL; cursor = cursor->next)
    {
        if (cursor == node)
        {
            found = true;
            break;
        }
    }

    return found;
}

/******************************************************************************************************/
/**
 * @brief Unlink a node from a list, trusting only the list. Caller holds the critical section.
 *
 * The unlink is the one operation here that can write through a pointer it did not choose:
 * os_list_remove stores through node->prev, so a node holding garbage sends a write to an
 * address nobody picked. Everything else only reads the object or overwrites its fields.
 *
 * So membership is PROVED first. A node the list does not contain is simply not found, and
 * once found its neighbours are known to be the list's own - which is what makes the remove
 * safe, with no marker to match by luck.
 *
 * @param[in,out] list  List to remove from.
 * @param[in,out] node  Node to remove.
 * @return bool  true when the node was a member and has been unlinked.
 */
static bool os_timer_unlink_locked(os_list_t *list, os_list_node_t *node)
{
    bool found = os_timer_member_locked(list, node);

    if (found)
    {
        os_list_remove(list, node);
    }

    return found;
}

#endif /* OS_CONFIG_TIMER_ENABLE */
