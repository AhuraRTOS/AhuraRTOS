/**
 * @file os_notify.c
 * @brief Direct-to-task notifications: a one-word mailbox built into every task, delivered
 *        without any intermediate object.
 *
 * The lightest signal the kernel offers: no object to create, size or keep alive, since it is
 * addressed to the task itself. One value with overwrite semantics - last give wins - which makes
 * it a signal rather than a stream; use a queue for anything that must not be lost.
 *
 * The mailbox lives in the TCB because it belongs to the task, not to this module: os_task.c hands
 * out the slot and the two facts about a task needed here (current, blocked).
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

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Deliver a value to a task's notification mailbox (overwrite: last write wins),
 *        waking it if it is currently blocked in os_notify_wait; ISR-safe.
 *
 * A task blocked for any other reason (delay, mutex/queue/semaphore/event wait) is left
 * alone - the value is only latched for it to pick up on its next os_notify_wait.
 *
 * A NULL task means THIS task, the same shorthand FreeRTOS uses for the calling task, which saves
 * a task from having to keep a handle to itself just to pre-arm its own mailbox. It is refused
 * from an ISR: "this task" there would mean whichever task the interrupt happened to land on, so
 * the kernel will not guess. Note that inside a timer callback it resolves to the kernel timer
 * task, since that is genuinely the task running the callback, and before the scheduler starts it
 * resolves to nothing, which lands on the same INVALID_ARG as a stale handle.
 *
 * @param[in,out] task   Target task, or NULL for the calling task.
 * @param[in]     value  Value to store.
 * @return os_status  OK, or INVALID_ARG for a stale handle, or for NULL from an ISR or before any
 *                    task is running.
 */
os_status os_notify_give(os_task_t *task, uint32_t value)
{
    os_status status = OS_STATUS_INVALID_ARG;
    void      *tcb   = NULL;

    os_critical_enter();

    if (task != NULL)
    {
        if (task->id != 0U)
        {
            tcb = os_task_tcb_resolve(task->id);
        }
    }
    else if (!os_arch_in_isr())
    {
        /* Resolves to NULL before the scheduler hands out a first task, which lands on the
         * same INVALID_ARG as a stale handle rather than on a guess. */
        tcb = os_task_tcb_current();
    }
    else
    {
        /* NULL from an ISR: nothing to address, and nothing worth inventing. */
    }

    if (tcb != NULL)
    {
        os_notify_slot_t *slot = os_task_notify_slot(tcb);

        slot->value   = value;
        slot->pending = true;

        /* Only a task parked in os_notify_wait is woken. waiting is true only inside that
         * block, so a task blocked on anything else keeps its own wakeup reason and finds the
         * value waiting the next time it asks for one.
         *
         * os_task_wake_tcb rather than os_task_wake: the critical section above already holds
         * both the kernel mask and (on multi-core) the cross-core spinlock that the
         * direct-handle form requires of its caller, so it skips the id lookup and the nested
         * critical section. */
        if (os_task_tcb_is_blocked(tcb) && slot->waiting)
        {
            slot->waiting = false;
            os_task_wake_tcb(tcb);
        }

        status = OS_STATUS_OK;
    }

    os_critical_exit();

    return status;
}

/******************************************************************************************************/
/**
 * @brief Wait for this task's own notification mailbox, up to timeout_ms.
 *
 * Task-only, like os_mutex_lock: an ISR has no task identity of its own to wait as. value_out may
 * be NULL when only the wake-up matters; the notification is consumed either way (what is dropped
 * is the copy, not the delivery), so a NULL cannot leave the mailbox full.
 *
 * @param[in]  timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @param[out] value_out   Set to the delivered value on OS_STATUS_OK; NULL to discard it.
 * @return os_status  OK on delivery, EMPTY when unavailable without waiting, TIMEOUT when the
 *                     wait elapsed, INVALID_ARG from an ISR or before a real task exists.
 */
os_status os_notify_wait(uint32_t timeout_ms, uint32_t *value_out)
{
    os_status status = OS_STATUS_INVALID_ARG;

    /* Task-only, like os_mutex_lock: an ISR has no task identity to wait as. */

    if (!os_arch_in_isr())
    {
        uint32_t budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
        uint32_t start_tick      = os_tick_get();
        uint32_t remaining_ticks = budget_ticks;
        bool     waiting         = true;

        /* Retry loop with one exit (MISRA Rule 15.5): each arm records the outcome in
         * status and clears the loop flag rather than returning for itself. */
        while (waiting)
        {
            void *current;

            os_critical_enter();

            current = os_task_tcb_current();
            if (current == NULL)
            {
                os_critical_exit();

                status  = OS_STATUS_INVALID_ARG;
                waiting = false;
            }
            else
            {
                os_notify_slot_t *slot = os_task_notify_slot(current);

                /* Cleared every iteration: only true while genuinely blocked below,
                 * so a give() arriving any other time correctly just latches. */
                slot->waiting = false;

                if (slot->pending)
                {
                    /* Consumed unconditionally: only the copy out is optional. */
                    slot->pending = false;

                    if (value_out != NULL)
                    {
                        *value_out = slot->value;
                    }

                    slot->value = 0U;
                    os_critical_exit();

                    status  = OS_STATUS_OK;
                    waiting = false;
                }
                else if ((timeout_ms == OS_WAIT_NOTHING) || (!os_internal_can_block()))
                {
                    os_critical_exit();

                    status  = OS_STATUS_EMPTY;
                    waiting = false;
                }
                else if (remaining_ticks == 0U)
                {
                    os_critical_exit();

                    status  = OS_STATUS_TIMEOUT;
                    waiting = false;
                }
                else
                {
                    /* Blocks without joining any object's waiter list - os_notify_give addresses
                     * this task directly by id, so there is no list for it to walk. The sleep runs
                     * inside the critical section that just found the mailbox empty (no
                     * lost-wakeup window); the switch happens when the outermost critical section
                     * is left. */
                    slot->waiting = true;
                    os_task_sleep_ticks(remaining_ticks);
                    os_critical_exit();

                    /* Resumed: either give() latched a value (checked at the top of the next
                     * iteration) or the wait timed out - the budget recomputed against the wall
                     * clock decides which, exactly as every other blocking primitive here does. */
                    remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
                }
            }
        }
    }

    return status;
}

#endif /* OS_CONFIG_NOTIFY_ENABLE */
