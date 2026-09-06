/**
 * @file os_timer.h
 * @brief Software timers and deferred calls (os_timer.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_TIMER_H
#define OS_TIMER_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Software timer     - OS_CONFIG_TIMER_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TIMER_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Timer operating mode.
 */
typedef enum
{
    
    OS_TIMER_MODE_ONE_SHOT = 0, /**< Fires once, then stops.             */
    OS_TIMER_MODE_PERIODIC = 1, /**< Reloads and fires every period.     */
    OS_TIMER_MODE_SUBMIT   = 2, /**< Defers a callback to the timer task */

} os_timer_mode_t;

/******************************************************************************************************/
/**
 * @brief Timer callback signature.
 *
 * Both arguments come from os_timer_start or os_timer_submit, so a call can say WHICH object
 * it concerns and WHAT happened without the kernel copying a payload.
 */
typedef void (*os_timer_callback_t)(void *context, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Software timer object.
 */
typedef struct
{
    /** Points at this object, so the kernel can tell a real timer from a lump of memory: the
     *  link state lives inside the object, and following a garbage node would write to an
     *  address nobody chose. Anything else is refused with OS_ERR_INVALID_ARG. A
     *  self-pointer rather than a constant catches a COPIED timer too. See os_timer.c. */
    void                *self;
    uint32_t            period_ticks;
    uint32_t            remaining_ticks;
    os_timer_mode_t     mode;
    bool                active;  /**< Counting down right now.                       */
    bool                paused;  /**< Halted by os_timer_pause, remaining_ticks kept. */
    bool                queued;  /**< Expiry noted by the tick, waiting its turn to run. */
    os_timer_callback_t callback;
    void                *context;
    uint32_t            value;
    os_list_node_t      ready_node;    /**< Links into the FIFO of expiries awaiting delivery.   */
    os_list_node_t      running_node;  /**< Links into the list of timers the tick counts down. */

} os_timer_t;

/******************************************************************************************************/
/**
 * @brief One slot in a pool: a timer, plus the pool to hand it back to. The back-pointer lives
 *        here rather than in os_timer_t so ordinary timers do not pay for it.
 *
 * The pool is named by its struct tag because the two types point at each other, and one of them
 * has to be reachable before it is complete. A pointer to an incomplete type is all this needs,
 * which is the same arrangement os_list_node uses for its own back-reference.
 */
typedef struct
{
    os_timer_t             timer;
    struct os_timer_pool_s *pool;

} os_timer_entry_t;

/******************************************************************************************************/
/**
 * @brief A pool of deferred calls: the storage os_timer_submit hands out, one slot per call.
 *
 * Declared by OS_TIMER_DEFINE_SUBMIT and owned by the caller, which is what keeps OS_ERR_FULL
 * local to one pool. Everything here is settled at compile time except free_list and ready, which
 * the kernel fills in the first time the pool is used - so a pool needs no init call and the
 * kernel keeps no list of pools.
 */
typedef struct os_timer_pool_s
{
    void                *self;       /**< Points at this pool; the same validity check timers use.   */
    os_timer_entry_t    *entries;    /**< The slots, from OS_TIMER_DEFINE_SUBMIT.                    */
    uint32_t            count;       /**< How many, so at most this many calls may be in flight.     */
    uint32_t            delay_ticks; /**< From OS_TIMER_DEFINE_SUBMIT; 0 means deliver immediately.  */
    os_timer_callback_t callback;    /**< What every submission to this pool runs.                   */
    os_list_t           free_list;   /**< Slots nobody is using; they link through timer.ready_node. */
    bool                ready;       /**< Set on first use, when the slots are threaded onto free_list. */

} os_timer_pool_t;

/*
 * A timer's life cycle, and what each call does to the countdown:
 *
 *   OS_TIMER_DEFINE_PERIODIC / OS_TIMER_DEFINE_ONESHOT   period and callback, at compile time
 *   os_timer_start     run - from the full period, or from where a pause left off
 *   os_timer_restart   run from the full period, whatever the timer was doing
 *                      both carry the context and value the callback will receive
 *   os_timer_pause     halt, keeping the time that was left
 *   os_timer_stop      cancel, discarding both the remaining time and any owed callback
 *   os_timer_period_set    retune the period; the countdown under way is left alone
 *   os_timer_callback_set  point it at a different callback
 *   os_timer_value_set     change the number the callback receives, without restarting
 *
 * There is no init and no delete: nothing is reserved, so os_timer_start cannot fail for want
 * of a resource however many timers are running, and a stopped timer keeps its configuration
 * so it can simply be started again.
 *
 * ALL of them are ISR-safe - each is a short critical section over a list and none blocks.
 * Two caveats. With OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY set, the interrupt must sit at or below
 * that priority, the rule every ISR-safe call here follows. And the CALLBACK is not ISR
 * context: it runs on the kernel timer task, so it may block, take a mutex or wait on a queue.
 */

/** A timer is set up entirely at COMPILE time, and the macro's NAME is the mode, so there is none
 *  to pass and none to get wrong:
 *
 *    OS_TIMER_DEFINE_PERIODIC(blinker, 500U, on_blink);
 *    OS_TIMER_DEFINE_ONESHOT(timeout,  250U, on_timeout);
 *    os_timer_start(&blinker, &led2, 3U);
 *
 *  What the timer IS is settled here; what a RUN carries - the context and value the callback
 *  receives - is given to os_timer_start. Parameters are prefixed (timer_period_ms) so a caller's
 *  own names cannot be substituted into the field designators. */

/** Reloads and fires every period_ms until stopped. */
#define OS_TIMER_DEFINE_PERIODIC(timer_name, timer_period_ms, timer_callback)             \
    OS_STATIC_ASSERT(((timer_period_ms) != 0U) && ((timer_period_ms) != OS_WAIT_FOREVER), \
                   "OS_TIMER_DEFINE_PERIODIC: the period is in milliseconds and cannot "  \
                   "be 0 or OS_WAIT_FOREVER");                                            \
    os_timer_t timer_name = {                                                             \
        .self         = &timer_name,                                                      \
        .period_ticks = OS_TICKS_FROM_MS(timer_period_ms),                                \
        .mode         = OS_TIMER_MODE_PERIODIC,                                           \
        .callback     = (timer_callback)                                                  \
    }

/** Fires once, period_ms after it is started, then stops. */
#define OS_TIMER_DEFINE_ONESHOT(timer_name, timer_period_ms, timer_callback)              \
    OS_STATIC_ASSERT(((timer_period_ms) != 0U) && ((timer_period_ms) != OS_WAIT_FOREVER), \
                   "OS_TIMER_DEFINE_ONESHOT: the period is in milliseconds and cannot "   \
                   "be 0 or OS_WAIT_FOREVER");                                            \
    os_timer_t timer_name = {                                                             \
        .self         = &timer_name,                                                      \
        .period_ticks = OS_TICKS_FROM_MS(timer_period_ms),                                \
        .mode         = OS_TIMER_MODE_ONE_SHOT,                                           \
        .callback     = (timer_callback)                                                  \
    }

/** A pool of deferred calls, for the case os_timer_start deliberately does NOT serve.
 *
 *    OS_TIMER_DEFINE_SUBMIT(uart_defer, 8U, 0U, on_uart_event);
 *                                       |    |
 *                                       |    delay before each call (0 = as soon as possible)
 *                                       how many may be in flight at once
 *
 *    os_timer_submit(&uart_defer, &dev, code1);   // in the ISR
 *    os_timer_submit(&uart_defer, &dev, code2);   // again, before the first has run
 *    -> the callback runs TWICE, with code1 then code2
 *
 *  os_timer_start would have run it ONCE carrying only code2, since starting a pending timer
 *  reschedules it. A submission never coalesces: each call takes its own slot, so OS_ERR_FULL means
 *  "your eight are in flight". The delay belongs to the pool; work needing a different one is
 *  another pool. */
#define OS_TIMER_DEFINE_SUBMIT(pool_name, pool_depth, pool_delay_ms, pool_callback)       \
    OS_STATIC_ASSERT((pool_depth) > 0U,                                                   \
                   "OS_TIMER_DEFINE_SUBMIT: the depth is how many calls may be in "       \
                   "flight at once and cannot be 0");                                     \
    OS_STATIC_ASSERT((pool_delay_ms) != OS_WAIT_FOREVER,                                  \
                   "OS_TIMER_DEFINE_SUBMIT: the delay is in milliseconds; use 0 for "     \
                   "as soon as possible");                                                \
    static os_timer_entry_t pool_name##_timer_buf[(pool_depth)];                          \
    os_timer_pool_t pool_name = {                                                         \
        .self        = &pool_name,                                                        \
        .entries     = pool_name##_timer_buf,                                             \
        .count       = (pool_depth),                                                      \
        .delay_ticks = OS_TICKS_FROM_MS(pool_delay_ms),                                   \
        .callback    = (pool_callback)                                                    \
    }

/** Name a timer defined in another file. One macro for both kinds, since PERIODIC and ONESHOT
 *  declare the same object and differ only in the mode written into it. */
#define OS_TIMER_DECLARE(timer_name)    extern os_timer_t timer_name

/** Name a deferred-call pool defined in another file. A pool rather than a timer, since that is
 *  what OS_TIMER_DEFINE_SUBMIT declares; only the pool crosses, its entry array stays private. */
#define OS_TIMER_POOL_DECLARE(pool_name) extern os_timer_pool_t pool_name

/******************************************************************************************************/
/**
 * @brief Start a timer, or resume one os_timer_pause halted - a paused timer continues with the
 *        time it had left, anything else starts a full period. context and value are what the
 *        callback receives on every expiry of this run.
 */
os_err_t os_timer_start(os_timer_t *timer, void *context, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Restart a software timer from a full period, whether it was running, paused or stopped.
 *        The call to reach for when an event should push the deadline back. context and value as
 *        for os_timer_start.
 */
os_err_t os_timer_restart(os_timer_t *timer, void *context, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Halt a running timer, keeping the time it had left for os_timer_start to resume from.
 *        An expiry already noted but not yet delivered still runs.
 */
os_err_t os_timer_pause(os_timer_t *timer);

/******************************************************************************************************/
/**
 * @brief Stop a software timer, discarding the remaining time and any owed callback.
 */
os_err_t os_timer_stop(os_timer_t *timer);

/******************************************************************************************************/
/**
 * @brief Change a timer's period, in milliseconds. A countdown already under way keeps the time
 *        it had; follow with os_timer_restart to apply the new period from now.
 */
os_err_t os_timer_period_set(os_timer_t *timer, uint32_t period_ms);

/******************************************************************************************************/
/**
 * @brief Change what a timer calls. Takes effect from the next expiry; a delivery already under
 *        way still runs what it copied out. The context stays as os_timer_start left it.
 */
os_err_t os_timer_callback_set(os_timer_t *timer, os_timer_callback_t callback);

/******************************************************************************************************/
/**
 * @brief Change the value a timer's callback receives. Takes effect from the next expiry.
 */
os_err_t os_timer_value_set(os_timer_t *timer, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Run the pool's callback later, once per call - the deferred form that never coalesces.
 *        Each submission takes its own slot and produces its own delivery, FIFO. ISR-safe, and
 *        nothing is copied, so whatever context points at must outlive the call.
 *
 *        A submission has NO HANDLE, so it cannot be cancelled, retuned or given a different
 *        value. If it may need cancelling, it wants a named timer and os_timer_start instead.
 */
os_err_t os_timer_submit(os_timer_pool_t *pool, void *context, uint32_t value);

#endif /* OS_CONFIG_TIMER_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_TIMER_H */
