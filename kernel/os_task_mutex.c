/**
 * @file os_task_mutex.c
 * @brief Priority inheritance: the boost a mutex owner is owed, and the deadlock check.
 *
 * Split out of os_task.c, which is where this used to live under OS_CONFIG_MUTEX_ENABLE. The
 * algorithm is self-contained - a boost is recomputed from the waiters that justify it, and the
 * recompute walks the blocked_on chain - so it reads better as its own file than as 300 guarded
 * lines inside the scheduler's.
 *
 * The call SITES stay in os_task.c: pausing, deleting or re-prioritising a task all have to tell
 * this layer, and those are one line each at the point they happen.
 *
 * See doc/api.md, "Mutexes and priority inheritance".
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
#include "os_task_internal.h"

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static void os_task_mutex_effective_recompute(os_task_tcb_t *owner);
static void os_task_mutex_chain_recompute(os_task_tcb_t *task);
#endif

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Link a just-acquired mutex into the calling task's owned-mutex list.
 *
 * Only an identifiable task gets an entry. A mutex stores nothing but its owner's id, so an owner
 * that cannot be found by id at unlock time could never have its entry removed either. Id 0 - the
 * idle task, or code before the scheduler starts - is already outside priority inheritance and
 * simply holds the mutex without a list entry.
 *
 * @param[in,out] owner_node  The mutex's own owner_node link.
 * @return None.
 */
void os_task_mutex_owner_link(os_list_node_t *owner_node)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    if ((current != NULL) && (current->id != 0U))
    {
        os_list_push_back(&current->owned_mutexes, owner_node);
    }
}

/******************************************************************************************************/
/**
 * @brief Boost owner_task_id's effective priority to the calling (waiting) task's, and carry that
 *        along the chain of owners the boost has to reach to be worth anything.
 *
 * @param[in] owner_task_id  Id of the mutex's current owner.
 * @return None.
 */
