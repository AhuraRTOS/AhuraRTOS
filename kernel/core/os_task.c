/**
 * @file os_task.c
 * @brief Task subsystem implementation: static TCB pool, O(1) list-based
 *        scheduling (per-priority ready lists + ready bitmap, round-robin by
 *        list rotation), delay list, blocking, idle task.
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

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* OS_TASK_PRIO_IDLE is no longer defined here: it is part of os_task_priority_t in ahura.h, so the
 * public enum names every level the scheduler has - the idle level below the user range, and
 * OS_TASK_PRIO_MAX above it - instead of the two ends being kernel secrets. */

#define OS_TASK_STACK_FILL_BYTE      0xA5U

/* Guard word written at the LOWEST address of every task stack - the last place a growing stack
 * reaches before it leaves its own memory. Deliberately the fill byte repeated, so a build with
 * OS_CONFIG_STACK_WATERMARK_ENABLE already writes it as part of its whole-stack fill and the two
 * features cost nothing together. */
#define OS_TASK_STACK_CANARY         0xA5A5A5A5UL

/* TCB back-references from the embedded intrusive list nodes. */
#define OS_TASK_TCB_FROM_NODE(node)      ((os_task_tcb_t *)(void *)((uint8_t *)(node) - offsetof(os_task_tcb_t, state_node)))
#define OS_TASK_TCB_FROM_WAIT_NODE(node) ((os_task_tcb_t *)(void *)((uint8_t *)(node) - offsetof(os_task_tcb_t, wait_node)))
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/* Mutex back-reference from its embedded owner_node (priority inheritance). */
#define OS_MUTEX_FROM_OWNER_NODE(node)   ((const os_mutex_t *)(const void *)((const uint8_t *)(node) - offsetof(os_mutex_t, owner_node)))
#endif

/* OS_TASK_DEADLOCK_MAX_DEPTH (os_internal.h) bounds how far a wait chain is followed: a cycle
 * that already formed among other tasks would otherwise be walked forever. Eight is far past any
 * sane nesting depth - a design needing more than a couple of held mutexes at once has a
 * lock-ordering problem this check cannot fix anyway. */

/* A floor under OS_CONFIG_MIN_STACK_SIZE, because every other check in the kernel measures against
 * it and so cannot catch it being wrong itself: os_task_create only tests
 * stack_bytes >= OS_CONFIG_MIN_STACK_SIZE, and the idle stacks below are SIZED from it. Set it too
 * low and os_arch_task_stack_initialize writes its initial frame - 17 words on v6m/v7m, 18 on v8m,
 * so 72 bytes at the top - straight past the bottom of every task stack in the system, at creation
 * time, silently. 128 leaves that frame plus room for a task to make a call; it is a sanity floor,
 * not a recommendation (the template ships 256, and most real tasks need considerably more).
 *
 * A _Static_assert rather than #if because a compile-time constant expression is exactly what this
 * is, and the message names the option - see os_timer.c for the same pattern on priorities. */
_Static_assert((OS_CONFIG_MIN_STACK_SIZE >= 128U) && ((OS_CONFIG_MIN_STACK_SIZE % 8U) == 0U),
               "OS_CONFIG_MIN_STACK_SIZE must be at least 128 and a multiple of 8");

/*
 * ***********************************************************************************************************
 * Types
 * ***********************************************************************************************************
*/

typedef struct
{
    const char      *name;         /* not read by the kernel: kept for debugger/trace visibility */
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
#endif
#if (OS_MUTEX_DEADLOCK_CHECK == 1)
    /* Mutex this task is blocked on, NULL otherwise: the forward edge a deadlock walk follows
     * ("this owner is itself waiting for..."). Debug builds only - see os_internal.h. */
    const os_mutex_t *blocked_on_mutex;
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    /* One-word mailbox owned by os_notify.c; stored here because it belongs to the task. */
    os_notify_slot_t notify;
#endif

} os_task_tcb_t;

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/* Every scheduling core owns one idle task and one current-task slot; the
 * task table and the ready/delay lists are shared between the cores (the
 * critical sections and the explicit PRIMASK guards protect them, plus the
 * kernel spinlock on multi-core builds). */
/* Table slots the kernel reserves for its own service tasks, on top of OS_CONFIG_MAX_USER_TASKS.
 *
 * This is what lets OS_CONFIG_MAX_USER_TASKS mean what an application would expect it to mean - how
 * many of ITS tasks may exist - instead of a budget it has to share with whichever kernel services
 * happen to be enabled. Turning on the log or the timer service no longer quietly costs the
 * application a task.
 *
 * Exactly one of tsk_main and tsk_test always exists (os_kernel.c compiles in one or the other,
 * never both), and each optional service task adds one. The idle tasks are NOT counted: they live
 * in os_task_idle_tcb below, outside this table, one per core. */
#define OS_TASK_SYSTEM_SLOTS  (1U + \
                               ((OS_CONFIG_TIMER_ENABLE == 1U) ? 1U : 0U) + \
                                                              ((OS_CONFIG_LOG_ENABLE   == 1U) ? 1U : 0U))

#define OS_TASK_TABLE_SIZE    (OS_CONFIG_MAX_USER_TASKS + OS_TASK_SYSTEM_SLOTS)

