/**
 * @file os_mutex.c
 * @brief Mutex module implementation with owner tracking and timeouts.
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

#if (OS_CONFIG_MUTEX_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a mutex object.
 *
 * Re-initializing a mutex that still has queued waiters is refused: resetting
 * the waiter list would strand the queued tasks on dangling intrusive nodes
 * and corrupt the list. (The check reads the object's current memory, so
 * first-time init must run on zero-initialized storage - static objects are.)
 *
 * @param[in,out] mutex  Mutex object.
 * @return os_status  OK, or BUSY while tasks are waiting on it.
 */
os_status os_mutex_init(os_mutex_t *mutex)
{
    os_status status = OS_STATUS_INVALID_ARG;

    if (mutex != NULL)
    {
        os_critical_enter();

        if (mutex->waiters.head != NULL)
        {
            status = OS_STATUS_BUSY;
        }
        else
        {
            mutex->locked   = false;
            mutex->owner_id = 0U;
            os_list_init(&mutex->waiters);
            mutex->owner_node.next = NULL;
            mutex->owner_node.prev = NULL;

            status = OS_STATUS_OK;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Acquire a mutex, waiting up to timeout_ms when contended.
 *
 * Task-only: an ISR has no identity of its own and would silently borrow the interrupted task's.
 * Not recursive either - relocking one the caller already holds fails with OS_STATUS_BUSY rather
 * than deadlocking.
 *
 * @param[in,out] mutex       Mutex object.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_status  OK on acquisition, BUSY when unavailable without waiting,
 *                    TIMEOUT when the wait elapsed, INVALID_ARG from an ISR.
 */
os_status os_mutex_lock(os_mutex_t *mutex, uint32_t timeout_ms)
{
    os_status status = OS_STATUS_INVALID_ARG;

    /* A mutex is an ownership object and an ISR has no identity of its own, so
     * locking from one could only borrow whichever task it interrupted. */
    OS_ASSERT(!os_arch_in_isr());
    OS_ASSERT(mutex != NULL);

    if ((mutex != NULL) && (!os_arch_in_isr()))
    {
        uint32_t self_id         = os_task_current_id_get();
        uint32_t budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
        uint32_t start_tick      = os_tick_get();
        uint32_t remaining_ticks = budget_ticks;
        bool     waiting         = true;

        /* Retry loop with one exit (MISRA Rule 15.5): each arm records the outcome
         * in status and clears the loop flag rather than returning for itself. */
        while (waiting)
        {
            os_critical_enter();

            if (!mutex->locked)
            {
                mutex->locked   = true;
                mutex->owner_id = self_id;
                os_task_mutex_owner_link(&mutex->owner_node);
                os_task_wait_end();
                os_critical_exit();

                status  = OS_STATUS_OK;
                waiting = false;
            }
            else
            {
                bool held_by_self = ((self_id != 0U) && (mutex->owner_id == self_id));

                /* Recursive lock attempt would deadlock forever: fail fast. */
                if (held_by_self || (timeout_ms == OS_WAIT_NOTHING) || (!os_internal_can_block()))
                {
                    os_task_wait_end();
                    os_critical_exit();

                    status  = OS_STATUS_BUSY;
                    waiting = false;
                }
                else if (remaining_ticks == 0U)
                {
                    os_task_wait_end();
                    os_critical_exit();

                    status  = OS_STATUS_TIMEOUT;
                    waiting = false;
                }
                else
                {
                    /* Boost the owner before blocking: closes the priority-inversion
                     * window instead of leaving it open until the owner's next unlock. */
                    os_task_mutex_priority_inherit(mutex->owner_id);

                    /* Join the waiter list inside the same critical section that saw the
                     * mutex locked (no lost-wakeup window); the switch happens on exit. */
                    os_task_wait_begin(&mutex->waiters, remaining_ticks);
                    os_critical_exit();

                    /* Resumed: unlock signaled us (retry the take - another task may
                     * have been faster) or the wait timed out. The budget is recomputed
                     * against the wall clock so READY time counts toward the timeout. */
                    if (os_task_wait_signaled())
                    {
                        remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
                    }
                    else
                    {
                        os_task_wait_end();

                        status  = OS_STATUS_TIMEOUT;
                        waiting = false;
                    }
                }
            }
        }
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Attempt to acquire a mutex without blocking.
 *
 * @param[in,out] mutex  Mutex object.
 * @return os_status Status code.
 */
os_status os_mutex_try_lock(os_mutex_t *mutex)
{
    return os_mutex_lock(mutex, OS_WAIT_NOTHING);
}

/******************************************************************************************************/
/**
 * @brief Release a mutex object (only the owner may unlock; task-only, like os_mutex_lock).
 *
 * @param[in,out] mutex  Mutex object.
 * @return os_status  OK on release, ERROR when not locked,
 *                    NOT_OWNER when held by another task, INVALID_ARG from an ISR.
 */
os_status os_mutex_unlock(os_mutex_t *mutex)
{
    os_status status = OS_STATUS_INVALID_ARG;

    OS_ASSERT(!os_arch_in_isr());
    OS_ASSERT(mutex != NULL);

    if ((mutex != NULL) && (!os_arch_in_isr()))
    {
        uint32_t self_id = os_task_current_id_get();

        os_critical_enter();

        if (!mutex->locked)
        {
            status = OS_STATUS_ERROR;
        }
        /* Enforce ownership when both sides are identifiable tasks.
         *
         * Deliberately NOT an OS_ASSERT: OS_STATUS_NOT_OWNER is a documented return
         * value, so callers are entitled to attempt the unlock and handle it. It
         * also depends on runtime scheduling rather than on a static mistake in the
         * code, which is the line assertions are meant to sit on. */
        else if ((mutex->owner_id != 0U) && (self_id != 0U) && (mutex->owner_id != self_id))
        {
            status = OS_STATUS_NOT_OWNER;
        }
        else
        {
            /* Captured before the release clears it: the id is the only handle on the
             * task whose owned-mutex list and priority boost this unlock has to undo,
             * and that task is not necessarily the caller (see the ownership check
             * above, which passes when either side is unidentifiable). */
            uint32_t owner_id = mutex->owner_id;

            mutex->locked   = false;
            mutex->owner_id = 0U;

            /* Drop any boost owed to this mutex before waking the next waiter, so
             * the wake's own preempt check compares against the correct priority. */
            os_task_mutex_owner_unlink_and_reprioritize(owner_id, &mutex->owner_node);

            /* Hand the release to the highest-priority waiter (it re-takes in its
             * own context; no ownership transfer inside the unlock). */
            (void)os_task_waiters_wake_one(&mutex->waiters);

            status = OS_STATUS_OK;
        }

        os_critical_exit();
    }

    return status;
}

#endif /* OS_CONFIG_MUTEX_ENABLE */