void os_task_mutex_priority_inherit(uint32_t owner_task_id)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];
    os_task_tcb_t *owner    = os_task_find_by_id(owner_task_id);

    if ((current != NULL) && (owner != NULL))
    {
        /* Record whose boost this is BEFORE deciding whether to apply one, and record it even when
         * no boost is applied here. The owner's effective priority can be raised by a LATER, higher
         * waiter on the same mutex, and that boost still has to be recomputed when THIS task
         * departs - the recompute is a max() over everyone still queued, so every departure has to
         * trigger it, not only the departures that raised it. */
        current->pi_owner_id = owner_task_id;

        if (current->priority > owner->priority)
        {
            /* The immediate owner is raised directly rather than recomputed, because the caller is
             * not in the mutex's waiter list yet - os_task_wait_begin runs after this - so a max()
             * over that list would not see the very waiter asking for the boost. */
            os_task_effective_priority_set(owner, current->priority);

            /* From the next link outward every waiter IS queued, so the ordinary recompute is both
             * correct and the same rule the release paths use. */
            if (owner->blocked_on_mutex != NULL)
            {
                os_task_mutex_chain_recompute(os_task_find_by_id(owner->blocked_on_mutex->owner_id));
            }
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Release the priority boost tcb handed a mutex owner, now that it has left the waiter queue.
 *
 * Call AFTER the task has been unlinked from the waiter list: the recompute is a max() over the
 * tasks still queued, so running it while this one is still linked would just re-derive the boost
 * being dropped. Caller must hold a critical section.
 *
 * @param[in,out] tcb  Departing waiter.
 * @return None.
 */
void os_task_mutex_waiter_depart_tcb(os_task_tcb_t *tcb)
{
    uint32_t owner_id = tcb->pi_owner_id;

    if (owner_id != 0U)
    {
        /* Cleared first: this task owes the owner nothing from here on, and clearing before the
         * lookup means an owner that has since been deleted still ends the debt. */
        tcb->pi_owner_id = 0U;

        /* Chain, not a single step: this task may have been the reason a whole line of owners was
         * lifted, and dropping only the first of them leaves the rest boosted for nothing - which
         * inverts the priorities the other way and is just as wrong. */
        os_task_mutex_chain_recompute(os_task_find_by_id(owner_id));
    }
}

/******************************************************************************************************/
/**
 * @brief Release the boost the CALLING task handed a mutex owner (os_mutex.c, on every path that
 *        leaves the wait without acquiring). Caller must hold a critical section.
 *
 * @return None.
 */
void os_task_mutex_waiter_depart(void)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    if (current != NULL)
    {
        os_task_mutex_waiter_depart_tcb(current);
    }
}

/******************************************************************************************************/
/**
 * @brief Unlink a released mutex from its owner's list and recompute the owner's effective
 *        priority as max(base_priority, highest waiter still queued on any mutex it still holds).
 *        Correct even when the task holds several mutexes at once.
 *
 * The owner comes from the id passed in, never from the running task: os_mutex_unlock reaches here
 * for a non-owner too, and working on the caller would splice this node out of the REAL owner's
 * list while updating the CALLER's head and tail. An unresolvable owner has nothing to undo.
 *
 * @param[in]     owner_id    Id the mutex recorded for its owner, captured before unlock cleared it.
 * @param[in,out] owner_node  The mutex's own owner_node link, already unlocked by the caller.
 * @return None.
 */
void os_task_mutex_owner_unlink_and_reprioritize(uint32_t owner_id, os_list_node_t *owner_node)
{
    os_task_tcb_t *owner = os_task_find_by_id(owner_id);

    /* An owner that has already gone owns nothing left to unlink. */
    if (owner != NULL)
    {
        os_list_remove(&owner->owned_mutexes, owner_node);

        /* Chain, for the same reason as in os_task_mutex_waiter_depart_tcb: releasing a mutex can
         * lower this owner, and anyone waiting behind IT was only boosted on its account. */
        os_task_mutex_chain_recompute(owner);
    }
}
/******************************************************************************************************/
/**
 * @brief Record the mutex the calling task is about to block on, and whether that wait ever ends.
 *        Cleared by os_task_wait_end().
 *
 * In every build, not only a debug one: this edge is what os_task_mutex_chain_recompute follows to
 * find the task a boost actually has to reach.
 *
 * @param[in] mutex    Mutex about to be waited on, NULL to clear.
 * @param[in] forever  True for an OS_WAIT_FOREVER wait - the narrower case the deadlock walk wants.
 * @return None.
 */
void os_task_mutex_blocked_on_set(const os_mutex_t *mutex, bool forever)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    if (current != NULL)
    {
        current->blocked_on_mutex = mutex;
        current->blocked_forever  = forever;
    }
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Recompute owner's effective priority as max(base_priority, highest waiter still queued on
 *        any mutex it still holds). Caller must hold a critical section.
 *
 * The single definition of what a task's inherited priority IS, so that every event which can change
 * the answer - an unlock, a waiter timing out, a waiter being paused or deleted - arrives at it the
 * same way instead of each path carrying its own idea.
 *
 * @param[in,out] owner  Task whose effective priority is recomputed.
 * @return None.
 */
static void os_task_mutex_effective_recompute(os_task_tcb_t *owner)
{
    os_list_node_t *node;
    uint32_t        new_priority = owner->base_priority;

    for (node = owner->owned_mutexes.head; node != NULL; node = node->next)
    {
        const os_mutex_t *held       = OS_MUTEX_FROM_OWNER_NODE(node);
        os_list_node_t   *top_waiter = held->waiters.head;

        if (top_waiter != NULL)
        {
            uint32_t waiter_priority = OS_TASK_TCB_FROM_WAIT_NODE(top_waiter)->priority;

            if (waiter_priority > new_priority)
            {
                new_priority = waiter_priority;
            }
        }
    }

    os_task_effective_priority_set(owner, new_priority);
}

/******************************************************************************************************/
/**
 * @brief Recompute a task's inherited priority and then everyone it is transitively waiting behind.
 *
 * A boost is only worth what it lets the boosted task DO, and a blocked task can do nothing with
 * one - so raising the immediate owner and stopping there leaves a three-deep inversion untouched.
 * The walk follows blocked_on_mutex until it reaches a task that is actually runnable.
 *
 * Each step is the same max() every other path uses, which makes it correct in BOTH directions: a
 * boost arriving raises each link, a boost released lowers it by the same rule, and nothing here
 * knows which is happening.
 *
 * OS_TASK_DEADLOCK_MAX_DEPTH is load-bearing, not decorative: a cycle among already-deadlocked
 * tasks would otherwise be walked forever inside a critical section. See doc/api.md, "Mutexes and
 * priority inheritance".
 *
 * Caller holds a critical section.
 *
 * @param[in,out] task  Where to start; NULL is a no-op.
 * @return None.
 */