static uint8_t                 os_task_idle_stack[OS_CONFIG_CORE_COUNT][OS_CONFIG_MIN_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_tcb_t           os_task_idle_tcb[OS_CONFIG_CORE_COUNT];
static os_task_tcb_t           os_task_table[OS_TASK_TABLE_SIZE];
static uint32_t                os_task_next_id = 1U;

/* Written by PendSV and read from task/ISR context: the pointer itself is the
 * shared object (it changes on every context switch), not what it points to -
 * __IO placed after the '*' qualifies the pointer, not the pointed-to TCB. */
static os_task_tcb_t* __IO     os_task_current[OS_CONFIG_CORE_COUNT];

/* Scheduler structures: one FIFO ready list per priority plus a bitmap of
 * non-empty priorities (bit n = priority n has ready tasks), and one list of
 * finite-delay sleepers. OS_WAIT_FOREVER sleepers and suspended tasks sit in
 * no list; the running tasks and the idle tasks are never queued. */
static os_list_t               os_task_ready_list[OS_TASK_PRIO_MAX + 1U];
static uint32_t                os_task_ready_bitmap = 0U;
static os_list_t               os_task_delay_list;

/* Scheduler lock: while nonzero, the owning core defers its own context
 * switches with interrupts left fully live (os_kernel_lock). Per core and
 * read from ISR context (the tick's reschedule check, PendSV), hence __IO.
 *
 * A deferred switch is remembered rather than dropped: whoever wanted it sets
 * the pending flag, and the outermost os_kernel_unlock issues the PendSV
 * the lock swallowed. The flag is also set by the switch path itself, which
 * catches a PendSV that was already pending when the lock was taken. */

#if (OS_CONFIG_TIME_SLICE_TICKS > 0U)
/* Ticks left in the running task's round-robin quantum, per core. Reloaded
 * whenever a task is dispatched and counted down by the tick; an
 * equal-priority peer only takes over once it reaches zero. */
static __IO uint32_t           os_task_slice_left[OS_CONFIG_CORE_COUNT];
#endif

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static os_err_t      os_task_create_any(os_task_t *task, const os_task_config_t *config, bool system_task);
#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
static void           os_task_stack_fill(uint8_t *stack_base, size_t stack_bytes);
#endif
#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
static void           os_task_stack_guard_set(uint8_t *stack_base);
static void           os_task_stack_guard_check(const os_task_tcb_t *tcb, const uint32_t *stack_ptr);
#endif
static void           os_task_idle_entry(void *context);
static void           os_task_tcb_clear(os_task_tcb_t *tcb);
static os_task_tcb_t* os_task_find_by_id(uint32_t id);
static os_task_tcb_t* os_task_self_tcb(void);
static void           os_task_make_ready(os_task_tcb_t *tcb);
static void           os_task_unlink(os_task_tcb_t *tcb);
static void           os_task_wait_node_insert(os_list_t *waiters, os_task_tcb_t *tcb);
static void           os_task_switch_request(void);
static void           os_task_preempt_request(const os_task_tcb_t *tcb);
static uint32_t       os_task_running_core(const os_task_tcb_t *tcb);
static void           os_task_wake_compensate(os_task_tcb_t *tcb);
static void           os_task_wake_locked(os_task_tcb_t *tcb);
static void           os_task_effective_priority_set(os_task_tcb_t *tcb, uint32_t new_priority);
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static void           os_task_mutex_effective_recompute(os_task_tcb_t *owner);
static void           os_task_mutex_waiter_depart_tcb(os_task_tcb_t *tcb);
#endif

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Create a task using the provided configuration.
 *
 * OS_TASK_PRIO_IDLE and OS_TASK_PRIO_MAX (where the kernel's service tasks run
 * by default) are reserved: user tasks must use OS_TASK_PRIO_1_LOWEST to
 * OS_TASK_PRIO_30_HIGHEST.
 *
 * @param[out] task    Output task handle.
 * @param[in]  config  Task creation configuration.
 * @return os_err_t   Status code.
 */
os_err_t os_task_create(os_task_t *task, const os_task_config_t *config)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    /* Cast to the unsigned level the scheduler stores rather than comparing against the enum
     * directly: priority is a uint32_t and the enum's underlying type is implementation-defined,
     * so an uncast comparison is a sign-compare waiting to happen. */
    if ((config != NULL) &&
        (config->priority >= (uint32_t)OS_TASK_PRIO_1_LOWEST) &&
        (config->priority <= (uint32_t)OS_TASK_PRIO_30_HIGHEST))
    {
        status = os_task_create_any(task, config, false);
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Create a task without the user priority restriction; kernel use only.
 *
 * Tasks created here are marked as the kernel's own: os_task_pause and os_task_delete refuse them
 * (OS_ERR_BUSY), so an application cannot stop the timer or log service out from under the
 * kernel APIs that depend on it. tsk_main and tsk_test do NOT come through here - they are ordinary
 * application tasks and stay under the application's control.
 *
 * @param[out] task    Output task handle.
 * @param[in]  config  Task creation configuration.
 * @return os_err_t   Status code.
 */
os_err_t os_task_create_system(os_task_t *task, const os_task_config_t *config)
{
    return os_task_create_any(task, config, true);
}

/******************************************************************************************************/
/**
 * @brief Start a created task (make it ready to run).
 *
 * @param[in] task  Task handle.
 * @return os_err_t  Status code.
 */
os_err_t os_task_start(os_task_t *task)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((task != NULL) && (task->id != 0U))
    {
        os_task_tcb_t *tcb;

        os_critical_enter();

        tcb = os_task_find_by_id(task->id);
        if (tcb != NULL)
        {
            /* Reviving a task blocked on a primitive reads as a forced (spurious)
             * signal: the primitive re-checks its condition with its timeout budget
             * preserved, instead of misreporting OS_ERR_TIMEOUT - even on an
             * OS_WAIT_FOREVER wait. Unlink runs first (it inspects delay_ticks). */
            if (tcb->state == OS_TASK_STATE_BLOCKED)
            {
                os_task_unlink(tcb);
                tcb->wait_signaled = true;
            }
            else
            {
                os_task_unlink(tcb);
                tcb->delay_ticks = 0U;
            }

            /* Starting a running task keeps it running; anything else queues at the
             * tail of its priority's ready list. */
            if (os_task_running_core(tcb) < OS_CONFIG_CORE_COUNT)
            {
                tcb->state = OS_TASK_STATE_RUNNING;
            }
            else
            {
                os_task_make_ready(tcb);
            }

            /* Let the scheduler decide immediately in case the new task outranks the
             * current one (on this core, or via IPI on a core its affinity allows). */
            if (os_kernel_is_running())
            {
                os_task_preempt_request(tcb);
            }

            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Pause a task (NULL means current running task).
 *
 * @param[in] task  Task handle, or NULL for the calling task.
 * @return os_err_t  Status code.
 */
os_err_t os_task_pause(os_task_t *task)
{
    uint32_t       core   = os_arch_core_id_get();
    os_err_t      status = OS_ERR_NONE;
    os_task_tcb_t *tcb    = NULL;

    /* Task-only, like os_mutex_lock and os_notify_wait. Both of these can end up acting on the
     * CALLING task - and inside an interrupt "the calling task" is merely whichever task was
     * preempted, whether it was named by a handle or by NULL. Deleting or suspending that one
     * tears down the context the interrupt is about to return into, so the call is refused rather
     * than allowed to corrupt an innocent task.
     *
     * OS_ERR_ISR rather than INVALID_ARG: the handle is fine, the CONTEXT is not, and a caller
     * sent to inspect its arguments is looking in the wrong place. */

    if (os_arch_in_isr())
    {
        return OS_ERR_ISR;
    }

    os_critical_enter();

    if (task == NULL)
    {
        tcb = os_task_self_tcb();
    }
    else if (task->id == 0U)
    {
        status = OS_ERR_INVALID_ARG;
    }
    else
    {
        tcb = os_task_find_by_id(task->id);
    }

    if (status == OS_ERR_NONE)
    {
        /* The idle task is checked before the NULL case so the two keep the statuses they
         * always had: BUSY for idle, INVALID_ARG for an unresolvable handle. */
        if (tcb == &os_task_idle_tcb[core])
        {
            status = OS_ERR_BUSY;
        }
        else if (tcb == NULL)
        {
            status = OS_ERR_INVALID_ARG;
        }
        /* The kernel's own service tasks are off limits, for the same reason the idle task is:
         * the timer and log APIs are both built on one running, and suspending it turns
         * every call into a silent no-op that reports success. Kernel code that needs one parked
         * blocks it from the inside instead. */
        else if (tcb->system_task)
        {
            status = OS_ERR_BUSY;
        }
        else
        {
            bool is_self = (tcb == os_task_current[core]);

            /* A task executing on another core cannot be paused from here: its
             * context is live over there. */
            if (!is_self && (os_task_running_core(tcb) < OS_CONFIG_CORE_COUNT))
            {
                status = OS_ERR_BUSY;
            }
            /* Suspending the CALLING task means switching away from it, which is what
             * a scheduler lock defers - it would keep running while marked SUSPENDED.
             * Refused rather than deferred, so the caller learns it happened. Pausing
             * any OTHER task needs no switch and stays allowed. */
            else if (is_self && (os_kernel_lock_count[core] != 0U))
            {
                status = OS_ERR_BUSY;
            }
            else
            {
                /* This task may be READY only because a give/send/set_bits woke it and
                 * it has not yet run its retry loop to consume the notification (see
                 * os_task_wake_compensate): pass the notification on to another waiter
                 * of the same object before suspending, so it is not silently dropped. */
                os_task_wake_compensate(tcb);

                /* Suspending a task blocked on a primitive reads as a forced (spurious)
                 * signal, mirroring os_task_wake/os_task_start: on a later os_task_start
                 * the primitive re-checks its condition instead of misreporting
                 * OS_ERR_TIMEOUT, even on an OS_WAIT_FOREVER wait. */
                if (tcb->state == OS_TASK_STATE_BLOCKED)
                {
                    tcb->wait_signaled = true;
                }

                os_task_unlink(tcb);

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
                /* Off the waiter list now, so a mutex owner this task was boosting drops back to
                 * whatever the tasks STILL queued justify. Without this the owner would keep the
                 * departed waiter's priority until its next unlock, starving anything in between. */
                os_task_mutex_waiter_depart_tcb(tcb);
#endif

                tcb->delay_ticks = 0U;
                tcb->state       = OS_TASK_STATE_SUSPENDED;

                if (is_self && os_kernel_is_running())
                {
                    os_task_switch_request();
                }
            }
        }
    }

    os_critical_exit();

    return status;
}

/******************************************************************************************************/
/**
 * @brief Delete a task and release its TCB slot (NULL means current running task).
 *
 * @param[in,out] task  Task handle, or NULL for the calling task.
 * @return os_err_t    Status code.
 */
os_err_t os_task_delete(os_task_t *task)
{
    uint32_t      core = os_arch_core_id_get();
    os_task_tcb_t *tcb;
    bool          is_self;

    /* Task-only, like os_mutex_lock and os_notify_wait. Both of these can end up acting on the
     * CALLING task - and inside an interrupt "the calling task" is merely whichever task was
     * preempted, whether it was named by a handle or by NULL. Deleting or suspending that one
     * tears down the context the interrupt is about to return into, so the call is refused rather
     * than allowed to corrupt an innocent task.
     *
     * OS_ERR_ISR rather than INVALID_ARG: the handle is fine, the CONTEXT is not, and a caller
     * sent to inspect its arguments is looking in the wrong place. */

    if (os_arch_in_isr())
    {
        return OS_ERR_ISR;
    }

    os_critical_enter();

    if (task == NULL)
    {
        tcb = os_task_self_tcb();
        if ((tcb == NULL) || (tcb == &os_task_idle_tcb[core]))
        {
            os_critical_exit();
            return (tcb == &os_task_idle_tcb[core]) ? OS_ERR_BUSY : OS_ERR_INVALID_ARG;
        }

        /* Re-resolve through the table: confirms the running task really owns
         * a live table slot before its TCB is torn down. */
        tcb = os_task_find_by_id(tcb->id);
    }
    else if (task->id == 0U)
    {
        os_critical_exit();
        return OS_ERR_INVALID_ARG;
    }
    else
    {
        tcb = os_task_find_by_id(task->id);
    }

    if (tcb == NULL)
    {
        os_critical_exit();
        return OS_ERR_INVALID_ARG;
    }

    /* See os_task_pause: a kernel service task is not the application's to tear down. Deleting one
     * would also release its TCB slot and leave the timer/log registries pointing at a task
     * that no longer exists. */
    if (tcb->system_task)
    {
        os_critical_exit();
        return OS_ERR_BUSY;
    }

    is_self = (tcb == os_task_current[core]);

    /* A task executing on another core cannot be deleted from here: its
     * context is live over there. */
    if (!is_self && (os_task_running_core(tcb) < OS_CONFIG_CORE_COUNT))
    {
        os_critical_exit();
        return OS_ERR_BUSY;
    }

    /* See os_task_pause: deleting the CALLING task requires switching away
     * from it, and a locked scheduler cannot. Tearing its TCB down and then
     * letting it run on would be far worse than refusing. */
    if (is_self && (os_kernel_lock_count[core] != 0U))
    {
        os_critical_exit();
        return OS_ERR_BUSY;
    }

    /* See os_task_pause: pass on an unconsumed wake before this task's TCB
     * is torn down, so the notification is not silently dropped. */
    os_task_wake_compensate(tcb);

    os_task_unlink(tcb);

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    /* Before the TCB is cleared, while pi_owner_id still says whose boost this task was holding
     * up. See os_task_pause for why departure has to trigger the recompute; os_task_tcb_clear
     * below handles the mutexes this task OWNED, which is the opposite direction. */
    os_task_mutex_waiter_depart_tcb(tcb);
#endif

    os_task_tcb_clear(tcb);

    if (task != NULL)
    {
        task->id = 0U;
    }

    if (is_self)
    {
        /* The calling task ceases to exist: drop the current pointer so the
         * switch-out path does not touch the freed TCB, then switch away. */
        os_task_current[core] = NULL;
        if (os_kernel_is_running())
        {
            os_task_switch_request();
        }
    }

    os_critical_exit();
    return OS_ERR_NONE;
}

/******************************************************************************************************/
/**
 * @brief Yield the processor to another ready task of equal or higher priority.
 *
 * @return None.
 */
void os_task_yield(void)
{
    if (os_kernel_is_running() && !os_arch_in_isr())
    {
        os_task_switch_request();
    }
}

/******************************************************************************************************/
/**
 * @brief Change a task's priority (NULL means the calling task).
 *
 * Only user priorities are accepted: OS_TASK_PRIO_IDLE belongs to the idle task and
 * OS_TASK_PRIO_MAX to the kernel service tasks, so neither is the application's to hand out. The
 * idle task and the kernel's own tasks are refused outright, exactly as os_task_pause and
 * os_task_delete refuse them.
 *
 * Takes effect immediately, whatever the task is doing: a READY task moves between ready lists, a
 * RUNNING one may be preempted on the spot, and a task blocked on a mutex, semaphore, queue or
 * event is re-sorted in that object's waiter list so it is woken in its new priority order.
 *
 * A task currently holding a priority-inheritance boost keeps it. The new value becomes its base
 * priority - what it returns to when it releases the mutex - and only takes effect now if it is
 * HIGHER than the boost. Lowering a boosted task immediately would hand back the very inversion
 * the boost exists to prevent.
 *
 * @param[in,out] task      Task handle, or NULL for the calling task.
 * @param[in]     priority  New priority: OS_TASK_PRIO_1..OS_TASK_PRIO_30 (or any value in that range).
 * @return os_err_t  OK; INVALID_ARG for an unknown handle or an out-of-range priority;
 *                    BUSY for the idle task or a kernel service task.
 */
os_err_t os_task_priority_set(os_task_t *task, os_task_priority_t priority)
{
    uint32_t      core  = os_arch_core_id_get();
    uint32_t      value = (uint32_t)priority;
    os_task_tcb_t *tcb;

    /* Cast for the same reason as in os_task_create above. */
    if ((value < (uint32_t)OS_TASK_PRIO_1_LOWEST) || (value > (uint32_t)OS_TASK_PRIO_30_HIGHEST))
    {
        return OS_ERR_INVALID_ARG;
    }

    os_critical_enter();

    tcb = (task == NULL) ? os_task_self_tcb()
                         : ((task->id == 0U) ? NULL : os_task_find_by_id(task->id));

    if ((tcb == NULL) || (tcb == &os_task_idle_tcb[core]))
    {
        os_critical_exit();
        return (tcb == NULL) ? OS_ERR_INVALID_ARG : OS_ERR_BUSY;
    }

    if (tcb->system_task)
    {
        os_critical_exit();
        return OS_ERR_BUSY;
    }

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    {
        /* Boosted exactly when the effective priority has been raised above the base. */
        bool boosted = (tcb->priority > tcb->base_priority);

        tcb->base_priority = value;

        if (!boosted || (value > tcb->priority))
        {
            os_task_effective_priority_set(tcb, value);
        }
    }
#else
    os_task_effective_priority_set(tcb, value);
#endif

    os_critical_exit();
    return OS_ERR_NONE;
}

/******************************************************************************************************/
/**
 * @brief Get a task's priority (NULL means the calling task).
 *
 * Reports the priority the application set, not a priority-inheritance boost that may be in force
 * right now. That is what makes the value safe to save and restore around a section: a boost is
 * transient and belongs to the kernel, and writing one back later would make it permanent.
 *
 * @param[in]  task          Task handle, or NULL for the calling task.
 * @param[out] priority_out  Receives the task's priority.
 * @return os_err_t  OK, or INVALID_ARG for an unknown handle or a NULL output.
 */
os_err_t os_task_priority_get(const os_task_t *task, os_task_priority_t *priority_out)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if (priority_out != NULL)
    {
        const os_task_tcb_t *tcb;

        os_critical_enter();

        tcb = (task == NULL) ? os_task_self_tcb()
                             : ((task->id == 0U) ? NULL : os_task_find_by_id(task->id));

        if (tcb != NULL)
        {
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
            *priority_out = (os_task_priority_t)tcb->base_priority;
#else
            *priority_out = (os_task_priority_t)tcb->priority;
#endif
            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Get the current state of a task.
 *
 * @param[in] task  Task handle, or NULL for the calling task.
 * @return os_task_state_t  Task state; OS_TASK_STATE_INACTIVE for unknown handles.
 */
os_task_state_t os_task_state_get(const os_task_t *task)
{
    os_task_tcb_t   *tcb;
    os_task_state_t state;

    os_critical_enter();

    if (task == NULL)
    {
        tcb = os_task_self_tcb();
    }
    else if (task->id == 0U)
    {
        tcb = NULL;
    }
    else
    {
        tcb = os_task_find_by_id(task->id);
    }

    state = (tcb == NULL) ? OS_TASK_STATE_INACTIVE : tcb->state;

    os_critical_exit();
    return state;
}

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the minimum stack headroom a task has ever had, in bytes.
 *
 * Counts the fill-pattern bytes still untouched at the bottom of the stack,
 * i.e. the smallest distance the stack pointer has ever had to the overflow
 * boundary since the task was created.
 *
 * @param[in]  task            Task handle, or NULL for the calling task.
 * @param[out] min_free_bytes  Worst-case remaining stack in bytes.
 * @return os_err_t  Status code.
 */
os_err_t os_task_stack_watermark_get(const os_task_t *task, size_t *min_free_bytes)
{
    os_err_t status      = OS_ERR_INVALID_ARG;
    uint8_t  *stack_base  = NULL;
    size_t    stack_bytes = 0U;

    if (min_free_bytes != NULL)
    {
        os_task_tcb_t *tcb;

        os_critical_enter();

        if (task == NULL)
        {
            tcb = os_task_self_tcb();
        }
        else if (task->id == 0U)
        {
            tcb = NULL;
        }
        else
        {
            tcb = os_task_find_by_id(task->id);
        }

        if ((tcb != NULL) && (tcb->stack_base != NULL))
        {
            stack_base  = tcb->stack_base;
            stack_bytes = tcb->stack_bytes;

            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    /* The scan runs outside the critical section, and only once the lookup above succeeded: for a
     * task that stays alive, bytes below the deepest stack excursion are never rewritten, so the
     * walk needs no lock.
     *
     * What it is NOT safe against is the task ceasing to exist underneath it. Between the
     * os_critical_exit() above and the scan below, the TCB can be deleted and the handle re-created
     * - which re-fills that same array with the fill byte - and on SMP a peer core can run exactly
     * that concurrently. The number would then describe a task that no longer exists, reported as
     * though it were current. So the slot is re-resolved afterwards and the result is only
     * published if the id still names the same live task. */
    if (status == OS_ERR_NONE)
    {
        size_t               index;
        const os_task_tcb_t *recheck;

        for (index = 0U; index < stack_bytes; index++)
        {
            if (stack_base[index] != OS_TASK_STACK_FILL_BYTE)
            {
                break;
            }
        }

        os_critical_enter();

        recheck = (task == NULL) ? os_task_self_tcb() : os_task_find_by_id(task->id);

        /* Same slot, same identity, same buffer: anything else means the task was torn down
         * mid-scan and the count belongs to nothing. Reported as INVALID_ARG, the status this call
         * already uses for a handle that resolves to no live task - a caller retrying gets a real
         * answer, where a stale number would never announce itself as wrong. */
        if ((recheck != NULL) && (recheck->stack_base == stack_base) &&
            (recheck->stack_bytes == stack_bytes))
        {
            *min_free_bytes = index;
        }
        else
        {
            status = OS_ERR_INVALID_ARG;
        }

        os_critical_exit();
    }

    return status;
}
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief The calling task's TCB handle, or NULL when there is no task identity to notify.
 *
 * The notification mailbox belongs to a real task: the idle task and pre-scheduler code have no
 * identity of their own to receive one, and both answer NULL here rather than handing out a slot
 * nobody could ever wait on. Call inside a critical section (os_notify.c holds one).
 *
 * @return void*  Opaque TCB handle for os_task_notify_slot, or NULL.
 */
void* os_task_tcb_current(void)
{
    uint32_t      core    = os_arch_core_id_get();
    os_task_tcb_t *current = os_task_current[core];

    if ((current == NULL) || (current == &os_task_idle_tcb[core]))
    {
        return NULL;
    }

    return (void *)current;
}

/******************************************************************************************************/
/**
 * @brief The notification mailbox embedded in a resolved task.
 *
 * The mailbox lives in the TCB because it belongs to the task, but what a notification means is
 * os_notify.c's business - so the storage is handed out and nothing here reads or writes it.
 *
 * @param[in] tcb_handle  Handle from os_task_tcb_resolve or os_task_tcb_current (never NULL).
 * @return os_notify_slot_t*  That task's mailbox.
 */
os_notify_slot_t* os_task_notify_slot(void *tcb_handle)
{
    return &((os_task_tcb_t *)tcb_handle)->notify;
}

/******************************************************************************************************/
/**
 * @brief Whether a resolved task is currently BLOCKED (ISR-safe, caller holds a critical section).
 *
 * @param[in] tcb_handle  Handle from os_task_tcb_resolve (never NULL).
 * @return bool  True when the task is blocked on a wait or a delay.
 */
bool os_task_tcb_is_blocked(const void *tcb_handle)
{
    return (((const os_task_tcb_t *)tcb_handle)->state == OS_TASK_STATE_BLOCKED);
}
#endif /* OS_CONFIG_NOTIFY_ENABLE */

/******************************************************************************************************/
/**
 * @brief Update task delays with elapsed kernel ticks; wakes expired tasks.
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks.
 * @return None.
 */
void os_task_tick_update(uint32_t elapsed_ticks)
{
    uint32_t       mask_state;
    os_list_node_t *node;

    if (elapsed_ticks == 0U)
    {
        return;
    }

    /* Only finite-delay sleepers live in the delay list, so the cost is
     * O(sleeping tasks), not O(task table). OS_WAIT_FOREVER sleepers are in
     * no list and only os_task_wake releases them. The kernel mask is raised
     * so a preempting ISR cannot resize the list mid-walk, and on multi-core
     * builds the cross-core spinlock additionally excludes the other cores'
     * os_task_wait_begin/os_task_sleep_ticks callers, who insert into this
     * same shared delay list under os_critical_enter. */
    mask_state = os_arch_kernel_mask_save();
    os_critical_multicore_lock();

    node = os_task_delay_list.head;
    while (node != NULL)
    {
        os_list_node_t *next_node = node->next; /* the node may leave the list below */
        os_task_tcb_t  *tcb       = OS_TASK_TCB_FROM_NODE(node);

        if (tcb->delay_ticks > elapsed_ticks)
        {
            tcb->delay_ticks -= elapsed_ticks;
        }
        else
        {
            /* Unlink leaves the delay list and any object waiter list;
             * wait_signaled stays false = the wait timed out. */
            tcb->delay_ticks = 0U;
            os_task_unlink(tcb);
            os_task_make_ready(tcb);
        }

        node = next_node;
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);
}

/******************************************************************************************************/
/**
 * @brief Return ticks until the next finite-delay sleeper wakes (tickless planning).
 *
 * OS_WAIT_FOREVER sleepers are in no list and never bound the idle time.
 *
 * @return uint32_t  Minimum remaining delay in ticks, UINT32_MAX when no task is delaying.
 */
uint32_t os_task_next_delay_ticks_get(void)
{
    uint32_t       mask_state = os_arch_kernel_mask_save();
    uint32_t       minimum    = UINT32_MAX;
    os_list_node_t *node;

    /* See os_task_tick_update: the cross-core spinlock excludes the other
     * cores' insertions into this same shared delay list. */
    os_critical_multicore_lock();

    for (node = os_task_delay_list.head; node != NULL; node = node->next)
    {
        const os_task_tcb_t *tcb = OS_TASK_TCB_FROM_NODE(node);

        if (tcb->delay_ticks < minimum)
        {
            minimum = tcb->delay_ticks;
        }
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);

    return minimum;
}

/******************************************************************************************************/
/**
 * @brief Block the calling task for the given number of ticks.
 *
 * @param[in] ticks  Number of ticks to sleep; 0 returns immediately;
 *                   OS_WAIT_FOREVER blocks until os_task_wake is called.
 * @return None.
 */
void os_task_sleep_ticks(uint32_t ticks)
{
    uint32_t      core;
    os_task_tcb_t *current;

    if (ticks == 0U)
    {
        return;
    }

    os_critical_enter();

    core    = os_arch_core_id_get();
    current = os_task_current[core];

    /* Only a real task in task context can block, and only with the scheduler
     * open: marking a task BLOCKED whose switch-out the lock then defers would
     * leave it running in a state the kernel believes is parked. Callers see
     * the same "could not block" behaviour they get from an ISR - os_delay
     * busy-waits instead. */
    if (!os_kernel_is_running() || os_arch_in_isr() ||
        (os_kernel_lock_count[core] != 0U) ||
        (current == NULL) || (current == &os_task_idle_tcb[core]))
    {
        os_critical_exit();
        return;
    }

    current->delay_ticks = ticks;
    current->state       = OS_TASK_STATE_BLOCKED;

    /* Finite delays wait in the delay list; forever-sleepers stay out of
     * every list until os_task_wake releases them. The running task is in
     * no ready list, so no unlink is needed first. */
    if (ticks != OS_WAIT_FOREVER)
    {
        os_list_push_back(&os_task_delay_list, &current->state_node);
    }

    /* The switch is taken as soon as the critical section is left. */
    os_task_switch_request();
    os_critical_exit();
}

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by id; no-op for other states.
 *
 * Safe from interrupt and task context. Used by the kernel service modules
 * to release their OS_WAIT_FOREVER sleepers.
 *
 * @param[in] task_id  Id of the task to wake.
 * @return None.
 */
void os_task_wake(uint32_t task_id)
{
    os_task_tcb_t *tcb;

    if (task_id == 0U)
    {
        return;
    }

    os_critical_enter();
    tcb = os_task_find_by_id(task_id);
    os_task_wake_locked(tcb);
    os_critical_exit();
}

/******************************************************************************************************/
/**
 * @brief Resolve a task id to an opaque handle once, for os_task_wake_tcb (ISR-safe).
 *
 * The handle is a raw TCB pointer; it stays valid for the task's lifetime, so callers that
 * never delete the target (e.g. the kernel's own timer service task) may resolve it
 * once at init and cache it, skipping the O(OS_TASK_TABLE_SIZE) id scan on every later wake.
 *
 * @param[in] task_id  Id to resolve.
 * @return void*  Opaque handle, or NULL when the id does not exist.
 */
void* os_task_tcb_resolve(uint32_t task_id)
{
    return (void *)os_task_find_by_id(task_id);
}

/******************************************************************************************************/
/**
 * @brief Wake a BLOCKED task by its os_task_tcb_resolve handle (ISR-safe), skipping both the
 *        id lookup and the nested critical section os_task_wake pays: the caller must
 *        already hold the kernel mask and, on multi-core builds, the cross-core spinlock
 *        (os_critical_multicore_lock) - exactly what the tick-time timer wake path
 *        already holds around its registry walk. No-op for a NULL handle or a task not
 *        currently BLOCKED, same as os_task_wake.
 *
 * @param[in] tcb_handle  Handle from os_task_tcb_resolve.
 * @return None.
 */
void os_task_wake_tcb(void *tcb_handle)
{
    os_task_wake_locked((os_task_tcb_t *)tcb_handle);
}

/******************************************************************************************************/
/**
 * @brief Queue the calling task on an object's waiter list and block it (priority ordered,
 *        FIFO among equals). Call from task context inside a critical section, right after
 *        finding the object unavailable; the switch happens when the caller leaves the
 *        critical section. On resume, os_task_wait_signaled tells signal from timeout.
 *
 * @param[in,out] waiters        The object's waiter list.
 * @param[in]     timeout_ticks  Wait budget in ticks; OS_WAIT_FOREVER waits indefinitely.
 * @return None.
 */
void os_task_wait_begin(os_list_t *waiters, uint32_t timeout_ticks)
{
    uint32_t       core    = os_arch_core_id_get();
    os_task_tcb_t  *current = os_task_current[core];

    /* Only a real task can wait; the caller checks os_internal_can_block. */
    if ((current == NULL) || (current == &os_task_idle_tcb[core]))
    {
        return;
    }

    os_task_wait_node_insert(waiters, current);

    current->wait_list     = waiters;
    current->wait_signaled = false;
    /* Whatever wake put this task back on the CPU is spent - it did not
     * satisfy the caller, which is why it is blocking again. Leaving the
     * source list here would let os_task_wake_compensate pass a wake on to
     * an object this task no longer has anything to do with. */
    current->woken_from    = NULL;
    current->delay_ticks   = timeout_ticks;
    current->state         = OS_TASK_STATE_BLOCKED;

    if (timeout_ticks != OS_WAIT_FOREVER)
    {
        os_list_push_back(&os_task_delay_list, &current->state_node);
    }

    os_task_switch_request();
}

/******************************************************************************************************/
/**
 * @brief Drop the calling task's pending-wake state: call on every path where a blocking
 *        primitive stops retrying and returns to its caller, signaled or not.
 *
 * This is what keeps os_task_wake_compensate's window narrow. Until this call the task counts as
 * holding an unconsumed wake that a pause or delete must pass on; from here the primitive has
 * finished with the object, so the notification is spent and the list it came from is no longer
 * this task's business - it may not even exist by the time the task blocks on something else.
 *
 * No-op from interrupt context: an ISR borrows the interrupted task's identity and must not
 * discard that task's own wait state.
 *
 * @return None.
 */
void os_task_wait_end(void)
{
    os_task_tcb_t *current;

    if (os_arch_in_isr())
    {
        return;
    }

    current = os_task_current[os_arch_core_id_get()];

    /* No lock needed: both fields are only ever written by a waker, which
     * acts on BLOCKED tasks alone, and by the task itself - and the task
     * running this call is neither blocked nor wakeable right now. */
    if (current != NULL)
    {
        current->wait_signaled = false;
        current->woken_from    = NULL;
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
        /* Backstop only. Every path that leaves a mutex wait without acquiring calls
         * os_task_mutex_waiter_depart first, which is what actually releases the boost; this makes
         * sure no exit route can leave a stale id behind to be spent on the wrong owner later. */
        current->pi_owner_id   = 0U;
#endif
#if (OS_MUTEX_DEADLOCK_CHECK == 1)
        /* Cleared here rather than in os_mutex.c because every way out of a wait passes through
         * this call, so no exit path can leave a stale edge behind for a later walk to follow. */
        current->blocked_on_mutex = NULL;
#endif
    }
}

/******************************************************************************************************/
/**
 * @brief After resuming from os_task_wait_begin: true when the object signaled the task,
 *        false when the wait timed out.
 *
 * @return bool  Wakeup reason.
 */
bool os_task_wait_signaled(void)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    return (current != NULL) ? current->wait_signaled : false;
}

/******************************************************************************************************/
/**
 * @brief Attach two words of per-wait condition data to the calling task; call inside the
 *        critical section right before os_task_wait_begin. A waker's match callback reads
 *        them through os_task_waiters_wake_match. The pending result is reset here.
 *
 * @param[in] data0  First condition word (meaning owned by the primitive).
 * @param[in] data1  Second condition word.
 * @return None.
 */
void os_task_wait_data_set(uint32_t data0, uint32_t data1)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    if (current != NULL)
    {
        current->wait_data[0] = data0;
        current->wait_data[1] = data1;
        current->wait_result  = 0U;
    }
}

/******************************************************************************************************/
/**
 * @brief After a signaled resume: the delivery a match waker stored for this task, or 0 when
 *        the wake came from any other source (plain wake-one, forced wake, timeout).
 *
 * @return uint32_t  Stored wait result.
 */
uint32_t os_task_wait_result_get(void)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    return (current != NULL) ? current->wait_result : 0U;
}

/******************************************************************************************************/
/**
 * @brief Wake every waiter whose stored condition the callback confirms, storing each
 *        waiter's delivery for os_task_wait_result_get (call inside a critical section;
 *        ISR-safe). The walk is unlink-safe and evaluates in list order, i.e. highest
 *        priority first.
 *
 * @param[in,out] waiters  The object's waiter list.
 * @param[in]     match    Condition callback; true = wake this waiter.
 * @param[in]     context  Opaque pointer handed to the callback.
 * @return uint32_t  Number of waiters woken.
 */
uint32_t os_task_waiters_wake_match(os_list_t *waiters, os_task_wait_match_fn match, void *context)
{
    os_list_node_t *node  = waiters->head;
    uint32_t       woken  = 0U;

    while (node != NULL)
    {
        os_list_node_t *next_node = node->next; /* the node may leave the list below */
        os_task_tcb_t  *tcb       = OS_TASK_TCB_FROM_WAIT_NODE(node);
        uint32_t       result     = 0U;

        if (match(tcb->wait_data[0], tcb->wait_data[1], context, &result))
        {
            uint32_t remaining = tcb->delay_ticks;

            os_task_unlink(tcb);              /* leaves the delay list and the waiter list */
            tcb->delay_ticks   = remaining;   /* keep the timeout budget for the retry     */
            tcb->wait_signaled = true;
            tcb->wait_result   = result;
            tcb->woken_from    = waiters;     /* until consumed: see os_task_wake_compensate */
            os_task_make_ready(tcb);

            if (os_kernel_is_running())
            {
                os_task_preempt_request(tcb);
            }

            woken++;
        }

        node = next_node;
    }

    return woken;
}

/******************************************************************************************************/
/**
 * @brief Wake the highest-priority task waiting on an object (call inside a critical
 *        section; ISR-safe). The task resumes with its remaining timeout preserved and
 *        os_task_wait_signaled() true, so it re-checks the object's condition.
 *
 * @param[in,out] waiters  The object's waiter list.
 * @return bool  True when a task was woken.
 */
bool os_task_waiters_wake_one(os_list_t *waiters)
{
    os_list_node_t *node = waiters->head;
    os_task_tcb_t  *tcb;
    uint32_t       remaining;

    if (node == NULL)
    {
        return false;
    }

    tcb       = OS_TASK_TCB_FROM_WAIT_NODE(node);
    remaining = tcb->delay_ticks;

    os_task_unlink(tcb);              /* leaves the delay list and the waiter list */
    tcb->delay_ticks   = remaining;   /* keep the timeout budget for the retry     */
    tcb->wait_signaled = true;
    tcb->woken_from    = waiters;     /* until consumed: see os_task_wake_compensate */
    os_task_make_ready(tcb);

    if (os_kernel_is_running())
    {
        os_task_preempt_request(tcb);
    }

    return true;
}

/******************************************************************************************************/
/**
 * @brief Wake every task waiting on an object (call inside a critical section; ISR-safe).
 *        Used by events, where each waiter re-evaluates its own bit condition.
 *
 * @param[in,out] waiters  The object's waiter list.
 * @return None.
 */
void os_task_waiters_wake_all(os_list_t *waiters)
{
    while (os_task_waiters_wake_one(waiters))
    {
    }
}

/******************************************************************************************************/
/**
 * @brief Get the id of the current task, 0 when idle/none/pre-scheduler.
 *
 * @return uint32_t  Current task id.
 */
uint32_t os_task_current_id_get(void)
{
    os_task_tcb_t *tcb = os_task_current[os_arch_core_id_get()];

    return (tcb == NULL) ? 0U : tcb->id;
}

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Link a just-acquired mutex into the calling task's owned-mutex list.
 *
 * Only an identifiable task gets an entry. A mutex stores nothing but its owner's id, so an owner
 * that cannot be found by id again at unlock time could never have its list entry removed either -
 * and id 0 (the idle task, or code running before the scheduler starts) is already outside
 * priority inheritance, since os_task_mutex_priority_inherit cannot resolve it. Such an owner
 * simply holds the mutex without owning a list entry for it.
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
 * @brief Boost owner_task_id's effective priority to the calling (waiting) task's effective
 *        priority when that is higher. Single-level only: does not chase what the owner may
 *        itself be blocked on.
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
            os_task_effective_priority_set(owner, current->priority);
        }
    }
}

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
 * @brief Release the priority boost tcb handed a mutex owner, now that it has left the waiter queue.
 *
 * Call AFTER the task has been unlinked from the waiter list: the recompute is a max() over the
 * tasks still queued, so running it while this one is still linked would just re-derive the boost
 * being dropped. Caller must hold a critical section.
 *
 * @param[in,out] tcb  Departing waiter.
 * @return None.
 */
static void os_task_mutex_waiter_depart_tcb(os_task_tcb_t *tcb)
{
    uint32_t owner_id = tcb->pi_owner_id;

    if (owner_id != 0U)
    {
        os_task_tcb_t *owner;

        /* Cleared first: this task owes the owner nothing from here on, and clearing before the
         * lookup means an owner that has since been deleted still ends the debt. */
        tcb->pi_owner_id = 0U;

        owner = os_task_find_by_id(owner_id);
        if (owner != NULL)
        {
            os_task_mutex_effective_recompute(owner);
        }
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
 * @brief Unlink a released mutex from its owner's owned-mutex list and recompute the owner's
 *        effective priority as max(base_priority, highest waiter still queued on any mutex it
 *        still holds) - correct even when the task holds several mutexes at once.
 *
 * The owner comes from the id passed in, never from the running task: os_mutex_unlock reaches here
 * for a non-owner too, and working on the caller would splice this node out of the REAL owner's
 * list while updating the CALLER's head and tail - two corrupted lists, plus a boost left behind
 * that nothing can recompute away. An unresolvable owner has nothing to undo: id 0 never had a
 * list entry, and a deleted task's entries were detached by os_task_tcb_clear.
 *
 * @param[in]     owner_id    Id the mutex recorded for its owner (captured before the unlock cleared it).
 * @param[in,out] owner_node  The mutex's own owner_node link (already unlocked by the caller).
 * @return None.
 */
void os_task_mutex_owner_unlink_and_reprioritize(uint32_t owner_id, os_list_node_t *owner_node)
{
    os_task_tcb_t *owner = os_task_find_by_id(owner_id);

    if (owner == NULL)
    {
        return;
    }

    os_list_remove(&owner->owned_mutexes, owner_node);
    os_task_mutex_effective_recompute(owner);
}
#endif /* OS_CONFIG_MUTEX_ENABLE */

#if (OS_MUTEX_DEADLOCK_CHECK == 1)

/* Post-mortem record for the debugger; see os_internal.h for why an assertion cannot carry this
 * itself. Zero-initialised, so requested == NULL means nothing has been detected. */
os_task_deadlock_report_t os_task_deadlock_report;

/******************************************************************************************************/
/**
 * @brief Record the mutex the calling task is about to block on. Cleared by os_task_wait_end().
 *
 * @param[in] mutex  Mutex about to be waited on, NULL to clear.
 * @return None.
 */
void os_task_mutex_blocked_on_set(const os_mutex_t *mutex)
{
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];

    if (current != NULL)
    {
        current->blocked_on_mutex = mutex;
    }
}

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
            os_task_deadlock_report.waiter_name  = current->name;
            os_task_deadlock_report.waiter_id    = current->id;
            os_task_deadlock_report.owner_name   = (first_owner != NULL) ? first_owner->name : NULL;
            os_task_deadlock_report.owner_id     = mutex->owner_id;
            os_task_deadlock_report.cycle_length = depth + 1U;

            cycle = true;
            break;
        }

        /* NULL for a task waiting with a timeout, which ends the walk: it will give up and break
         * the chain, so it cannot be part of a real deadlock (os_mutex.c publishes the edge only
         * for OS_WAIT_FOREVER). */
        link = owner->blocked_on_mutex;
    }

    return cycle;
}
#endif /* OS_MUTEX_DEADLOCK_CHECK */

