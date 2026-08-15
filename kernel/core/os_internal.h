/**
 * @file os_internal.h
 * @brief Ahura kernel internal (cross-module) interfaces. Not part of the public API.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_INTERNAL_H
#define OS_INTERNAL_H

#include "../ahura.h"
#include "os_arch_port.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Internal function prototypes
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * PART 1 - ALWAYS AVAILABLE
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * Tasks and the scheduler
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize the task management subsystem (os_task.c).
 */
void os_task_system_init(void);

/*
 * Scheduler-lock state, defined in os_kernel.c and read directly here rather than through
 * os_kernel_is_locked(): the scheduler tests it on its hot paths - every PendSV, every tick
 * that considers a reschedule - where a cross-module call would cost more than the check.
 * os_kernel_lock_count is nonzero while that core defers its switches; os_kernel_switch_pending
 * records a switch swallowed while it was held, for the outermost unlock to issue.
 */
extern __IO uint32_t os_kernel_lock_count[OS_CONFIG_CORE_COUNT];
extern __IO bool     os_kernel_switch_pending[OS_CONFIG_CORE_COUNT];

/******************************************************************************************************/
/**
 * @brief Create the mandatory idle task (os_task.c).
 */
os_status os_task_idle_create(void);

/******************************************************************************************************/
/**
 * @brief Check whether the idle task is already created (os_task.c).
 */
bool os_task_idle_is_created(void);

/******************************************************************************************************/
/**
 * @brief Update task delays with elapsed kernel ticks; wakes expired tasks (os_task.c, ISR context).
 */
