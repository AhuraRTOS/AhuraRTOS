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

/* A task id carries the table slot that owns it, so resolving one is an index rather than a search.
 *
 *     bits 31..24   slot + 1   (never 0, so a whole id can never be 0 either)
 *     bits 23..0    generation, bumped every time this slot is handed out
 *
 * os_task_find_by_id used to walk the whole table. That is not a cold path: it is on os_task_wake,
 * on every blocking mutex lock through os_task_mutex_priority_inherit, on every timeout, pause or
 * delete of a waiter through os_task_mutex_waiter_depart_tcb - and once priority inheritance
 * started walking CHAINS, once per link as well. The id already had to be unique; making it say
 * where it lives costs nothing and turns all of that into one bounds check and one compare.
 *
 * The generation is what keeps a stale handle honest. A slot reused by a new task gets a new
 * generation, so an id from the previous occupant no longer matches and resolves to NULL - which is
 * exactly what it did before, when the search simply failed to find it. 16.7 million creations per
 * slot before a generation repeats, and a repeat needs the ORIGINAL handle to still be held that
 * many creations later.
 *
 * The slot count is bounded by the 8 bits reserved for it, which OS_TASK_TABLE_SIZE is checked
 * against below. */
#define OS_TASK_ID_SLOT_SHIFT        24U
#define OS_TASK_ID_GENERATION_MASK   0x00FFFFFFUL
#define OS_TASK_ID_SLOT(id)          (((id) >> OS_TASK_ID_SLOT_SHIFT) - 1U)
#define OS_TASK_ID_MAKE(slot, gen)   ((((uint32_t)(slot) + 1UL) << OS_TASK_ID_SLOT_SHIFT) |        \
                                      ((gen) & OS_TASK_ID_GENERATION_MASK))

/* Guard word written at the LOWEST address of every task stack - the last place a growing stack
 * reaches before it leaves its own memory. Deliberately the fill byte repeated, so a build with
 * OS_CONFIG_STACK_WATERMARK_ENABLE already writes it as part of its whole-stack fill and the two
 * features cost nothing together. */
#define OS_TASK_STACK_CANARY         0xA5A5A5A5UL

#include "os_task_internal.h"

/* OS_TASK_DEADLOCK_MAX_DEPTH (os_internal.h) bounds how far a wait chain is followed: a cycle
 * that already formed among other tasks would otherwise be walked forever. Eight is far past any
 * sane nesting depth - a design needing more than a couple of held mutexes at once has a
 * lock-ordering problem this check cannot fix anyway. */

/* A floor under OS_CONFIG_MIN_STACK_SIZE, because every other check measures against it and so
 * cannot catch it being wrong itself. Too low and os_arch_task_stack_initialize writes its initial
 * frame, up to 72 bytes, past the bottom of every task stack at creation time, silently. 128 is a
 * sanity floor, not a recommendation; the template ships 256. */
OS_STATIC_ASSERT((OS_CONFIG_MIN_STACK_SIZE >= 128U) && ((OS_CONFIG_MIN_STACK_SIZE % 8U) == 0U),
                 "OS_CONFIG_MIN_STACK_SIZE must be at least 128 and a multiple of 8");

/*
 * ***********************************************************************************************************
 * Types
 * ***********************************************************************************************************
*/


/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/* Every scheduling core owns one idle task and one current-task slot; the task table and the
 * ready/delay lists are shared, protected by the critical sections and the kernel spinlock. */
/* Table slots the kernel reserves for its own service tasks, on top of OS_CONFIG_MAX_USER_TASKS.
 *
 * This is what lets OS_CONFIG_MAX_USER_TASKS mean how many of ITS tasks may exist, rather than a
 * budget shared with whichever kernel services happen to be enabled.
 *
 * Exactly one of tsk_main and tsk_test always exists, and each optional service adds one. The idle
 * tasks are NOT counted: they live in os_task_idle_tcb, outside this table, one per core. */
#define OS_TASK_SYSTEM_SLOTS  (1U + \
                               ((OS_CONFIG_TIMER_ENABLE == 1U) ? 1U : 0U) + \
                                                              ((OS_CONFIG_LOG_ENABLE   == 1U) ? 1U : 0U))

#define OS_TASK_TABLE_SIZE    (OS_CONFIG_MAX_USER_TASKS + OS_TASK_SYSTEM_SLOTS)

/* The slot has to fit in the byte a task id reserves for it, and slot + 1 must not overflow it. */
OS_STATIC_ASSERT(OS_TASK_TABLE_SIZE < 255U,
                 "OS_CONFIG_MAX_USER_TASKS leaves more task slots than a task id can name");

static uint8_t                 os_task_idle_stack[OS_CONFIG_CORE_COUNT][OS_CONFIG_MIN_STACK_SIZE] OS_STACK_ALIGNED;
static os_task_tcb_t           os_task_idle_tcb[OS_CONFIG_CORE_COUNT];
static os_task_tcb_t           os_task_table[OS_TASK_TABLE_SIZE];
/* Next generation to hand out for each slot. Per slot rather than global, because that is what
 * lets an id name its slot: a global counter would put an arbitrary number in the bits the slot
 * needs. Starts at 1 so a slot's first id is never all-zero in the generation field either. */
static uint32_t                os_task_next_generation[OS_TASK_TABLE_SIZE];

/* Written by PendSV and read from task/ISR context: the pointer itself is the
 * shared object (it changes on every context switch), not what it points to -
 * __IO placed after the '*' qualifies the pointer, not the pointed-to TCB. */