/******************************************************************************************************/
/**
 * @brief Check whether the calling core's idle task is currently running (ISR-safe).
 *
 * @return bool  True when the current task is this core's idle task.
 */
bool os_task_current_is_idle(void)
{
    uint32_t core = os_arch_core_id_get();

    return (os_task_current[core] == &os_task_idle_tcb[core]);
}

/******************************************************************************************************/
/**
 * @brief Whether a PendSV on this core would actually switch or round-robin (ISR-safe).
 *
 * The running task is never queued, so a bit above its priority means a real preemption is due,
 * and its own priority bit means a peer is waiting to round-robin - which only counts once the
 * time slice is used up. A locked scheduler answers false outright. Lets the tick skip the PendSV
 * round trip on a tick that would not switch, which with a quantum above 1 is most of them.
 *
 * @return bool  True when a reschedule is currently possible on this core.
 */
bool os_task_reschedule_possible(void)
{
    uint32_t       mask_state = os_arch_kernel_mask_save();
    uint32_t       core;
    os_task_tcb_t  *current;
    bool           result;

    os_critical_multicore_lock();

    core    = os_arch_core_id_get();
    current = os_task_current[core];

    if (os_kernel_lock_count[core] != 0U)
    {
        result = false;
    }
    else if (current == NULL)
    {
        result = (os_task_ready_bitmap != 0U);
    }
    else
    {
        /* Strictly above the running priority: a preemption, always due.
         * OS_TASK_PRIO_MAX has nothing above it, and (1UL << 32) would be
         * undefined, so that level takes the empty mask. */
        uint32_t above_mask = (current->priority < OS_TASK_PRIO_MAX)
                              ? ~((1UL << (current->priority + 1U)) - 1U)
                              : 0U;

        result = ((os_task_ready_bitmap & above_mask) != 0U);

#if (OS_CONFIG_TIME_SLICE_TICKS > 0U)
        /* Equal priority: rotate only when the quantum has run out. At the
         * default quantum of one tick this is true on every tick, i.e. the
         * unconditional round-robin the kernel has always done. */
        if (!result && (os_task_slice_left[core] == 0U))
        {
            result = ((os_task_ready_bitmap & (1UL << current->priority)) != 0U);
        }
#endif
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);

    return result;
}