void os_task_tick_update(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Create a task without the user priority restriction; kernel use only (os_task.c).
 */
os_status os_task_create_system(os_task_t *task, const os_task_config_t *config);

/******************************************************************************************************/
/**
 * @brief Block the calling task for ticks; OS_WAIT_FOREVER blocks until os_task_wake (os_task.c).
 */
void os_task_sleep_ticks(uint32_t ticks);

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by id; no-op for other states (os_task.c, ISR-safe).
 */
void os_task_wake(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief Resolve a task id to an opaque handle once, for os_task_wake_tcb (os_task.c, ISR-safe).
 */
void* os_task_tcb_resolve(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by its os_task_tcb_resolve handle, skipping the id lookup and the
 *        nested critical section os_task_wake pays - caller must already hold the kernel mask
 *        (and, on multi-core builds, os_critical_multicore_lock) (os_task.c, ISR-safe).
 */
void os_task_wake_tcb(void *tcb_handle);

/******************************************************************************************************/
/**
 * @brief Queue the calling task on an object's waiter list and block it; call inside a critical
 *        section from task context, the switch happens when the caller exits it (os_task.c).
 */
void os_task_wait_begin(os_list_t *waiters, uint32_t timeout_ticks);

/******************************************************************************************************/
/**
 * @brief Drop the calling task's pending-wake state; call on every path where a blocking
 *        primitive stops retrying and returns to its caller, signaled or not - that is what
 *        tells os_task_wake_compensate the notification was consumed (os_task.c).
 */
void os_task_wait_end(void);

/******************************************************************************************************/
/**
 * @brief After resuming from a wait: true = object signaled, false = timeout (os_task.c).
 */
bool os_task_wait_signaled(void);

/******************************************************************************************************/
/**
 * @brief Attach two words of per-wait data to the calling task before os_task_wait_begin;
 *        a waker's match callback reads them to evaluate the waiter's condition (os_task.c).
 */
void os_task_wait_data_set(uint32_t data0, uint32_t data1);

/******************************************************************************************************/
/**
 * @brief After a signaled resume: the result value the waker stored for this task via
 *        os_task_waiters_wake_match, 0 when woken any other way (os_task.c).
 */
uint32_t os_task_wait_result_get(void);

/******************************************************************************************************/
/**
 * @brief Waker-side condition callback for os_task_waiters_wake_match: receives one waiter's
 *        wait data; returns true to wake that waiter and stores its delivery in *result_out.
 */
typedef bool (*os_task_wait_match_fn)(uint32_t data0, uint32_t data1, void *context, uint32_t *result_out);

/******************************************************************************************************/
/**
 * @brief Wake every waiter whose condition the callback confirms, storing each waiter's result
 *        for os_task_wait_result_get. Call inside a critical section; ISR-safe (os_task.c).
 *
 * @return uint32_t  Number of waiters woken.
 */
uint32_t os_task_waiters_wake_match(os_list_t *waiters, os_task_wait_match_fn match, void *context);

/******************************************************************************************************/
/**
 * @brief Wake the highest-priority waiter of an object; true when one was woken
 *        (os_task.c, call inside a critical section, ISR-safe).
 */
bool os_task_waiters_wake_one(os_list_t *waiters);

/******************************************************************************************************/
/**
 * @brief Wake every waiter of an object (os_task.c, call inside a critical section, ISR-safe).
 */
void os_task_waiters_wake_all(os_list_t *waiters);

/******************************************************************************************************/
/**
 * @brief Get the id of the current task, 0 when idle/none/pre-scheduler (os_task.c).
 */
uint32_t os_task_current_id_get(void);

/******************************************************************************************************/
/**
 * @brief Check whether the idle task is currently running (os_task.c, ISR-safe).
 */
bool os_task_current_is_idle(void);

/******************************************************************************************************/
/**
 * @brief Whether a PendSV on this core would actually switch or round-robin (os_task.c,
 *        ISR-safe). Lets the tick handler skip a pointless PendSV round trip. False while the
 *        scheduler is locked, and for an equal-priority peer until the running task's time
 *        slice has run out.
 */
bool os_task_reschedule_possible(void);

/******************************************************************************************************/
/**
 * @brief Count elapsed ticks against the running task's round-robin time slice; call once per
 *        tick on every core, before os_task_reschedule_possible (os_task.c, ISR context).
 */
void os_task_slice_tick(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Terminate the calling task; used when a task entry function returns (os_task.c).
 */
void os_task_exit(void);

/******************************************************************************************************/
/**
 * @brief Save the stack pointer of the task being switched out (os_task.c, called from PendSV).
 */
void os_task_stack_save_current(uint32_t *stack_ptr);

/******************************************************************************************************/
/**
 * @brief Select the next task to run and return its stack pointer; never NULL (os_task.c, called from PendSV).
 */
uint32_t* os_task_stack_select_next(void);

/******************************************************************************************************/
/**
 * @brief Return ticks until the next finite-delay sleeper wakes, UINT32_MAX when none (os_task.c).
 */
uint32_t os_task_next_delay_ticks_get(void);

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
/*
 * ***********************************************************************************************************
 * Task notification mailbox - the storage os_notify.c works on
 * ***********************************************************************************************************
 *
 * The mailbox lives in the TCB because it belongs to the task, not to any object; os_notify.c owns
 * what it MEANS. These three calls are the whole surface between them - os_task.c never reads or
 * writes a slot, and os_notify.c never sees a TCB - and they join os_task_tcb_resolve,
 * os_task_wake_tcb and os_task_sleep_ticks, which the notify module uses as they stand.
*/

/******************************************************************************************************/
/**
 * @brief One task's notification mailbox: a value with overwrite semantics, plus the two flags
 *        that say whether one is latched and whether the owner is parked waiting for it.
 */
typedef struct
{
    uint32_t value;   /* latched value (overwrite: last os_notify_give wins)          */
    bool     pending; /* a value is latched, waiting to be consumed by os_notify_wait */
    bool     waiting; /* true only while the task is blocked inside os_notify_wait    */

} os_notify_slot_t;

/******************************************************************************************************/
/**
 * @brief The calling task's TCB handle, or NULL for the idle task and before the scheduler starts
 *        (os_task.c, call inside a critical section).
 */
void* os_task_tcb_current(void);

/******************************************************************************************************/
/**
 * @brief The notification mailbox embedded in a resolved task (os_task.c).
 */
os_notify_slot_t* os_task_notify_slot(void *tcb_handle);

/******************************************************************************************************/
/**
 * @brief Whether a resolved task is currently BLOCKED (os_task.c, caller holds a critical section).
 */
bool os_task_tcb_is_blocked(const void *tcb_handle);
#endif /* OS_CONFIG_NOTIFY_ENABLE */

/*
 * ***********************************************************************************************************
 * Tick
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize the kernel tick source and bookkeeping (os_tick.c).
 */
void os_tick_init(void);

/******************************************************************************************************/
/**
 * @brief Handle one periodic tick event; call from the tick interrupt (os_tick.c).
 */
void os_tick_handler(void);

/******************************************************************************************************/
/**
 * @brief Announce multiple elapsed ticks to the kernel time base (os_tick.c).
 */
void os_tick_announce(uint32_t elapsed_ticks);

/*
 * ***********************************************************************************************************
 * Internal helpers
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Acquire the cross-core kernel spinlock only (os_critical.c); caller manages its own
 *        local kernel mask directly. See os_critical_multicore_lock for the deadlock rule.
 */
void os_critical_multicore_lock(void);

/******************************************************************************************************/
/**
 * @brief Release the cross-core kernel spinlock acquired by os_critical_multicore_lock.
 */
void os_critical_multicore_unlock(void);
#else
/* No cross-core exclusion needed on single-core builds: the local kernel mask
 * (already raised by every caller) is already sufficient by itself. */
static inline void os_critical_multicore_lock(void)   { }
static inline void os_critical_multicore_unlock(void) { }
#endif

/******************************************************************************************************/
/**
 * @brief Check whether the caller is allowed to block (task context, scheduler running and not
 *        locked).
 *
 * A scheduler lock defers the very context switch blocking depends on, so a primitive that blocked
 * under one would leave its task running while the kernel had it parked. Every blocking primitive
 * therefore degrades to its OS_WAIT_NOTHING behaviour there, exactly as it does in an ISR.
 */
static inline bool os_internal_can_block(void)
{
    return (os_kernel_is_running() && !os_arch_in_isr() &&
            (os_kernel_lock_count[os_arch_core_id_get()] == 0U));
}

/******************************************************************************************************/
/**
 * @brief Convert a millisecond timeout to ticks, preserving OS_WAIT_FOREVER and saturating
 *        huge finite values one tick short of the sentinel (a finite request must never
 *        silently become "wait forever").
 */
static inline uint32_t os_internal_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == OS_WAIT_FOREVER)
    {
        return OS_WAIT_FOREVER;
    }

#if (OS_CONFIG_TICK_HZ == 1000U)
    /* 1 ms = 1 tick: identity, no 64-bit math on the hot path. */
    return timeout_ms;
#else
    {
        uint64_t ticks = (((uint64_t)timeout_ms * (uint64_t)OS_CONFIG_TICK_HZ) + 999ULL) / 1000ULL;

        if (ticks >= (uint64_t)OS_WAIT_FOREVER)
        {
            return OS_WAIT_FOREVER - 1U;
        }

        return (uint32_t)ticks;
    }
#endif
}

/******************************************************************************************************/
/**
 * @brief Remaining wait budget measured against the wall clock: budget minus ticks elapsed
 *        since start_tick (wrap-safe), never below 0, OS_WAIT_FOREVER passed through.
 *
 * Blocking primitives recompute their budget with this after every spurious wake, so time
 * spent READY (preempted between wake and re-check) counts against the timeout - a relative
 * re-arm would freeze the clock and stretch timeouts unboundedly under wake traffic.
 */
static inline uint32_t os_internal_wait_remaining(uint32_t budget_ticks, uint32_t start_tick)
{
    uint32_t elapsed;

    if (budget_ticks == OS_WAIT_FOREVER)
    {
        return OS_WAIT_FOREVER;
    }

    elapsed = os_tick_get() - start_tick;

    return (elapsed >= budget_ticks) ? 0U : (budget_ticks - elapsed);
}

/*
 * ***********************************************************************************************************
 * PART 2 - CONFIGURABLE
 * ***********************************************************************************************************
 *
 * Same order as PART 2 of ahura.h and of os_config.h. Every prototype here is
 * behind the guard of the module that DEFINES it, so a disabled feature cannot
 * leave a declaration pointing at a symbol that was never compiled.
*/

/*
 * ***********************************************************************************************************
 * Mutex priority inheritance - OS_CONFIG_MUTEX_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Link a just-acquired mutex into the calling task's owned-mutex list (os_task.c,
 *        call inside the same critical section as the successful lock).
 */
void os_task_mutex_owner_link(os_list_node_t *owner_node);

/******************************************************************************************************/
/**
 * @brief Boost owner_task_id's effective priority to the calling (waiting) task's effective
 *        priority if that is higher; single-level only, not transitive (os_task.c, call inside
 *        a critical section, right before joining the mutex's waiter list).
 */
void os_task_mutex_priority_inherit(uint32_t owner_task_id);

/******************************************************************************************************/
/**
 * @brief Unlink a released mutex from the owned-mutex list of the task owner_id names - not the
 *        calling task, which need not be the owner - and recompute that owner's effective
 *        priority as max(base_priority, highest waiter still queued on any mutex it still holds)
 *        (os_task.c, call inside a critical section, right after releasing).
 */
void os_task_mutex_owner_unlink_and_reprioritize(uint32_t owner_id, os_list_node_t *owner_node);
#endif /* OS_CONFIG_MUTEX_ENABLE */

/*
 * ***********************************************************************************************************
 * Mutex deadlock detection - development builds only
 * ***********************************************************************************************************
 *
 * Tied to OS_CONFIG_ASSERT_ENABLE rather than carrying a switch of its own, because an assertion is
 * the only way it can report: with assertions compiled out it would walk the wait chain and then
 * throw the answer away. Off also means the TCB does not carry the field this needs, so a release
 * build pays neither the RAM nor the walk.
 *
 * Worth having on in development precisely because priority inheritance cannot help here. A
 * deadlock is an ORDERING fault - two tasks taking the same two mutexes in opposite orders - while
 * inheritance only fixes the TIMING of a wait. Nothing the scheduler can do breaks a cycle, so
 * without this the symptom is a board that silently stops, with every task blocked and nothing
 * pointing at which lock pair caused it.
*/
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_ASSERT_ENABLE == 1U)
#define OS_MUTEX_DEADLOCK_CHECK  1
#else
#define OS_MUTEX_DEADLOCK_CHECK  0
#endif

#if (OS_MUTEX_DEADLOCK_CHECK == 1)

/* Longest wait chain a walk follows, and so the most mutexes a reported cycle can name. */
#define OS_TASK_DEADLOCK_MAX_DEPTH   8U

/******************************************************************************************************/
/**
 * @brief What the last detected deadlock consisted of - written the instant a cycle is found, and
 *        left in RAM for the debugger.
 *
 * This exists because an assertion can only carry a file and a line, and those are always the same
 * ones inside os_mutex.c: "a deadlock happened" without saying between what. Nor can the details be
 * printed on the way down. The kernel log is a ring drained by a task, and that task will never run
 * again once the core parks; calling the output transport directly from here would be worse still,
 * since a DMA-based os_log_output_cb would wait forever for a completion interrupt that cannot
 * arrive with interrupts masked.
 *
 * So it is left where a halted core can still be asked: break in after the assert and read
 * os_task_deadlock_report. A deliberately non-static symbol, so it can be found by name in the map
 * file of a build with no debug info at all. `requested` is NULL until something is recorded.
 */
typedef struct
{
    const os_mutex_t *requested;    /* mutex the blocking task asked for                         */
    const char       *waiter_name;  /* task that was about to block (NULL if unnamed)            */
    uint32_t          waiter_id;
    const char       *owner_name;   /* task holding `requested`, i.e. the one it waits behind    */
    uint32_t          owner_id;
    uint32_t          cycle_length; /* mutexes in the cycle: 1 = it waits behind itself, 2 = pair */
    const os_mutex_t *cycle[OS_TASK_DEADLOCK_MAX_DEPTH]; /* those mutexes, in the order walked   */

} os_task_deadlock_report_t;

extern os_task_deadlock_report_t os_task_deadlock_report;

/******************************************************************************************************/
/**
 * @brief Record the mutex the calling task is about to block on, so another task's chain walk can
 *        follow it (os_task.c, call inside the same critical section as the wait). Cleared for the
 *        caller by os_task_wait_end().
 *
 * Call it ONLY for an unbounded wait. A task that will time out is not a link in any deadlock -
 * it breaks the chain by giving up - so it must stay invisible to the walk.
 */
void os_task_mutex_blocked_on_set(const os_mutex_t *mutex);

/******************************************************************************************************/
/**
 * @brief Report whether blocking the calling task on this mutex would close a wait cycle - i.e.
 *        deadlock (os_task.c, call inside a critical section, right before joining the waiter
 *        list). Diagnostic only: it detects, it cannot recover.
 */
bool os_task_mutex_deadlock_check(const os_mutex_t *mutex);
#endif /* OS_MUTEX_DEADLOCK_CHECK */

/*
 * ***********************************************************************************************************
 * Software timers            - OS_CONFIG_TIMER_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Create and start the kernel timer service task (os_timer.c).
 */
os_status os_timer_system_init(void);

/******************************************************************************************************/
/**
 * @brief Advance all registered software timers by elapsed ticks (os_timer.c, ISR context).
 */
void os_timer_tick_process(uint32_t elapsed_ticks);

/******************************************************************************************************/
/**
 * @brief Return ticks until the next active timer expiry, UINT32_MAX when none (os_timer.c).
 */
uint32_t os_timer_next_expiry_ticks_get(void);
#endif /* OS_CONFIG_TIMER_ENABLE */

/*
 * ***********************************************************************************************************
 * Buffered logging           - OS_CONFIG_LOG_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_LOG_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Create and start the kernel log service task (os_log.c).
 */
os_status os_log_system_init(void);
#endif /* OS_CONFIG_LOG_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_INTERNAL_H */