static void os_task_mutex_chain_recompute(os_task_tcb_t *task)
{
    uint32_t depth = 0U;

    while ((task != NULL) && (depth < OS_TASK_DEADLOCK_MAX_DEPTH))
    {
        const os_mutex_t *waiting_on;

        os_task_mutex_effective_recompute(task);

        /* Runnable, or waiting on something that is not a mutex: the chain ends here. */
        waiting_on = task->blocked_on_mutex;
        if (waiting_on == NULL)
        {
            break;
        }

        /* One link further out. An owner that cannot be resolved - it was deleted while holding the
         * mutex - ends the walk, exactly as it ends the deadlock walk, and for the same reason:
         * there is nobody left to boost. */
        task = os_task_find_by_id(waiting_on->owner_id);
        depth++;
    }
}

#endif /* OS_CONFIG_MUTEX_ENABLE */

#if (OS_MUTEX_DEADLOCK_CHECK == 1)

/* Post-mortem record for the debugger; see os_internal.h for why an assertion cannot carry this
 * itself. Zero-initialised, so requested == NULL means nothing has been detected. */
os_task_deadlock_report_t os_task_deadlock_report;

/******************************************************************************************************/
/**
 * @brief Report whether blocking the calling task on this mutex would close a wait cycle.
 *
 * A deadlock IS a cycle in "task waits for a mutex the next task holds": each member waits for the
 * next, so none of them can ever run again to release anything. This walks that chain forward from
 * the mutex the caller is about to wait on - who owns it, and what is that owner itself blocked on
 * - and stops at whichever comes first:
 *
 *   the caller           the chain came back to us, so waiting here would close the cycle
 *   an owner at rest     running, ready, or waiting on something that is not a mutex: no cycle
 *   an unresolved owner  id 0 or a deleted task, nothing left to follow
 *   the depth cap        see below
 *
 * Deliberately conservative. The cap exists because a cycle that already formed among OTHER tasks
 * would otherwise be walked forever, and reaching it reports NOTHING: a legitimately deep chain
 * must never be accused of a deadlock it does not have. A detector that cries wolf gets disabled,
 * which costs more than the cases it would have caught.
 *
 * Detection only - it cannot recover. Nothing the scheduler does can break a cycle, and priority
 * inheritance in particular does not: inheritance fixes how SOON a waiting task runs, while this
 * is about the ORDER two tasks took two locks in. So the answer is an assertion raised the instant
 * the cycle WOULD form, while the offending task is still running and its call stack still names
 * the code that took the locks in that order. The alternative is finding out later, from a board
 * on which several tasks are blocked forever and nothing records how they got there.
 *
 * @param[in] mutex  Mutex the caller is about to block on.
 * @return bool  true when blocking on it would complete a cycle.
 */
bool os_task_mutex_deadlock_check(const os_mutex_t *mutex)
{
    const os_task_tcb_t *current     = os_task_current[os_arch_core_id_get()];
    const os_task_tcb_t *first_owner = NULL;
    const os_mutex_t    *link        = mutex;
    bool                 cycle       = false;
    uint32_t             depth;

    for (depth = 0U; (depth < OS_TASK_DEADLOCK_MAX_DEPTH) && (link != NULL) && (current != NULL); depth++)
    {
        const os_task_tcb_t *owner = os_task_find_by_id(link->owner_id);

        /* Recorded as the walk goes, so a cycle report already holds the whole chain. Harmless
         * when no cycle is found: the fields below are what mark the report valid. */
        os_task_deadlock_report.cycle[depth] = link;

        if (depth == 0U)
        {
            first_owner = owner;
        }

        if (owner == NULL)
        {
            break;
        }

        if (owner == current)
        {
            os_task_deadlock_report.requested    = mutex;
            os_task_deadlock_report.waiter_name  = OS_TASK_NAME_OF(current);
            os_task_deadlock_report.waiter_id    = current->id;
            os_task_deadlock_report.owner_name   = (first_owner != NULL) ? OS_TASK_NAME_OF(first_owner) : NULL;
            os_task_deadlock_report.owner_id     = mutex->owner_id;
            os_task_deadlock_report.cycle_length = depth + 1U;

            cycle = true;
            break;
        }

        /* Only an UNBOUNDED wait continues the walk. A task that will time out gives up and
         * breaks the chain, so it cannot be part of a real deadlock - and the edge itself is no
         * longer the place to encode that, now that priority inheritance follows the same edge for
         * every waiter. blocked_forever is what draws the line. */
        link = owner->blocked_forever ? owner->blocked_on_mutex : NULL;
    }

    return cycle;
}
#endif /* OS_MUTEX_DEADLOCK_CHECK */
