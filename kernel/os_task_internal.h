/**
 * @file os_task_internal.h
 * @brief The TCB layout, shared by os_task.c and os_task_mutex.c and by nothing else.
 *
 * Not in os_internal.h on purpose. Every kernel file includes that one, and the scheduler's
 * central data structure has no business being visible to os_queue.c or os_log.c. This header
 * exists because priority inheritance was split into its own translation unit and genuinely
 * needs the layout; keeping it separate is what stops that split widening into a free-for-all.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_TASK_INTERNAL_H
#define OS_TASK_INTERNAL_H

#include "os_internal.h"

/* TCB back-references from the embedded intrusive list nodes. */
#define OS_TASK_TCB_FROM_NODE(node)      ((os_task_tcb_t *)(void *)((uint8_t *)(node) - offsetof(os_task_tcb_t, state_node)))
#define OS_TASK_TCB_FROM_WAIT_NODE(node) ((os_task_tcb_t *)(void *)((uint8_t *)(node) - offsetof(os_task_tcb_t, wait_node)))

/* A TCB's name, or NULL in a build that does not carry them (OS_CONFIG_TASK_NAME_ENABLE).
 *
 * Every reader of a name here is a diagnostic - the stack-overflow callback, the deadlock report,
 * os_task_name_get - and each one already has to cope with an unnamed task, because a handle can
 * be unresolvable. So the option needs no second code path anywhere: it makes "no name" the answer
 * for every task instead of for some of them. Not NULL-safe on purpose; the callers below check
 * their pointer where one can be NULL, exactly as they did before. */
#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
#define OS_TASK_NAME_OF(tcb)             ((tcb)->name)
#else
#define OS_TASK_NAME_OF(tcb)             ((const char *)NULL)
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/* Mutex back-reference from its embedded owner_node (priority inheritance). */
#define OS_MUTEX_FROM_OWNER_NODE(node)   ((const os_mutex_t *)(const void *)((const uint8_t *)(node) - offsetof(os_mutex_t, owner_node)))
#endif

typedef struct
{
#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
    const char      *name;         /* not read by the kernel: kept for debugger/trace visibility */
#endif
    uint8_t         *stack_base;
    uint32_t        *stack_ptr;
    size_t          stack_bytes;
    uint32_t        priority;
    uint32_t        id;
    uint32_t        delay_ticks;
    uint32_t        core_affinity; /* bitmask of cores the task may run on, 0 = any */
    os_task_state_t state;
    uint32_t        running_core;  /* which core dispatched this task and has not
                                     * yet saved its context; OS_CONFIG_CORE_COUNT
                                     * when the context is safely saved/not running */
    os_list_node_t  state_node;    /* links into one ready list or the delay list  */
    os_list_node_t  wait_node;     /* links into one object's waiter list          */
    os_list_t       *wait_list;    /* joined waiter list, NULL when waiting on none */
    bool            wait_signaled; /* wakeup reason: object signal vs timeout      */
    /* Kernel service task (timer/log): not the application's to pause or delete. Placed
     * against the bool above so both share one alignment hole - it costs no RAM at all. */
    bool            system_task;
    uint32_t        wait_data[2];  /* per-wait condition data read by match wakers */
    uint32_t        wait_result;   /* delivery stored by a match waker, else 0     */
    os_list_t       *woken_from;   /* waiter list a pending wake came from, until consumed */
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    uint32_t        base_priority; /* configured priority, restored once no held mutex needs a boost */
    os_list_t       owned_mutexes; /* mutexes currently locked by this task (priority inheritance) */
    /* Owner of the mutex this task is queued on, 0 when it is queued on none. The boost handed to
     * that owner by os_task_mutex_priority_inherit has to come back off when this task leaves the
     * queue by ANY route - timeout, pause or delete - and not only when the owner finally unlocks.
     * Recorded as an id rather than a pointer for the same reason os_mutex_unlock captures one: the
     * owner may be gone by the time it is read, and an id resolves to NULL where a pointer would
     * dangle. */
    uint32_t        pi_owner_id;

    /* Mutex this task is blocked on, NULL otherwise: the forward edge "this owner is itself waiting
     * for...". TWO walks follow it, and they want different subsets of it:
     *
     *   priority inheritance   every blocked waiter, timed or not. A task that will give up in
     *                          200 ms still blocks the owner for those 200 ms, and the inversion it
     *                          causes meanwhile is just as real.
     *   deadlock detection     infinite waiters only, which is what blocked_forever below says. A
     *                          timed waiter breaks any cycle it is part of by timing out, so
     *                          reporting it as a deadlock would be a false alarm.
     *
     * The edge used to be published only for the infinite case, because the deadlock walk was its
     * only reader and it lived in debug builds alone. Priority inheritance needs it in every build
     * and for every waiter, so it is unconditional now and the narrower question moved into its own
     * flag. */
    const os_mutex_t *blocked_on_mutex;

    /* Whether the wait recorded above is an OS_WAIT_FOREVER one. Placed against the other bools so
     * it shares their alignment hole and costs no RAM. */
    bool            blocked_forever;
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    /* One-word mailbox owned by os_notify.c; stored here because it belongs to the task. */
    os_notify_slot_t notify;
#endif

} os_task_tcb_t;

/* Defined in os_task.c, used by os_task_mutex.c. */
extern os_task_tcb_t* __IO os_task_current[OS_CONFIG_CORE_COUNT];

os_task_tcb_t* os_task_find_by_id(uint32_t id);
void           os_task_effective_priority_set(os_task_tcb_t *tcb, uint32_t new_priority);

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/* Defined in os_task_mutex.c, used by os_task.c when a waiter pauses or is deleted. */
void os_task_mutex_waiter_depart_tcb(os_task_tcb_t *tcb);
#endif

#endif /* OS_TASK_INTERNAL_H */