/******************************************************************************************************/
/**
 * @brief Count elapsed ticks against the running task's time slice (ISR context, per core).
 *
 * Called from every core's tick, and once per announced window on the tickless path, before the
 * reschedule check that consumes the result. Compiles to nothing when round-robin time slicing is
 * turned off (OS_CONFIG_TIME_SLICE_TICKS 0).
 *
 * @param[in] elapsed_ticks  Ticks that have passed since the last call.
 * @return None.
 */
void os_task_slice_tick(uint32_t elapsed_ticks)
{
#if (OS_CONFIG_TIME_SLICE_TICKS > 0U)
    uint32_t core = os_arch_core_id_get();
    uint32_t left = os_task_slice_left[core];

    /* Only this core writes the counter (its tick and its PendSV), and a
     * 32-bit store is indivisible, so the read-modify-write needs no lock. */
    os_task_slice_left[core] = (left > elapsed_ticks) ? (left - elapsed_ticks) : 0U;
#else
    (void)elapsed_ticks;
#endif
}

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Change which cores a task may run on.
 *
 * The task keeps its place in the shared ready/delay lists; each core simply
 * skips tasks its bit is missing from. When the task is currently executing
 * on a core the new mask excludes, that core is asked to reschedule.
 *
 * @param[in] task           Task handle.
 * @param[in] core_affinity  Bitmask of allowed cores; OS_TASK_CORE_ANY (0) = any core.
 * @return os_err_t  Status code.
 */
