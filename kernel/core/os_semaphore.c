/**
 * @file os_semaphore.c
 * @brief Semaphore module implementation with timeouts.
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

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize a semaphore object.
 *
 * @param[in,out] semaphore      Semaphore object.
 * @param[in]     initial_count  Initial token count.
 * @param[in]     max_count      Maximum token count.
 * @return os_err_t        Status code.
 */
os_err_t os_semaphore_init(os_semaphore_t *semaphore, uint32_t initial_count, uint32_t max_count)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((semaphore != NULL) && (max_count != 0U) && (initial_count <= max_count))
    {
        os_critical_enter();

        /* Re-initializing with queued waiters would strand them on dangling
         * intrusive nodes and corrupt the list (first-time init must run on
         * zero-initialized storage - static objects are). */
        if (semaphore->waiters.head != NULL)
        {
            status = OS_ERR_BUSY;
        }
        else
        {
            semaphore->count     = initial_count;
            semaphore->max_count = max_count;
            os_list_init(&semaphore->waiters);

            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Give one token to semaphore (ISR-safe, never blocks).
 *
 * @param[in,out] semaphore  Semaphore object.
 * @return os_err_t    Status code.
 */
os_err_t os_semaphore_give(os_semaphore_t *semaphore)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (semaphore != NULL)
    {
        os_critical_enter();

        if (semaphore->count >= semaphore->max_count)
        {
            status = OS_ERR_FULL;
        }
        else
        {
            semaphore->count++;

            /* Hand the token to the highest-priority waiter (it re-takes in its
             * own context). */
            (void)os_task_waiters_wake_one(&semaphore->waiters);

            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Take one token from semaphore, waiting up to timeout_ms when empty.
 *
 * Nonzero timeouts are only honored from task context after os_start; from
 * interrupt context the call behaves like OS_WAIT_NOTHING.
 *
 * @param[in,out] semaphore   Semaphore object.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_err_t  OK on take, EMPTY when unavailable without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_err_t os_semaphore_take(os_semaphore_t *semaphore, uint32_t timeout_ms)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (semaphore != NULL)
    {
        uint32_t budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
        uint32_t start_tick      = os_tick_get();
        uint32_t remaining_ticks = budget_ticks;
        bool     waiting         = true;

        /* Retry loop rather than the repeated returns this used to have: one exit
         * (MISRA Rule 15.5) means the outcome is recorded in status and the loop
         * is told to stop, instead of each arm returning for itself. */
        while (waiting)
        {
            os_critical_enter();

            if (semaphore->count > 0U)
            {
                semaphore->count--;
                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_NONE;
                waiting = false;
            }
            else if ((timeout_ms == OS_WAIT_NOTHING) || (!os_internal_can_block()))
            {
                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_EMPTY;
                waiting = false;
            }
            else if (remaining_ticks == 0U)
            {
                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_TIMEOUT;
                waiting = false;
            }
            else
            {
                /* Join the waiter list inside the same critical section that saw the
                 * semaphore empty (no lost-wakeup window); the switch happens on exit. */
                os_task_wait_begin(&semaphore->waiters, remaining_ticks);
                os_critical_exit();

                /* Resumed: a give signaled us (retry the take) or the wait timed
                 * out. The budget is recomputed against the wall clock so READY
                 * time counts toward the timeout. */
                if (os_task_wait_signaled())
                {
                    remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
                }
                else
                {
                    os_task_wait_end();

                    status  = OS_ERR_TIMEOUT;
                    waiting = false;
                }
            }
        }
    }

    return status;
}

#endif /* OS_CONFIG_SEMAPHORE_ENABLE */