os_task_tcb_t* __IO            os_task_current[OS_CONFIG_CORE_COUNT];

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
static bool          os_task_create_args_ok(const os_task_t *task, const os_task_config_t *config);
#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
static void           os_task_stack_fill(uint8_t *stack_base, size_t stack_bytes);
#endif
#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
static void           os_task_stack_guard_set(uint8_t *stack_base);
static void           os_task_stack_guard_check(const os_task_tcb_t *tcb, const uint32_t *stack_ptr);
#endif
static void           os_task_idle_entry(void *context);
static void           os_task_tcb_clear(os_task_tcb_t *tcb);
static os_task_tcb_t* os_task_self_tcb(void);
static void           os_task_delay_insert(os_task_tcb_t *tcb, uint32_t ticks);
static void           os_task_delay_remove(os_task_tcb_t *tcb);
static void           os_task_make_ready(os_task_tcb_t *tcb);
static void           os_task_unlink(os_task_tcb_t *tcb);
static void           os_task_wait_node_insert(os_list_t *waiters, os_task_tcb_t *tcb);
static void           os_task_switch_request(void);
static void           os_task_preempt_request(const os_task_tcb_t *tcb);
static uint32_t       os_task_running_core(const os_task_tcb_t *tcb);
static void           os_task_wake_compensate(os_task_tcb_t *tcb);
static os_err_t       os_task_delete_resolve(os_task_t *task, uint32_t core,
                                             os_task_tcb_t **tcb_out, bool *is_self_out);