os_err_t os_task_core_affinity_set(os_task_t *task, uint32_t core_affinity)
{
    os_task_tcb_t *tcb;
    uint32_t      running;

    /* Bits naming cores that do not exist are rejected rather than ignored. An empty mask is
     * OS_TASK_CORE_ANY and passes: it is a real request, not a missing one. */
    if ((core_affinity >> OS_CONFIG_CORE_COUNT) != 0U)
    {
        return OS_ERR_INVALID_ARG;
    }

    os_critical_enter();

    /* NULL means the calling task here too, so this no longer stands out as the one task call that
     * refuses the shorthand every other one accepts. */
    if (task == NULL)
    {
        tcb = os_task_self_tcb();
    }
    else
    {
        tcb = (task->id != 0U) ? os_task_find_by_id(task->id) : NULL;
    }

    if (tcb == NULL)
    {
        os_critical_exit();
        return OS_ERR_INVALID_ARG;
    }

    tcb->core_affinity = core_affinity;

    running = os_task_running_core(tcb);
    if ((running < OS_CONFIG_CORE_COUNT) && os_kernel_is_running() &&
        (core_affinity != OS_TASK_CORE_ANY) && ((core_affinity & (1UL << running)) == 0U))
    {
        if (running == os_arch_core_id_get())
        {
            os_task_switch_request();
        }
        else
        {
            os_arch_core_ipi_request_cb(running);
        }
    }

    os_critical_exit();
    return OS_ERR_NONE;
}
#endif /* OS_CONFIG_CORE_COUNT > 1U */

/******************************************************************************************************/
/**
 * @brief Terminate the calling task; used when a task entry function returns.
 *
 * @return None.
 */
void os_task_exit(void)
{
    (void)os_task_delete(NULL);

    /* Deletion switches away; if it could not (pre-scheduler misuse), park. */
    while (1)
    {
        OS_ARCH_IDLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Create the mandatory idle task.
 *
 * @return os_err_t  Status code.
 */
os_err_t os_task_idle_create(void)
{
    uint32_t core;

    if (os_task_idle_tcb[0].state != OS_TASK_STATE_INACTIVE)
    {
        return OS_ERR_NONE;
    }

    /* One idle task per scheduling core; each is pinned to its core and is
     * never queued in a ready list (it is the empty-bitmap fallback). */
    for (core = 0U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        os_task_tcb_t *tcb = &os_task_idle_tcb[core];
        uint32_t      *stack_ptr;

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
        os_task_stack_fill(os_task_idle_stack[core], sizeof(os_task_idle_stack[core]));
#endif
#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
        /* After any fill, which would otherwise overwrite it. */
        os_task_stack_guard_set(os_task_idle_stack[core]);
#endif

        stack_ptr = os_arch_task_stack_initialize(os_task_idle_stack[core], sizeof(os_task_idle_stack[core]),
                                                  os_task_idle_entry, NULL);
        if (stack_ptr == NULL)
        {
            return OS_ERR_ERROR;
        }

        tcb->name          = "tsk_idle";
        tcb->stack_base    = os_task_idle_stack[core];
        tcb->stack_ptr     = stack_ptr;
        tcb->stack_bytes   = sizeof(os_task_idle_stack[core]);
        tcb->priority      = OS_TASK_PRIO_IDLE;
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
        tcb->base_priority = OS_TASK_PRIO_IDLE;
#endif
        tcb->id            = 0U;
        tcb->delay_ticks   = 0U;
        tcb->core_affinity = (1UL << core);
        tcb->state         = OS_TASK_STATE_READY;
    }

    return OS_ERR_NONE;
}

/******************************************************************************************************/
/**
 * @brief Check whether idle task is already created.
 *
 * @return bool  True when idle task exists.
 */
bool os_task_idle_is_created(void)
{
    return (os_task_idle_tcb[0].state != OS_TASK_STATE_INACTIVE);
}

/******************************************************************************************************/
/**
 * @brief Initialize task management subsystem.
 *
 * @return None.
 */
void os_task_system_init(void)
{
    uint32_t index;
    uint32_t priority;
    uint32_t core;

    for (index = 0U; index < OS_TASK_TABLE_SIZE; index++)
    {
        os_task_tcb_clear(&os_task_table[index]);
    }

    for (priority = 0U; priority <= OS_TASK_PRIO_MAX; priority++)
    {
        os_list_init(&os_task_ready_list[priority]);
    }

    os_task_ready_bitmap = 0U;
    os_list_init(&os_task_delay_list);

    os_task_next_id = 1U;

    for (core = 0U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        os_task_current[core]        = NULL;
        os_kernel_lock_count[core] = 0U;
        os_kernel_switch_pending[core] = false;
#if (OS_CONFIG_TIME_SLICE_TICKS > 0U)
        os_task_slice_left[core]     = OS_CONFIG_TIME_SLICE_TICKS;
#endif
        os_task_tcb_clear(&os_task_idle_tcb[core]);
    }
}

/******************************************************************************************************/
/**
 * @brief Save the current running task stack pointer (called from PendSV).
 *
 * @param[in] stack_ptr  Updated process stack pointer.
 * @return None.
 */
void os_task_stack_save_current(uint32_t *stack_ptr)
{
    uint32_t      mask_state;
    uint32_t      core;
    os_task_tcb_t *current_task;

    /* PendSV runs with interrupts enabled: raise the kernel mask so an ISR
     * waking or pausing tasks cannot touch the ready lists mid-update, and
     * take the cross-core spinlock so another core's os_critical_enter
     * callers (which push/pop these same shared ready/delay lists and
     * bitmap) are excluded too - the local mask alone only stops this
     * core's own interrupts, not the other core's task-context callers. */
    mask_state = os_arch_kernel_mask_save();
    os_critical_multicore_lock();

    core         = os_arch_core_id_get();
    current_task = os_task_current[core];

    if (current_task != NULL)
    {
        current_task->stack_ptr = stack_ptr;

#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
        /* Checked here because this is the one place the kernel sees a task's stack pointer at
         * rest, on every switch away from it. A task that has overrun its stack is caught at the
         * moment it stops running, before the scheduler hands its (corrupt) frame to anyone. */
        os_task_stack_guard_check(current_task, stack_ptr);
#endif
    }

    /* Scheduler locked: this PendSV must not switch. Requests raised under the lock never pend
     * one, so getting here means it was already pending when the lock was taken (or an IPI
     * arrived). Leave the task RUNNING and owning this core - select_next hands the frame just
     * saved straight back - and remember the switch for the outermost os_kernel_unlock.
     * Only for a still-RUNNING task, the same condition select_next re-checks. */
    if ((current_task != NULL) && (current_task->state == OS_TASK_STATE_RUNNING) &&
        (os_kernel_lock_count[core] != 0U))
    {
        os_kernel_switch_pending[core] = true;

        os_critical_multicore_unlock();
        os_arch_kernel_mask_restore(mask_state);
        return;
    }

    if (current_task != NULL)
    {
        /* The context is now safely saved: from this point any core may
         * dispatch this task (see the running_core check in
         * os_task_stack_select_next). Before this write, a wake arriving on
         * another core while this task was still physically executing here
         * (state already flipped to BLOCKED/READY by the waker, but this
         * core had not yet run this function) must not let that other core
         * pick it up - its stack_ptr would still be stale. */
        current_task->running_core = OS_CONFIG_CORE_COUNT;

        /* A preempted task goes back to the TAIL of its priority's ready
         * list - that is the round-robin rotation. A task that just blocked
         * or suspended itself keeps its state (it is already in the right
         * list, or in none), and the idle tasks are never queued. */
        if (current_task->state == OS_TASK_STATE_RUNNING)
        {
            if (current_task == &os_task_idle_tcb[core])
            {
                current_task->state = OS_TASK_STATE_READY;
            }
            else
            {
                os_task_make_ready(current_task);
            }
        }
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);
}

/******************************************************************************************************/
/**
 * @brief Select the next task to run and return its stack pointer (called from PendSV).
 *
 * @return uint32_t*  Stack pointer for the selected task; never NULL (idle fallback).
 */
uint32_t* os_task_stack_select_next(void)
{
    uint32_t      mask_state = os_arch_kernel_mask_save();
    uint32_t      core;
    uint32_t      bitmap;
    os_task_tcb_t *next;

    /* See os_task_stack_save_current: the cross-core spinlock excludes the
     * other cores' os_critical_enter callers on these same shared lists. */
    os_critical_multicore_lock();

    core = os_arch_core_id_get();
    next = &os_task_idle_tcb[core];

    /* Scheduler locked on this core: hand the running task straight back, so
     * the PendSV that reached os_task_stack_save_current unwinds into the
     * same context it came from. Its quantum is deliberately NOT reloaded -
     * the task never actually left the CPU, and a locked region must not be
     * able to extend its own time slice. A task that is no longer RUNNING
     * (nothing the kernel does under a lock leaves one that way, but a
     * corrupted state must not be dispatched) falls through to a normal pick. */
    {
        os_task_tcb_t *locked_current = os_task_current[core];

        if ((os_kernel_lock_count[core] != 0U) && (locked_current != NULL) &&
            (locked_current->state == OS_TASK_STATE_RUNNING))
        {
            os_critical_multicore_unlock();
            os_arch_kernel_mask_restore(mask_state);

            return locked_current->stack_ptr;
        }
    }

    /* O(1) pick on single-core: the bitmap names the highest non-empty
     * priority and the FIFO head is the next task (round-robin). On
     * multi-core builds each list is additionally walked past tasks whose
     * affinity excludes this core, and past a task still mid-switch-out on
     * another core (running_core != CORE_COUNT: it was woken here before
     * that core's os_task_stack_save_current saved its context, so its
     * stack_ptr is not yet safe to restore from - see the running_core
     * write there); either skip leaves the bitmap bit set. */
    bitmap = os_task_ready_bitmap;
    while (bitmap != 0U)
    {
        uint32_t       priority = os_arch_highest_bit_get(bitmap);
        os_list_t      *list    = &os_task_ready_list[priority];
        os_list_node_t *node    = list->head;

        while (node != NULL)
        {
            os_task_tcb_t *tcb = OS_TASK_TCB_FROM_NODE(node);

#if (OS_CONFIG_CORE_COUNT > 1U)
            if ((tcb->core_affinity != OS_TASK_CORE_ANY) &&
                ((tcb->core_affinity & (1UL << core)) == 0U))
            {
                node = node->next;
                continue;
            }

            if (tcb->running_core != OS_CONFIG_CORE_COUNT)
            {
                node = node->next;
                continue;
            }
#endif

            os_list_remove(list, node);
            if (os_list_is_empty(list))
            {
                os_task_ready_bitmap &= ~(1UL << priority);
            }

            next = tcb;
            break;
        }

        if (next != &os_task_idle_tcb[core])
        {
            break;
        }

        bitmap &= ~(1UL << priority);
    }

    next->state           = OS_TASK_STATE_RUNNING;
    next->running_core     = core;
    os_task_current[core] = next;

#if (OS_CONFIG_TIME_SLICE_TICKS > 0U)
    /* A dispatched task starts a fresh quantum. That includes being
     * re-dispatched immediately (a yield with no peer ready), which is what
     * makes an explicit yield a genuine restart rather than a way to inherit
     * the tail of the previous slice. */
    os_task_slice_left[core] = OS_CONFIG_TIME_SLICE_TICKS;
#endif

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);

    return next->stack_ptr;
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Shared task creation core used by the public and kernel-internal paths.
 *
 * @param[out] task         Output task handle.
 * @param[in]  config       Task creation configuration.
 * @param[in]  system_task  True for a kernel service task (os_task_create_system): exempt from the
 *                          user priority range, and protected from os_task_pause/os_task_delete.
 * @return os_err_t   Status code.
 */
static os_err_t os_task_create_any(os_task_t *task, const os_task_config_t *config, bool system_task)
{
    uint32_t  index;
    uintptr_t stack_addr;
    uint32_t  *stack_ptr;

    /* The stack comes from the handle, not the config: a handle declared any way other than with
     * OS_TASK_DEFINE has no storage attached and is rejected here rather than being run on
     * whatever the object happened to contain. */
    if ((task == NULL) ||
        (task->storage == NULL) ||
        (config == NULL) ||
        (config->entry == (os_task_entry_t)0) ||
        (config->priority > OS_TASK_PRIO_MAX) ||
        (task->storage->stack_memory == NULL) ||
        (task->storage->stack_bytes < OS_CONFIG_MIN_STACK_SIZE))
    {
        return OS_ERR_INVALID_ARG;
    }

    /* The affinity mask may only name existing cores (0 = any core). */
    if ((config->core_affinity >> OS_CONFIG_CORE_COUNT) != 0U)
    {
        return OS_ERR_INVALID_ARG;
    }

    stack_addr = (uintptr_t)task->storage->stack_memory;
    if (((stack_addr % OS_ARCH_STACK_ALIGNMENT_BYTES) != 0U) ||
        ((task->storage->stack_bytes % OS_ARCH_STACK_ALIGNMENT_BYTES) != 0U))
    {
        return OS_ERR_INVALID_ARG;
    }

    /* Refuse a handle that is already live, BEFORE anything writes to its stack.
     *
     * Placement is the whole point of this check. The pre-fill, the guard word and the initial
     * exception frame below all land in task->storage->stack_memory, and OS_TASK_DEFINE gives one
     * array per handle - so creating twice through the same handle would paint the stack of the
     * task still RUNNING on it with the fill byte and lay a fresh frame over its live context. A
     * guard sharing the slot-scan critical section further down would reject the call only after
     * that damage was done.
     *
     * Every other object in the kernel refuses re-initialisation the same way (os_mutex_init,
     * os_semaphore_init, os_event_init, os_queue_bind_buffer, all with OS_ERR_BUSY); a task is
     * simply the one that cannot afford to find out late. Re-checked inside the slot-scan critical
     * section, which is what makes the test and the claim atomic against a peer core creating
     * through this same handle. */
    os_critical_enter();
    if ((task->id != 0U) && (os_task_find_by_id(task->id) != NULL))
    {
        os_critical_exit();
        return OS_ERR_BUSY;
    }
    os_critical_exit();

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    /* Pre-fill so os_task_stack_watermark_get can measure peak usage. */
    os_task_stack_fill((uint8_t *)task->storage->stack_memory, task->storage->stack_bytes);
#endif
#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
    /* After any fill, which would otherwise overwrite it. */
    os_task_stack_guard_set((uint8_t *)task->storage->stack_memory);
#endif

    /* Build the initial frame before taking the critical section: it only
     * touches the caller's stack memory. */
    stack_ptr = os_arch_task_stack_initialize((uint8_t *)task->storage->stack_memory,
                                              task->storage->stack_bytes,
                                              config->entry, config->context);
    if (stack_ptr == NULL)
    {
        return OS_ERR_ERROR;
    }

    os_critical_enter();

    /* The atomic half of the guard above: on SMP a peer core could have created through this same
     * handle since that check ran. The stack writes between the two are then wasted work on a
     * handle the caller had already made live, which is the caller's mistake to have made - but
     * the TCB slot and the id are still claimed exactly once. */
    if ((task->id != 0U) && (os_task_find_by_id(task->id) != NULL))
    {
        os_critical_exit();
        return OS_ERR_BUSY;
    }

    for (index = 0U; index < OS_TASK_TABLE_SIZE; index++)
    {
        if (os_task_table[index].state == OS_TASK_STATE_INACTIVE)
        {
            os_task_tcb_t *tcb = &os_task_table[index];

            /* Skip ids still owned by live tasks: the counter wraps at 2^32,
             * and a reused id would hand the new task another task's
             * identity (e.g. the right to unlock a dead owner's mutex). */
            while ((os_task_next_id == 0U) || (os_task_find_by_id(os_task_next_id) != NULL))
            {
                os_task_next_id++;
            }

            tcb->name             = task->storage->name;
            tcb->stack_base       = (uint8_t *)task->storage->stack_memory;
            tcb->stack_ptr        = stack_ptr;
            tcb->stack_bytes      = task->storage->stack_bytes;
            tcb->priority         = config->priority;
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
            tcb->base_priority    = config->priority;
            tcb->pi_owner_id      = 0U;
#endif
            tcb->id               = os_task_next_id;
            tcb->delay_ticks      = 0U;
            tcb->core_affinity    = config->core_affinity;
            tcb->system_task      = system_task;
            tcb->state            = OS_TASK_STATE_SUSPENDED;

            task->id = os_task_next_id;
            os_task_next_id++;

            if (os_task_next_id == 0U)
            {
                os_task_next_id = 1U;
            }

            os_critical_exit();
            return OS_ERR_NONE;
        }
    }

    os_critical_exit();
    return OS_ERR_FULL;
}

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Fill a stack with the watermark pattern before its first use.
 *
 * @param[in,out] stack_base   Base address of the stack memory.
 * @param[in]     stack_bytes  Size of the stack memory in bytes.
 * @return None.
 */
static void os_task_stack_fill(uint8_t *stack_base, size_t stack_bytes)
{
    size_t index;

    for (index = 0U; index < stack_bytes; index++)
    {
        stack_base[index] = OS_TASK_STACK_FILL_BYTE;
    }
}
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */

#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Write the guard word at the bottom of a task stack.
 *
 * @param[in] stack_base  Lowest address of the task's stack (8-byte aligned by OS_TASK_DEFINE).
 * @return None.
 */
static void os_task_stack_guard_set(uint8_t *stack_base)
{
    *(uint32_t *)(void *)stack_base = OS_TASK_STACK_CANARY;
}

/******************************************************************************************************/
/**
 * @brief Check a task's stack for overflow at switch-out; never returns if one is found.
 *
 * Two tests, catching different failures: a stack pointer below stack_base means the task is
 * executing outside its own stack right now, and a clobbered guard word means it went too deep and
 * came back, which nothing else would notice. On a hit there is no continuing - memory outside the
 * stack is already modified - so os_stack_overflow_cb reports it and the core parks, as a failed
 * OS_ASSERT does. Runs inside PendSV with the kernel mask raised and the cross-core spinlock held,
 * so the callback must not call kernel APIs.
 *
 * @param[in] tcb        Task being switched out.
 * @param[in] stack_ptr  Stack pointer just saved for it.
 * @return None.
 */
static void os_task_stack_guard_check(const os_task_tcb_t *tcb, const uint32_t *stack_ptr)
{
    if (tcb->stack_base == NULL)
    {
        return;
    }

    if (((const uint8_t *)(const void *)stack_ptr >= tcb->stack_base) &&
        (*(const uint32_t *)(const void *)tcb->stack_base == OS_TASK_STACK_CANARY))
    {
        return;
    }

    os_stack_overflow_cb(tcb->name);
    os_arch_config_fault_trap();

    /* os_arch_config_fault_trap never returns; the loop only convinces the
     * compiler of that when it is inlined as a plain call. */
    while (1)
    {
    }
}

#endif /* OS_CONFIG_STACK_CHECK_ENABLE */

/******************************************************************************************************/
/**
 * @brief Idle task body: wait for interrupts forever.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_task_idle_entry(void *context)
{
    (void)context;

    while (1)
    {
        OS_ARCH_IDLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Reset a TCB to the inactive state.
 *
 * @param[in,out] tcb  Task control block to clear.
 * @return None.
 */
static void os_task_tcb_clear(os_task_tcb_t *tcb)
{
    tcb->name          = NULL;
    tcb->stack_base    = NULL;
    tcb->stack_ptr     = NULL;
    tcb->stack_bytes   = 0U;
    tcb->priority      = 0U;
    tcb->id            = 0U;
    tcb->delay_ticks   = 0U;
    tcb->core_affinity = OS_TASK_CORE_ANY;
    tcb->system_task   = false;
    tcb->state         = OS_TASK_STATE_INACTIVE;
    tcb->running_core  = OS_CONFIG_CORE_COUNT;
    tcb->wait_list     = NULL;
    tcb->wait_signaled = false;
    tcb->wait_data[0]  = 0U;
    tcb->wait_data[1]  = 0U;
    tcb->wait_result   = 0U;
    tcb->woken_from    = NULL;
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    tcb->base_priority = 0U;
    tcb->pi_owner_id   = 0U;

    /* A task must not die still owning a mutex - by being deleted, or by simply returning from its
     * entry function, which os_arch_task_exit_trap turns into exactly the same deletion.
     *
     * Nothing downstream can repair it. Only the owner may unlock a mutex, and that owner no longer
     * exists, so the object stays locked forever behind an owner_id that resolves to nothing. Worse,
     * it is invisible to os_task_mutex_deadlock_check: that walk stops at the first unresolvable
     * owner, because normally an owner that cannot be resolved means there is NO cycle. So every
     * task queued on that mutex blocks permanently while the one tool built to explain this class of
     * hang stays silent. It is the only way this kernel stops without saying why.
     *
     * An assertion rather than a status, for the usual reason: it is a static mistake in the
     * application - code that took a lock and left without giving it back - not a runtime condition
     * a caller could handle. On the return path there is no caller left to hand a status to anyway;
     * the task's own code has already finished. Compiles away completely with
     * OS_CONFIG_ASSERT_ENABLE at 0, the list walk included.
     *
     * Release builds still run the detach below. It cannot rescue the mutex, but it keeps the next
     * task to occupy this slot from inheriting dangling links into a list that is not its own. */
    OS_ASSERT(os_list_is_empty(&tcb->owned_mutexes));

    /* Detach every mutex this task still owned before the slot is recycled:
     * otherwise the next task placed here would inherit dangling links into
     * an owned_mutexes list that no longer represents it. This only prevents
     * corruption - the mutex itself stays locked forever, same as today. */
    while (!os_list_is_empty(&tcb->owned_mutexes))
    {
        (void)os_list_pop_front(&tcb->owned_mutexes);
    }
#endif
#if (OS_MUTEX_DEADLOCK_CHECK == 1)
    tcb->blocked_on_mutex = NULL;
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    tcb->notify.value   = 0U;
    tcb->notify.pending = false;
    tcb->notify.waiting = false;
#endif

    tcb->state_node.next = NULL;
    tcb->state_node.prev = NULL;
    tcb->wait_node.next  = NULL;
    tcb->wait_node.prev  = NULL;
}

/******************************************************************************************************/
/**
 * @brief Find a live task by its ID.
 *
 * A slot only counts as live while its state is not INACTIVE, so a recycled or never-created
 * slot can never be matched by a stale id.
 *
 * @param[in] id  Task ID.
 * @return os_task_tcb_t*  Pointer to the task control block, or NULL if not found.
 */
/******************************************************************************************************/
/**
 * @brief The TCB that a NULL public handle stands for: the calling task.
 *
 * NULL means "this task" across the task API, which needs a calling task to mean. An ISR has none:
 * os_task_current there is merely whichever task the interrupt happened to preempt, so acting on it
 * would hit a plausible but wrong target - silently deleting, pausing or re-prioritising a task that
 * simply had the bad luck to be running. NULL therefore resolves to nothing in interrupt context,
 * and every caller already turns a NULL TCB into OS_ERR_INVALID_ARG.
 *
 * Also NULL before the scheduler dispatches a first task, which lands on the same status.
 *
 * @return os_task_tcb_t*  The calling task's TCB, or NULL when there is no calling task.
 */
static os_task_tcb_t* os_task_self_tcb(void)
{
    return os_arch_in_isr() ? NULL : os_task_current[os_arch_core_id_get()];
}

/******************************************************************************************************/
static os_task_tcb_t* os_task_find_by_id(uint32_t id)
{
    uint32_t index;

    for (index = 0U; index < OS_TASK_TABLE_SIZE; index++)
    {
        if ((os_task_table[index].state != OS_TASK_STATE_INACTIVE) && (os_task_table[index].id == id))
        {
            return &os_task_table[index];
        }
    }

    return NULL;
}

/******************************************************************************************************/
/**
 * @brief Queue a task at the tail of its priority's ready list and flag the priority.
 *
 * Caller must hold a critical section (or have interrupts masked) and must
 * never pass the idle task or a task already sitting in a list.
 *
 * @param[in,out] tcb  Task to make ready.
 * @return None.
 */
static void os_task_make_ready(os_task_tcb_t *tcb)
{
    tcb->state = OS_TASK_STATE_READY;
    os_list_push_back(&os_task_ready_list[tcb->priority], &tcb->state_node);
    os_task_ready_bitmap |= (1UL << tcb->priority);
}

/******************************************************************************************************/
/**
 * @brief Remove a task from whatever scheduler list its state implies (no-op when in none).
 *
 * READY tasks leave their priority's ready list (clearing the bitmap bit when
 * it empties); finite-delay BLOCKED tasks leave the delay list. Running,
 * suspended and forever-blocked tasks are in no list. Caller must hold a
 * critical section (or have interrupts masked).
 *
 * @param[in,out] tcb  Task to unlink.
 * @return None.
 */
static void os_task_unlink(os_task_tcb_t *tcb)
{
    if (tcb->state == OS_TASK_STATE_READY)
    {
        os_list_t *list = &os_task_ready_list[tcb->priority];

        os_list_remove(list, &tcb->state_node);

        if (os_list_is_empty(list))
        {
            os_task_ready_bitmap &= ~(1UL << tcb->priority);
        }
    }
    else if ((tcb->state == OS_TASK_STATE_BLOCKED) && (tcb->delay_ticks != OS_WAIT_FOREVER))
    {
        os_list_remove(&os_task_delay_list, &tcb->state_node);
    }
    else
    {
        /* Running, suspended, inactive or forever-blocked: in no state list. */
    }

    /* A blocked task may additionally sit in an object's waiter list. */
    if (tcb->wait_list != NULL)
    {
        os_list_remove(tcb->wait_list, &tcb->wait_node);
        tcb->wait_list = NULL;
    }
}

/******************************************************************************************************/
/**
 * @brief Queue a task's wait node in an object's waiter list, priority ordered.
 *
 * Insert before the first waiter of strictly lower priority: wakeups go to the
 * highest-priority waiter, FIFO within one priority level. Every path that puts
 * a wait node in a list goes through here, so a task whose priority changes
 * while it is queued can be re-sorted by removing it and inserting it again
 * (see os_task_effective_priority_set). Caller holds a critical section.
 *
 * @param[in,out] waiters  The object's waiter list.
 * @param[in,out] tcb      Task to queue; its priority decides the position.
 * @return None.
 */
static void os_task_wait_node_insert(os_list_t *waiters, os_task_tcb_t *tcb)
{
    os_list_node_t *position = waiters->head;

    while (position != NULL)
    {
        if (OS_TASK_TCB_FROM_WAIT_NODE(position)->priority < tcb->priority)
        {
            break;
        }

        position = position->next;
    }

    os_list_insert_before(waiters, position, &tcb->wait_node);
}

/******************************************************************************************************/
/**
 * @brief Pend a context switch on the calling core, or remember it when the scheduler is locked.
 *
 * Every LOCAL switch request in the kernel goes through here rather than pending PendSV directly,
 * so a scheduler-locked core pays no PendSV round trip per request (and none per tick). The cost of
 * swallowing one is a flag: os_kernel_unlock re-issues it. Cross-core IPI requests are NOT routed
 * through this - another core's scheduling is not this core's lock to hold.
 *
 * @return None.
 */
static void os_task_switch_request(void)
{
    uint32_t core = os_arch_core_id_get();

    if (os_kernel_lock_count[core] != 0U)
    {
        os_kernel_switch_pending[core] = true;
        return;
    }

    OS_ARCH_CONTEXT_SWITCH_REQUEST();
}

/******************************************************************************************************/
/**
 * @brief Request a reschedule wherever the given task may run: locally when its affinity
 *        allows this core, otherwise via IPI to the first core in its mask.
 *
 * Caller checks os_kernel_is_running(). On single-core builds this is a
 * plain local PendSV request.
 *
 * @param[in] tcb  Task that just became ready.
 * @return None.
 */
static void os_task_preempt_request(const os_task_tcb_t *tcb)
{
    uint32_t            core = os_arch_core_id_get();
    const os_task_tcb_t *current;

#if (OS_CONFIG_CORE_COUNT > 1U)
    if ((tcb->core_affinity != OS_TASK_CORE_ANY) &&
        ((tcb->core_affinity & (1UL << core)) == 0U))
    {
        /* The task cannot run here: nudge the first core it may run on
         * (weak default IPI does nothing - that core then picks the task
         * up at its own next tick). */
        os_arch_core_ipi_request_cb(os_arch_lowest_bit_get(tcb->core_affinity));
        return;
    }
#endif

    /* Only a strictly higher-priority task warrants an immediate switch:
     * equal priorities round-robin at the tick, so skipping the PendSV here
     * both saves the full context-switch cost and stops every wake from
     * acting as an implicit yield of the waker's remaining timeslice. */
    current = os_task_current[core];

    if ((current == NULL) || (tcb->priority > current->priority))
    {
        os_task_switch_request();
        return;
    }

#if (OS_CONFIG_CORE_COUNT > 1U)
    /* This core keeps its own (higher-or-equal priority) task, but another
     * core allowed by the woken task's affinity may be idle or running
     * something lower priority: without this scan the task would wait for
     * that core's own next tick even though it could run immediately.
     * Reading os_task_current[] here is safe because every caller of this
     * function already holds os_critical_enter, which on multi-core builds
     * holds the same spinlock os_task_stack_select_next takes before
     * writing os_task_current[]. */
    {
        uint32_t other;

        for (other = 0U; other < OS_CONFIG_CORE_COUNT; other++)
        {
            const os_task_tcb_t *other_current;

            if (other == core)
            {
                continue;
            }

            if ((tcb->core_affinity != OS_TASK_CORE_ANY) && ((tcb->core_affinity & (1UL << other)) == 0U))
            {
                continue;
            }

            other_current = os_task_current[other];

            if ((other_current == NULL) || (tcb->priority > other_current->priority))
            {
                os_arch_core_ipi_request_cb(other);
                return;
            }
        }
    }
#endif
}

/******************************************************************************************************/
/**
 * @brief Shared body of os_task_wake / os_task_wake_tcb: caller already holds whatever
 *        locking that entry point's contract requires.
 *
 * @param[in,out] tcb  Task to wake, or NULL (no-op).
 * @return None.
 */
static void os_task_wake_locked(os_task_tcb_t *tcb)
{
    if ((tcb != NULL) && (tcb->state == OS_TASK_STATE_BLOCKED))
    {
        /* Unlink inspects delay_ticks to pick the list, so it runs first.
         * A forced wake reads as a (spurious) signal: a primitive waiter
         * then re-checks its condition instead of reporting a timeout.
         * delay_ticks is left untouched so the waiter keeps its remaining
         * timeout budget for the retry (recomputed against the wall clock). */
        os_task_unlink(tcb);
        tcb->wait_signaled = true;
        os_task_make_ready(tcb);

        if (os_kernel_is_running())
        {
            os_task_preempt_request(tcb);
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Pass on an unconsumed wake before a READY task is suspended or deleted.
 *
 * woken_from is live only between a waker handing this task a notification and the task's own
 * primitive finishing with it (os_task_wait_begin clears it on a re-block, os_task_wait_end on
 * return). The task may be READY rather than RUNNING inside that window, so the pair
 * (wait_signaled, woken_from) is what marks an unconsumed notification, not the state alone.
 * Without this, pausing or deleting there would strand the resource with nobody left to signal
 * the next waiter; the clears are what stop woken_from outliving the object it points into.
 *
 * @param[in,out] tcb  Task about to be suspended or deleted.
 * @return None.
 */
static void os_task_wake_compensate(os_task_tcb_t *tcb)
{
    if ((tcb->state == OS_TASK_STATE_READY) && tcb->wait_signaled && (tcb->woken_from != NULL))
    {
        os_list_t *list = tcb->woken_from;

        tcb->woken_from = NULL;
        (void)os_task_waiters_wake_one(list);
    }
}

/******************************************************************************************************/
/**
 * @brief Find the core a task is currently executing on.
 *
 * @param[in] tcb  Task to look up.
 * @return uint32_t  Core index, or OS_CONFIG_CORE_COUNT when the task is not running anywhere.
 */
static uint32_t os_task_running_core(const os_task_tcb_t *tcb)
{
    uint32_t core;

    for (core = 0U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        if (os_task_current[core] == tcb)
        {
            return core;
        }
    }

    return OS_CONFIG_CORE_COUNT;
}

/******************************************************************************************************/
/**
 * @brief Change a task's effective (scheduled) priority, moving it between ready-list buckets,
 *        re-sorting it in any waiter list it is queued on, and requesting a reschedule wherever
 *        needed - the only correct way to mutate tcb->priority once a task may already be
 *        READY/RUNNING or blocked on an object. Caller holds a critical section (mutex
 *        lock/unlock's own).
 *
 * @param[in,out] tcb           Task to reprioritize.
 * @param[in]     new_priority  New effective priority.
 * @return None.
 */
static void os_task_effective_priority_set(os_task_tcb_t *tcb, uint32_t new_priority)
{
    if (new_priority == tcb->priority)
    {
        return;
    }

    if (tcb->state == OS_TASK_STATE_READY)
    {
        bool increasing = (new_priority > tcb->priority);

        os_task_unlink(tcb);   /* leaves the OLD priority's bucket */
        tcb->priority = new_priority;
        os_task_make_ready(tcb); /* rejoins at the NEW priority's bucket */

        if (increasing && os_kernel_is_running())
        {
            os_task_preempt_request(tcb);
        }
        return;
    }

    if ((tcb->state == OS_TASK_STATE_RUNNING) && (new_priority < tcb->priority) && os_kernel_is_running())
    {
        /* Lowering the priority of a task that is currently executing may
         * unmask a ready task that now outranks it - nothing else triggers
         * this check (unlike the READY/boost case, no os_task_preempt_request
         * call is naturally in the caller's path). */
        uint32_t core = os_task_running_core(tcb);

        tcb->priority = new_priority;

        if ((core < OS_CONFIG_CORE_COUNT) && (new_priority < OS_TASK_PRIO_MAX))
        {
            uint32_t above_mask = ~((1UL << (new_priority + 1U)) - 1U);

            if ((os_task_ready_bitmap & above_mask) != 0U)
            {
#if (OS_CONFIG_CORE_COUNT > 1U)
                if (core == os_arch_core_id_get())
                {
                    os_task_switch_request();
                }
                else
                {
                    os_arch_core_ipi_request_cb(core);
                }
#else
                os_task_switch_request();
#endif
            }
        }
        return;
    }

    /* BLOCKED/SUSPENDED, or RUNNING with an unchanged-relevance change: no
     * state list to move and no reschedule to request yet - that part takes
     * effect the next time this task is made ready. */
    tcb->priority = new_priority;

    /* A waiter list is priority-sorted at insert time, so a task boosted while
     * it is queued on some other object still sits where its OLD priority put
     * it: it would be woken after waiters it now outranks, and (since the head
     * is read as the highest-priority waiter) it would also feed a wrong
     * recomputed priority back into os_task_mutex_owner_unlink_and_reprioritize.
     * Re-sorting is a remove and a re-insert at the new position. */
    if (tcb->wait_list != NULL)
    {
        os_list_remove(tcb->wait_list, &tcb->wait_node);
        os_task_wait_node_insert(tcb->wait_list, tcb);
    }
}