static void           os_task_wake_locked(os_task_tcb_t *tcb);

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
 * Tasks created here are marked as the kernel's own, so os_task_pause and os_task_delete refuse
 * them with OS_ERR_BUSY. tsk_main and tsk_test do NOT come through here: they are ordinary
 * application tasks.
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
    uint32_t       core;
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
        status = OS_ERR_ISR;
    }
    else
    {
    os_critical_enter();
    core = os_arch_core_id_get();

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
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Delete a task and release its TCB slot (NULL means current running task).
 *
 * @param[in,out] task  Task handle, or NULL for the calling task.
 * @return os_err_t    Status code.
 */
/******************************************************************************************************/
/**
 * @brief Resolve a delete request to the TCB it names, or say why it cannot be honoured.
 *
 * Split out of os_task_delete so both keep a single exit without a deep staircase. Called with the
 * kernel critical section already held.
 *
 * @param[in]  task         Handle to delete, or NULL for the calling task.
 * @param[in]  core         This core's index.
 * @param[out] tcb_out      The TCB to tear down, written only on success.
 * @param[out] is_self_out  Whether that TCB is the caller's own, written only on success.
 * @return os_err_t  OK when the caller may proceed; INVALID_ARG for a handle that names nothing;
 *                    BUSY for the idle task, a kernel service task, a task running on another
 *                    core, or the caller's own task while the scheduler is locked.
 */
static os_err_t os_task_delete_resolve(os_task_t *task, uint32_t core,
                                       os_task_tcb_t **tcb_out, bool *is_self_out)
{
    os_err_t       status = OS_ERR_INVALID_ARG;
    os_task_tcb_t *tcb    = NULL;

    if (task == NULL)
    {
        tcb = os_task_self_tcb();

        if (tcb == &os_task_idle_tcb[core])
        {
            tcb    = NULL;
            status = OS_ERR_BUSY;
        }
        else if (tcb != NULL)
        {
            /* Re-resolve through the table: confirms the running task really owns
             * a live table slot before its TCB is torn down. */
            tcb = os_task_find_by_id(tcb->id);
        }
        else
        {
            /* No calling task to speak of; status stays OS_ERR_INVALID_ARG. */
        }
    }
    else if (task->id != 0U)
    {
        tcb = os_task_find_by_id(task->id);
    }
    else
    {
        /* A handle already cleared by an earlier delete; status stays OS_ERR_INVALID_ARG. */
    }

    if (tcb != NULL)
    {
        bool is_self = (tcb == os_task_current[core]);

        /* See os_task_pause: a kernel service task is not the application's to tear down.
         * Deleting one would also release its TCB slot and leave the timer/log registries
         * pointing at a task that no longer exists. */
        if (tcb->system_task)
        {
            status = OS_ERR_BUSY;
        }
        /* A task executing on another core cannot be deleted from here: its context is live
         * over there. */
        else if (!is_self && (os_task_running_core(tcb) < OS_CONFIG_CORE_COUNT))
        {
            status = OS_ERR_BUSY;
        }
        /* See os_task_pause: deleting the CALLING task requires switching away from it, and a
         * locked scheduler cannot. Tearing its TCB down and then letting it run on would be far
         * worse than refusing. */
        else if (is_self && (os_kernel_lock_count[core] != 0U))
        {
            status = OS_ERR_BUSY;
        }
        else
        {
            *tcb_out     = tcb;
            *is_self_out = is_self;
            status       = OS_ERR_NONE;
        }
    }

    return status;
}

/******************************************************************************************************/
os_err_t os_task_delete(os_task_t *task)
{
    /* Task-only, like os_mutex_lock and os_notify_wait. Both of these can end up acting on the
     * CALLING task - and inside an interrupt "the calling task" is merely whichever task was
     * preempted, whether it was named by a handle or by NULL. Deleting or suspending that one
     * tears down the context the interrupt is about to return into, so the call is refused rather
     * than allowed to corrupt an innocent task.
     *
     * OS_ERR_ISR rather than INVALID_ARG: the handle is fine, the CONTEXT is not, and a caller
     * sent to inspect its arguments is looking in the wrong place. */
    os_err_t status = OS_ERR_ISR;

    if (!os_arch_in_isr())
    {
        uint32_t       core;
        os_task_tcb_t *tcb     = NULL;
        bool           is_self = false;

        os_critical_enter();
        core = os_arch_core_id_get();

        status = os_task_delete_resolve(task, core, &tcb, &is_self);

        if (status == OS_ERR_NONE)
        {
            /* See os_task_pause: pass on an unconsumed wake before this task's TCB
             * is torn down, so the notification is not silently dropped. */
            os_task_wake_compensate(tcb);

            os_task_unlink(tcb);

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
        }

        os_critical_exit();
    }

    return status;
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
 * User priorities only: the idle task and the kernel's own tasks are refused, as os_task_pause and
 * os_task_delete refuse them.
 *
 * Takes effect immediately whatever the task is doing: a READY task moves between ready lists, a
 * RUNNING one may be preempted on the spot, a blocked one is re-sorted in its waiter list.
 *
 * The new base priority is combined with all waiters on mutexes the task owns, and the resulting
 * effective priority is propagated to any mutex owner this task is waiting behind.
 *
 * @param[in,out] task      Task handle, or NULL for the calling task.
 * @param[in]     priority  New priority: OS_TASK_PRIO_1..OS_TASK_PRIO_30 (or any value in that range).
 * @return os_err_t  OK; INVALID_ARG for an unknown handle or an out-of-range priority;
 *                    BUSY for the idle task or a kernel service task.
 */
os_err_t os_task_priority_set(os_task_t *task, os_task_priority_t priority)
{
    uint32_t       core;
    uint32_t       value  = (uint32_t)priority;
    os_err_t       status = OS_ERR_INVALID_ARG;
    os_task_tcb_t *tcb;

    /* Cast for the same reason as in os_task_create above. */
    if ((value >= (uint32_t)OS_TASK_PRIO_1_LOWEST) && (value <= (uint32_t)OS_TASK_PRIO_30_HIGHEST))
    {
        os_critical_enter();
        core = os_arch_core_id_get();

        tcb = (task == NULL) ? os_task_self_tcb()
                             : ((task->id == 0U) ? NULL : os_task_find_by_id(task->id));

        if (tcb == &os_task_idle_tcb[core])
        {
            status = OS_ERR_BUSY;
        }
        else if (tcb == NULL)
        {
            status = OS_ERR_INVALID_ARG;
        }
        else if (tcb->system_task)
        {
            status = OS_ERR_BUSY;
        }
        else
        {
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
            tcb->base_priority = value;
            os_task_mutex_priority_recompute(tcb);
#else
            os_task_effective_priority_set(tcb, value);
#endif
            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Get a task's priority (NULL means the calling task).
 *
 * Reports the priority the application set, not a priority-inheritance boost in force right now,
 * which is what makes the value safe to save and restore around a section.
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

/******************************************************************************************************/
/**
 * @brief Get a task's name, as OS_TASK_DEFINE spelled it.
 *
 * For an application's own diagnostics; the kernel never reads a name itself. The pointer is safe
 * to hold, since every name is a string literal, but it does not prove the task is still alive.
 *
 * With OS_CONFIG_TASK_NAME_ENABLE at 0 the answer is NULL for every task. That is the documented
 * result rather than an error.
 *
 * @param[in] task  Task handle, or NULL for the calling task.
 * @return const char*  The task's name, or NULL for an unknown handle, an unnamed task, or a build
 *                      compiled without task names.
 */
const char* os_task_name_get(const os_task_t *task)
{
#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
    const os_task_tcb_t *tcb;
    const char          *name;

    os_critical_enter();

    tcb = (task == NULL) ? os_task_self_tcb()
                         : ((task->id == 0U) ? NULL : os_task_find_by_id(task->id));

    name = (tcb == NULL) ? NULL : OS_TASK_NAME_OF(tcb);

    os_critical_exit();
    return name;
#else
    (void)task;
    return NULL;
#endif
}

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the minimum stack headroom a task has ever had, in bytes.
 *
 * Counts the fill-pattern bytes still untouched at the bottom of the stack.
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
     * published if the id still names the same live task.
     *
     * Formally still a data race, recorded so a static-analysis run does not report it as new: a
     * peer core can be writing these bytes. The re-check does not remove the race, it removes the
     * wrong ANSWER - a torn read is thrown away by the identity test. Locking instead would put an
     * O(stack_bytes) walk inside the cross-core critical section, for a diagnostic. */
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
    uint32_t       core    = os_arch_core_id_get();
    os_task_tcb_t *current = os_task_current[core];
    void          *handle  = NULL;

    if ((current != NULL) && (current != &os_task_idle_tcb[core]))
    {
        handle = (void *)current;
    }

    return handle;
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
 * @brief Put a task in the delay list, keyed by how far away its wake-up is.
 *
 * A DELTA list: kept in wake-up order, each entry holding the ticks it waits AFTER the one in
 * front. Only the head is measured against the present, so the tick decrements ONE entry instead of
 * walking every sleeper. The cost moves to this insert, which is the right trade - a task blocks
 * once per wait, the tick fires a thousand times a second.
 *
 * Deltas rather than absolute wake ticks: nothing here ever compares two times, so there is no wrap
 * to be safe against and no cap on how long a single delay may be. See doc/design.md, "The delay
 * and timer lists".
 *
 * Caller holds a critical section.
 *
 * @param[in,out] tcb    Task to insert; must not already be in the list.
 * @param[in]     ticks  Ticks from now until it should wake. Never OS_WAIT_FOREVER - those sleepers
 *                       are in no list at all.
 * @return None.
 */
static void os_task_delay_insert(os_task_tcb_t *tcb, uint32_t ticks)
{
    os_list_node_t *node      = os_task_delay_list.head;
    uint32_t        remaining = ticks;

    while (node != NULL)
    {
        os_task_tcb_t *entry = OS_TASK_TCB_FROM_NODE(node);

        if (entry->delay_ticks > remaining)
        {
            /* We wake first, so this entry now waits only the difference after us. */
            entry->delay_ticks -= remaining;
            break;
        }

        /* It wakes before us: its share of the wait is spent, and what is left is ours. */
        remaining -= entry->delay_ticks;
        node        = node->next;
    }

    tcb->delay_ticks = remaining;

    /* A NULL position appends, which is exactly the "we wake last" case the loop falls out of. */
    os_list_insert_before(&os_task_delay_list, node, &tcb->state_node);
}

/******************************************************************************************************/
/**
 * @brief Take a task out of the delay list, handing its remaining share to whoever follows it.
 *
 * A delta list only means anything as long as the chain of differences is unbroken: an entry
 * removed from the middle was carrying part of the wait of everything behind it, and dropping that
 * silently would wake all of them early by exactly its delta.
 *
 * Caller holds a critical section; the task must be in the list.
 *
 * @param[in,out] tcb  Task to remove.
 * @return None.
 */
static void os_task_delay_remove(os_task_tcb_t *tcb)
{
    os_list_node_t *next = tcb->state_node.next;

    if (next != NULL)
    {
        OS_TASK_TCB_FROM_NODE(next)->delay_ticks += tcb->delay_ticks;
    }

    os_list_remove(&os_task_delay_list, &tcb->state_node);
}

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

    /* No time has passed, so no sleeper can be due and the list is left alone. */
    if (elapsed_ticks != 0U)
    {
        /* Only finite-delay sleepers live in the delay list, so the cost is
         * O(sleeping tasks), not O(task table). OS_WAIT_FOREVER sleepers are in
         * no list and only os_task_wake releases them. The kernel mask is raised
         * so a preempting ISR cannot resize the list mid-walk, and on multi-core
         * builds the cross-core spinlock additionally excludes the other cores'
         * os_task_wait_begin/os_task_sleep_ticks callers, who insert into this
         * same shared delay list under os_critical_enter. */
        mask_state = os_arch_kernel_mask_save();
        os_critical_multicore_lock();

        /* The list is a delta list (os_task_delay_insert), so only the HEAD is measured against
         * now. Everything behind it is measured against the entry in front, which means the walk
         * stops at the first entry that is not due yet - and on the overwhelming majority of ticks
         * that is the first entry it looks at. */
        node = os_task_delay_list.head;
        while (node != NULL)
        {
            os_list_node_t *next_node = node->next; /* the node may leave the list below */
            os_task_tcb_t  *tcb       = OS_TASK_TCB_FROM_NODE(node);

            if (tcb->delay_ticks > elapsed_ticks)
            {
                tcb->delay_ticks -= elapsed_ticks;
                break;
            }
            else
            {
                /* Due. Spend its share of the elapsed time and carry the rest to the next entry,
                 * which is what lets one announced window (tickless) expire several sleepers in
                 * the right order rather than only the first. */
                elapsed_ticks -= tcb->delay_ticks;

                /* Unlink leaves the delay list and any object waiter list;
                 * wait_signaled stays false = the wait timed out. Zeroed first so the delta this
                 * entry hands to its successor on the way out is nothing - it has none left. */
                tcb->delay_ticks = 0U;
                os_task_unlink(tcb);
                os_task_make_ready(tcb);

                /* Tell whichever core may run it, exactly as every other wake path does. Making a
                 * task READY only puts it in a list; something still has to look.
                 *
                 * Single-core gets away without this because os_tick_handler re-checks at the
                 * end. Across cores that check is worthless: it asks only about THIS core's
                 * running task, so a task pinned elsewhere was woken here and its own core never
                 * told. If that core was idle in a WFI/WFE, the poll that would have found it was
                 * itself what needed waking - the task stayed READY and the core stayed asleep.
                 * Not a latency bug: the wake is lost outright.
                 *
                 * Safe here for the same reason as in os_task_wake_locked: the caller holds the
                 * cross-core spinlock, and the IPI callback takes no lock of its own. */
                if (os_kernel_is_running())
                {
                    os_task_preempt_request(tcb);
                }
            }

            node = next_node;
        }

        os_critical_multicore_unlock();
        os_arch_kernel_mask_restore(mask_state);
    }
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
    uint32_t             mask_state = os_arch_kernel_mask_save();
    uint32_t             minimum    = UINT32_MAX;
    const os_list_node_t *head;

    /* See os_task_tick_update: the cross-core spinlock excludes the other
     * cores' insertions into this same shared delay list. */
    os_critical_multicore_lock();

    /* One read. The list is kept in wake-up order (os_task_delay_insert), so the earliest deadline
     * IS the head - there is no minimum left to search for.
     *
     * This used to walk the whole list, on the idle path, holding the cross-core spinlock, every
     * time a tickless window was planned. That walk is what the once-per-tick planning guard in
     * os_tickless_idle_process exists to ration: core 0's idle loop otherwise spent its time taking
     * and releasing the very lock the other core's tasks needed. The guard is still there and still
     * correct, but it is no longer paying for anything expensive. */
    head = os_task_delay_list.head;

    if (head != NULL)
    {
        minimum = OS_TASK_TCB_FROM_NODE(head)->delay_ticks;
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

    if (ticks != 0U)
    {
        os_critical_enter();

        core    = os_arch_core_id_get();
        current = os_task_current[core];

        /* Only a real task in task context can block, and only with the scheduler
         * open: marking a task BLOCKED whose switch-out the lock then defers would
         * leave it running in a state the kernel believes is parked. Callers see
         * the same "could not block" behaviour they get from an ISR - os_delay
         * busy-waits instead. */
        if (os_kernel_is_running() && !os_arch_in_isr() &&
            (os_kernel_lock_count[core] == 0U) &&
            (current != NULL) && (current != &os_task_idle_tcb[core]))
        {
            current->delay_ticks = ticks;
            current->state       = OS_TASK_STATE_BLOCKED;

            /* Finite delays wait in the delay list; forever-sleepers stay out of
             * every list until os_task_wake releases them. The running task is in
             * no ready list, so no unlink is needed first. */
            if (ticks != OS_WAIT_FOREVER)
            {
                os_task_delay_insert(current, ticks);

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
                /* A deadline core 0 may already have committed to sleeping through. */
                os_tickless_deadline_armed();
#endif
            }

            /* The switch is taken as soon as the critical section is left. */
            os_task_switch_request();
        }

        os_critical_exit();
    }
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

    /* Id 0 names no task, so there is nothing to look up and nothing to wake. */
    if (task_id != 0U)
    {
        os_critical_enter();
        tcb = os_task_find_by_id(task_id);
        os_task_wake_locked(tcb);
        os_critical_exit();
    }
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
    if ((current != NULL) && (current != &os_task_idle_tcb[core]))
    {
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
            os_task_delay_insert(current, timeout_ticks);

            #if (OS_CONFIG_TICKLESS_ENABLE == 1U)
            /* Same reason as os_task_sleep_ticks: a timeout armed here can fall inside a window
             * core 0 planned before it existed. */
            os_tickless_deadline_armed();
            #endif
        }

        os_task_switch_request();
    }
}

/******************************************************************************************************/
/**
 * @brief Drop the calling task's pending-wake state: call on every path where a blocking
 *        primitive stops retrying and returns to its caller, signaled or not.
 *
 * This is what keeps os_task_wake_compensate's window narrow. Until this call the task holds an
 * unconsumed wake that a pause or delete must pass on; from here the notification is spent and the
 * list it came from may not even exist by the time the task blocks on something else.
 *
 * No-op from interrupt context: an ISR borrows the interrupted task's identity and must not
 * discard that task's own wait state.
 *
 * @return None.
 */
void os_task_wait_end(void)
{
    /* An interrupt has no wait of its own to close out. */
    if (!os_arch_in_isr())
    {
        os_critical_enter();
        os_task_wait_end_locked();
        os_critical_exit();
    }
}

/******************************************************************************************************/
/**
 * @brief os_task_wait_end for a caller that already holds the critical section.
 *
 * Split out because almost every caller does. A blocking primitive that SUCCEEDS closes its wait
 * inside the same critical section that saw the object free - the acquire and the close have to be
 * one indivisible step - so the section os_task_wait_end takes for itself is a second, nested one
 * on the hot path of every one of them. Nested is cheaper than outermost (no spinlock), but it is
 * still a call and two mask operations per successful take, on both core counts.
 *
 * The timeout paths are the ones that genuinely need os_task_wait_end: they are reached after the
 * critical section that started the wait has already been left. Same body either way.
 *
 * @return None.
 */
void os_task_wait_end_locked(void)
{
    os_task_tcb_t *current;

    /* An interrupt has no wait of its own to close out. */
    if (!os_arch_in_isr())
    {
        current = os_task_current[os_arch_core_id_get()];

        /* The caller's lock is what protects the core/current pair against migration, and the
         * mutex wait edge against a priority-inheritance walk on another core. */
        if (current != NULL)
        {
            current->wait_signaled = false;
            current->woken_from    = NULL;
    #if (OS_CONFIG_MUTEX_ENABLE == 1U)
            /* Backstop only. Every path that leaves a mutex wait without acquiring calls
             * os_task_mutex_waiter_depart first, which is what actually releases the boost; this makes
             * sure no exit route can leave a stale id behind to be spent on the wrong owner later. */
            current->pi_owner_id   = 0U;

            /* Cleared here rather than in os_mutex.c because every way out of a wait passes through
             * this call, so no exit path can leave a stale edge behind for a later walk to follow. */
            current->blocked_on_mutex = NULL;
            current->blocked_forever  = false;
    #endif
        }
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
    uint32_t       mask_state = os_internal_migration_lock();
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];
    bool           signaled = (current != NULL) ? current->wait_signaled : false;

    os_internal_migration_unlock(mask_state);

    return signaled;
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
    uint32_t       mask_state = os_internal_migration_lock();
    os_task_tcb_t *current = os_task_current[os_arch_core_id_get()];
    uint32_t       result = (current != NULL) ? current->wait_result : 0U;

    os_internal_migration_unlock(mask_state);

    return result;
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
            /* os_task_unlink hands the delay-list delta to the successor itself; the
             * real remaining budget is recomputed from the wall clock by the caller. */
            os_task_unlink(tcb);              /* leaves the delay list and the waiter list */
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
    os_list_node_t *node  = waiters->head;
    bool            woken = false;

    if (node != NULL)
    {
        os_task_tcb_t *tcb = OS_TASK_TCB_FROM_WAIT_NODE(node);

        os_task_unlink(tcb);              /* leaves the delay list and the waiter list */
        tcb->wait_signaled = true;
        tcb->woken_from    = waiters;     /* until consumed: see os_task_wake_compensate */
        os_task_make_ready(tcb);

        if (os_kernel_is_running())
        {
            os_task_preempt_request(tcb);
        }

        woken = true;
    }

    return woken;
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
    uint32_t       mask_state = os_internal_migration_lock();
    os_task_tcb_t *tcb = os_task_current[os_arch_core_id_get()];
    uint32_t       id = (tcb == NULL) ? 0U : tcb->id;

    /* A core identity is only valid while this task cannot migrate to another core. Read both
     * the pointer and its identity before restoring the mask; returning a pointer is not enough. */
    os_internal_migration_unlock(mask_state);

    return id;
}

/******************************************************************************************************/
/**
 * @brief Check whether the calling core's idle task is currently running (ISR-safe).
 *
 * @return bool  True when the current task is this core's idle task.
 */
bool os_task_current_is_idle(void)
{
    uint32_t mask_state = os_internal_migration_lock();
    uint32_t core = os_arch_core_id_get();
    bool     idle = (os_task_current[core] == &os_task_idle_tcb[core]);

    os_internal_migration_unlock(mask_state);

    return idle;
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

    /* No cross-core spinlock here, and that is the point of the function.
     *
     * This runs from EVERY core's tick, a thousand times a second, to answer what is usually one
     * bitmap AND. Taking the global lock for it put two cores in contention on the kernel's single
     * hottest shared object at that rate, for a question neither of them is going to act on
     * directly.
     *
     * What makes dropping it safe is that the answer is a HINT and nothing else. Both callers use
     * it only to decide whether to pend PendSV, and PendSV re-derives the whole decision under the
     * lock in os_task_stack_select_next(). A false positive costs one PendSV that looks and changes
     * nothing; a false negative costs one tick of latency, and cannot lose a wake - anything urgent
     * enough to need one arrived through os_task_preempt_request, which pends PendSV or sends an
     * IPI directly rather than waiting to be noticed here.
     *
     * The reads themselves are sound without it:
     *
     *   os_kernel_lock_count[core]   this core's own, and the kernel mask above holds off the only
     *   os_task_current[core]        thing that writes them from here - this core's PendSV, which
     *   os_task_slice_left[core]     is below the tick in priority and cannot preempt it anyway.
     *
     *   os_task_ready_bitmap         another core may be changing it. One aligned 32-bit load, so
     *   current->priority            never torn; at worst a tick out of date, which is exactly the
     *                                stale-hint case above. current->priority can be moved by a
     *                                priority-inheritance boost on another core, with the same
     *                                consequence and no other.
     *
     * The mask stays. It is what makes the three per-core reads stable, and it costs two
     * instructions where the lock cost an uncontended round trip and, on a busy second core, a
     * spin. */
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
    os_err_t       status = OS_ERR_INVALID_ARG;
    os_task_tcb_t *tcb;
    uint32_t       running;

    /* Bits naming cores that do not exist are rejected rather than ignored. An empty mask is
     * OS_TASK_CORE_ANY and passes: it is a real request, not a missing one. */
    if ((core_affinity >> OS_CONFIG_CORE_COUNT) == 0U)
    {
        os_critical_enter();

        /* NULL means the calling task here too, so this no longer stands out as the one task
         * call that refuses the shorthand every other one accepts. */
        if (task == NULL)
        {
            tcb = os_task_self_tcb();
        }
        else
        {
            tcb = (task->id != 0U) ? os_task_find_by_id(task->id) : NULL;
        }

        if (tcb != NULL)
        {
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

            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    return status;
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
        os_arch_soc_idle_cb();
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
    os_err_t status = OS_ERR_NONE;
    uint32_t core;

    /* One idle task per scheduling core; each is pinned to its core and is
     * never queued in a ready list (it is the empty-bitmap fallback). Retry only a missing
     * core's task: a successful core 0 does not imply later cores initialized successfully. */
    for (core = 0U;
         (core < OS_CONFIG_CORE_COUNT) && (status == OS_ERR_NONE);
         core++)
    {
        os_task_tcb_t *tcb = &os_task_idle_tcb[core];
        uint32_t      *stack_ptr;

        if (tcb->state == OS_TASK_STATE_INACTIVE)
        {
#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
            os_task_stack_fill(os_task_idle_stack[core], sizeof(os_task_idle_stack[core]));
#endif
#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
            /* After any fill, which would otherwise overwrite it. */
            os_task_stack_guard_set(os_task_idle_stack[core]);
#endif

            stack_ptr = os_arch_task_stack_initialize(os_task_idle_stack[core],
                                                      sizeof(os_task_idle_stack[core]),
                                                      os_task_idle_entry, NULL);
            if (stack_ptr == NULL)
            {
                status = OS_ERR_ERROR;
            }
            else
            {
#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
                tcb->name          = "tsk_idle";
#endif
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
        }
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Check whether every core's idle task is already created.
 *
 * @return bool  True when every idle task has an initialized stack.
 */
bool os_task_idle_is_created(void)
{
    bool created = true;

    for (uint32_t core = 0U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        if ((os_task_idle_tcb[core].state == OS_TASK_STATE_INACTIVE) ||
            (os_task_idle_tcb[core].stack_ptr == NULL))
        {
            created = false;
        }
    }

    return created;
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

    {
        uint32_t slot;

        for (slot = 0U; slot < OS_TASK_TABLE_SIZE; slot++)
        {
            os_task_next_generation[slot] = 1U;
        }
    }

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
    }
    else if (current_task != NULL)
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
    else
    {
        /* No outgoing task on this core yet, which is the very first dispatch. */
    }

    /* One unlock and one mask restore, on every path: the deferred-switch case above used to
     * carry its own copy of both, which is exactly what a second exit costs. */
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
    uint32_t       mask_state = os_arch_kernel_mask_save();
    uint32_t       core;
    uint32_t       bitmap;
    os_task_tcb_t *next;
    os_task_tcb_t *locked_current;
    uint32_t       *result;

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
    locked_current = os_task_current[core];

    if ((os_kernel_lock_count[core] != 0U) && (locked_current != NULL) &&
        (locked_current->state == OS_TASK_STATE_RUNNING))
    {
        result = locked_current->stack_ptr;
    }
    else
    {
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

    result = next->stack_ptr;
    }

    os_critical_multicore_unlock();
    os_arch_kernel_mask_restore(mask_state);

    return result;
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
/******************************************************************************************************/
/**
 * @brief Whether a create request is well formed, before anything is written.
 *
 * Split out so os_task_create_any keeps one exit without nesting its whole body four levels
 * deep; the checks and their order are exactly the ones it used to make inline.
 *
 * @param[in] task    Handle being created through.
 * @param[in] config  Behaviour for the new task.
 * @return bool  True when every argument is usable.
 */
static bool os_task_create_args_ok(const os_task_t *task, const os_task_config_t *config)
{
    bool ok = false;

    /* The stack comes from the handle, not the config: a handle declared any way other than with
     * OS_TASK_DEFINE has no storage attached and is rejected here rather than being run on
     * whatever the object happened to contain. */
    if ((task != NULL) &&
        (task->storage != NULL) &&
        (config != NULL) &&
        (config->entry != (os_task_entry_t)0) &&
        (config->priority <= OS_TASK_PRIO_MAX) &&
        (task->storage->stack_memory != NULL) &&
        (task->storage->stack_bytes >= OS_CONFIG_MIN_STACK_SIZE) &&
        /* The affinity mask may only name existing cores (0 = any core). */
        ((config->core_affinity >> OS_CONFIG_CORE_COUNT) == 0U))
    {
        uintptr_t stack_addr = (uintptr_t)task->storage->stack_memory;

        ok = ((stack_addr % OS_ARCH_STACK_ALIGNMENT_BYTES) == 0U) &&
             ((task->storage->stack_bytes % OS_ARCH_STACK_ALIGNMENT_BYTES) == 0U);
    }

    return ok;
}

/******************************************************************************************************/
static os_err_t os_task_create_any(os_task_t *task, const os_task_config_t *config, bool system_task)
{
    os_err_t  status = OS_ERR_INVALID_ARG;
    uint32_t  index;
    uint32_t  *stack_ptr = NULL;
    bool      claimed = false;

    if (os_task_create_args_ok(task, config))
    {
        /* Refuse a handle that is already live, BEFORE anything writes to its stack.
         *
         * Placement is the whole point of this check. The pre-fill, the guard word and the initial
         * exception frame below all land in task->storage->stack_memory, and OS_TASK_DEFINE gives
         * one array per handle - so creating twice through the same handle would paint the stack of
         * the task still RUNNING on it with the fill byte and lay a fresh frame over its live
         * context. A guard sharing the slot-scan critical section further down would reject the
         * call only after that damage was done.
         *
         * Every other object in the kernel refuses re-initialisation the same way (os_mutex_init,
         * os_sem_init, os_event_init, os_queue_bind_buffer, all with OS_ERR_BUSY); a task is simply
         * the one that cannot afford to find out late. Re-checked inside the slot-scan critical
         * section, which is what makes the test and the claim atomic against a peer core creating
         * through this same handle. */
        os_critical_enter();
        status = ((task->id != 0U) && (os_task_find_by_id(task->id) != NULL))
                 ? OS_ERR_BUSY : OS_ERR_NONE;
        os_critical_exit();
    }

    if (status == OS_ERR_NONE)
    {
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
            status = OS_ERR_ERROR;
        }
    }

    if (status == OS_ERR_NONE)
    {
        os_critical_enter();

        /* The atomic half of the guard above: on SMP a peer core could have created through this
         * same handle since that check ran. The stack writes between the two are then wasted work
         * on a handle the caller had already made live, which is the caller's mistake to have made
         * - but the TCB slot and the id are still claimed exactly once. */
        if ((task->id != 0U) && (os_task_find_by_id(task->id) != NULL))
        {
            status = OS_ERR_BUSY;
        }
        else
        {
            /* No free slot until one is found; the scan ends through its own condition. */
            status = OS_ERR_FULL;

            for (index = 0U; (index < OS_TASK_TABLE_SIZE) && !claimed; index++)
            {
                if (os_task_table[index].state == OS_TASK_STATE_INACTIVE)
                {
                    os_task_tcb_t *tcb = &os_task_table[index];

                    /* This slot's next generation, which is what stops a reused slot handing the
                     * new task the previous occupant's identity - the right to unlock its mutexes,
                     * to receive its notifications. No search is needed to establish that: the id
                     * is unique by construction because no other slot can produce this slot's
                     * number, and no earlier occupant of it used this generation. */
                    uint32_t generation = os_task_next_generation[index];

#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
                    tcb->name             = task->storage->name;
#endif
                    tcb->stack_base       = (uint8_t *)task->storage->stack_memory;
                    tcb->stack_ptr        = stack_ptr;
                    tcb->stack_bytes      = task->storage->stack_bytes;
                    tcb->priority         = config->priority;
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
                    tcb->base_priority    = config->priority;
                    tcb->pi_owner_id      = 0U;
#endif
                    tcb->id               = OS_TASK_ID_MAKE(index, generation);
                    tcb->delay_ticks      = 0U;
                    tcb->core_affinity    = config->core_affinity;
                    tcb->system_task      = system_task;
                    tcb->state            = OS_TASK_STATE_SUSPENDED;

                    task->id = tcb->id;

                    /* Wrapping the generation past its 24 bits would eventually repeat an id for
                     * this slot; keeping it out of 0 keeps every id non-zero, which the whole API
                     * reads as "no task". */
                    generation = (generation + 1U) & OS_TASK_ID_GENERATION_MASK;
                    os_task_next_generation[index] = (generation == 0U) ? 1U : generation;

                    claimed = true;
                    status  = OS_ERR_NONE;
                }
            }
        }

        os_critical_exit();
    }

    return status;
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
    /* Both conditions mean "this stack is still intact", so the overflow path is the one
     * else-arm rather than two early returns. */
    if ((tcb->stack_base != NULL) &&
        !(((const uint8_t *)(const void *)stack_ptr >= tcb->stack_base) &&
          (*(const uint32_t *)(const void *)tcb->stack_base == OS_TASK_STACK_CANARY)))
    {
        os_stack_overflow_cb(OS_TASK_NAME_OF(tcb));
        os_arch_config_fault_trap();

        /* os_arch_config_fault_trap never returns; the loop only convinces the
         * compiler of that when it is inlined as a plain call. */
        while (1)
        {
        }
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
#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
        /* One tickless pass, then the ordinary idle. The pass itself decides whether this idle is
         * long enough to be worth suppressing, and on a secondary core it does nothing at all -
         * only core 0 owns the time base, so only core 0 may announce a suppressed window.
         *
         * The plain idle still follows it in every case: when the pass declined to sleep (too
         * short, or not core 0) this is the whole of the idle, and when it did sleep, the wake
         * that ended it may have left nothing runnable, so the core waits again here rather than
         * spinning back round to plan another window. */
        os_tickless_idle_process();
#endif
        os_arch_soc_idle_cb();
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
#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
    tcb->name          = NULL;
#endif
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
    tcb->blocked_on_mutex = NULL;
    tcb->blocked_forever  = false;
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
os_task_tcb_t* os_task_find_by_id(uint32_t id)
{
    os_task_tcb_t *found = NULL;

    /* Id 0 names nothing, and its slot field would underflow the subtraction below. */
    if (id != 0U)
    {
        uint32_t slot = OS_TASK_ID_SLOT(id);

        /* The slot comes from the id, so a corrupt or forged one has to be bounds-checked before it
         * indexes anything. Then the full id is compared, not just the slot: that is what makes a
         * stale handle to a recycled slot resolve to nothing, exactly as the old search did by
         * failing to find it. A slot only counts as live while its state is not INACTIVE. */
        if ((slot < OS_TASK_TABLE_SIZE) &&
            (os_task_table[slot].state != OS_TASK_STATE_INACTIVE) &&
            (os_task_table[slot].id == id))
        {
            found = &os_task_table[slot];
        }
    }

    return found;
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
        os_task_delay_remove(tcb);
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
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
        /* Revoke inheritance at the actual departure, including timeouts and forced wakes.
         * Waiting until the task is dispatched leaves owners boosted by a waiter no longer queued. */
        os_task_mutex_waiter_depart_tcb(tcb);
#endif
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
 * O(waiters) by choice, not oversight: the order has to exist somewhere, and a sorted insert puts
 * the cost on the task about to block rather than on the wake path, which often runs from an ISR.
 * It walks one object's waiters and stops at the first of lower priority, so the common shapes cost
 * one comparison. Many blocked senders of similar priority is the case that pays.
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
    uint32_t mask_state = os_internal_migration_lock();
    uint32_t core = os_arch_core_id_get();

    /* A locked scheduler remembers the request rather than taking it: os_kernel_unlock
     * raises the switch once the outermost lock is released. */
    if (os_kernel_lock_count[core] != 0U)
    {
        os_kernel_switch_pending[core] = true;
    }
    else
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }

    os_internal_migration_unlock(mask_state);
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
    bool                done = false;

#if (OS_CONFIG_CORE_COUNT > 1U)
    if ((tcb->core_affinity != OS_TASK_CORE_ANY) &&
        ((tcb->core_affinity & (1UL << core)) == 0U))
    {
        /* The task cannot run here: nudge the first core it may run on
         * (weak default IPI does nothing - that core then picks the task
         * up at its own next tick). */
        os_arch_core_ipi_request_cb(os_arch_lowest_bit_get(tcb->core_affinity));
        done = true;
    }
#endif

    if (!done)
    {
        /* Only a strictly higher-priority task warrants an immediate switch:
         * equal priorities round-robin at the tick, so skipping the PendSV here
         * both saves the full context-switch cost and stops every wake from
         * acting as an implicit yield of the waker's remaining timeslice. */
        current = os_task_current[core];

        if ((current == NULL) || (tcb->priority > current->priority))
        {
            os_task_switch_request();
            done = true;
        }
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
    if (!done)
    {
        uint32_t other;

        /* `done` ends the scan through the loop condition, so the first core nudged is
         * still the only one nudged, and the loop needs no break. */
        for (other = 0U; (other < OS_CONFIG_CORE_COUNT) && !done; other++)
        {
            bool allowed = (other != core) &&
                           ((tcb->core_affinity == OS_TASK_CORE_ANY) ||
                            ((tcb->core_affinity & (1UL << other)) != 0U));

            if (allowed)
            {
                const os_task_tcb_t *other_current = os_task_current[other];

                if ((other_current == NULL) || (tcb->priority > other_current->priority))
                {
                    os_arch_core_ipi_request_cb(other);
                    done = true;
                }
            }
        }
    }
#endif

    (void)done;
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
    uint32_t found = OS_CONFIG_CORE_COUNT;
    uint32_t core;

    /* OS_CONFIG_CORE_COUNT doubles as "not running anywhere", so it is both the initial
     * value and the sentinel that stops the search. */
    for (core = 0U; (core < OS_CONFIG_CORE_COUNT) && (found == OS_CONFIG_CORE_COUNT); core++)
    {
        if (os_task_current[core] == tcb)
        {
            found = core;
        }
    }

    return found;
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
void os_task_effective_priority_set(os_task_tcb_t *tcb, uint32_t new_priority)
{
    /* Four mutually exclusive cases over the task's state, so one chain rather than four
     * blocks that each returned for themselves. Order is unchanged. */
    if (new_priority == tcb->priority)
    {
        /* Already there: nothing to move and nothing to request. */
    }
    else if (tcb->state == OS_TASK_STATE_READY)
    {
        bool increasing = (new_priority > tcb->priority);

        os_task_unlink(tcb);     /* leaves the OLD priority's bucket  */
        tcb->priority = new_priority;
        os_task_make_ready(tcb); /* rejoins at the NEW priority's bucket */

        if (increasing && os_kernel_is_running())
        {
            os_task_preempt_request(tcb);
        }
    }
    else if ((tcb->state == OS_TASK_STATE_RUNNING) && (new_priority < tcb->priority) &&
             os_kernel_is_running())
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
    }
    else
    {
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
}
