/**
 * @file os_work.c
 * @brief Deferrable work queue: items run on a kernel task at OS_CONFIG_WORK_PRIORITY (the
 *        highest priority by default).
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

#include <string.h>

#if (OS_CONFIG_WORK_ENABLE == 1U)

/* See os_timer.c: checked with _Static_assert because an os_task_priority_t name is an enum
 * constant, which the preprocessor would read as 0. */
_Static_assert((OS_CONFIG_WORK_PRIORITY >= OS_TASK_PRIO_1_LOWEST) &&
               (OS_CONFIG_WORK_PRIORITY <= OS_TASK_PRIO_MAX),
               "OS_CONFIG_WORK_PRIORITY must be OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_MAX");

#if (OS_CONFIG_WORK_PAYLOAD_SIZE < 1U)
#error "OS_CONFIG_WORK_PAYLOAD_SIZE must be at least 1; the work registry sizes its payload copy from it."
#endif

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(tsk_work, OS_CONFIG_WORK_STACK_SIZE);

/* Resolved once in os_work_system_init: the work task is never deleted, so
 * this handle stays valid for the kernel's lifetime and every later wake
 * (submit and the tick-time expiry path) skips the id lookup. */
static void                *os_work_task_tcb = NULL;

/* One submitted call: what to run, the payload to run it on, and how long is left before it may.
 *
 * The submission lives here rather than in an object the caller owns, which is what lets
 * os_work_submit take a handler and a payload instead of a work item. A slot is free exactly when
 * its handler is NULL, so there is no separate in-use flag to keep in step, and "pending" is simply
 * a slot that is taken but not yet ready.
 *
 * The payload is a copy, not a pointer, so a caller may submit from a local buffer and return
 * immediately. The union is what gives that copy an alignment any type can be read back at: the
 * handler casts it to its own struct, and a bare uint8_t array would only be byte-aligned. */
typedef struct
{
    os_work_handler_t handler;     /**< NULL marks the slot free.                    */
    uint32_t          delay_ticks; /**< Remaining ticks until the call becomes ready. */
    size_t            len;         /**< Payload bytes in use; 0 when there is none.   */
    bool              ready;       /**< Delay elapsed, awaiting execution.            */

    union
    {
        uint64_t alignment;
        uint8_t  bytes[OS_CONFIG_WORK_PAYLOAD_SIZE];

    } payload;

} os_work_entry_t;

/* Registry of submitted calls, advanced on every kernel tick. Fixed slots so tick-time iteration
 * stays safe against concurrent submissions. Every field is read and written either inside
 * os_critical_enter/exit or under the kernel mask and cross-core spinlock that
 * os_work_tick_process holds, which is what makes plain (non-volatile) access correct here. */
static os_work_entry_t     os_work_registry[OS_CONFIG_MAX_WORKS];

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void       os_work_task_entry(void *context);
static bool       os_work_ready_fetch(os_work_handler_t *handler_out, void *payload_out, size_t *len_out);
static bool       os_work_ready_exists(void);
static uint32_t   os_work_registry_slot_free(void);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Submit a function to run after delay_ms on the kernel work task.
 *
 * ISR-safe. delay_ms == 0 makes the call ready immediately; at the default
 * OS_CONFIG_WORK_PRIORITY the work task then preempts any user task as soon as the scheduler is
 * invoked - lower that priority and it waits its turn like anything else.
 *
 * The handler AND the payload are copied into a free slot, so a local buffer may go out of scope
 * the moment this returns. Each submission is independent and none can be cancelled afterwards.
 * The copy runs inside the critical section, so keep payloads small: OS_CONFIG_WORK_PAYLOAD_SIZE
 * bounds them and anything larger is refused rather than truncated. Pass a pointer to hand over
 * more, which puts the lifetime question visibly back in the caller's hands.
 *
 * @param[in] handler   Function to run on the kernel work task.
 * @param[in] data      Payload to copy, or NULL when len is 0.
 * @param[in] len       Payload size in bytes, at most OS_CONFIG_WORK_PAYLOAD_SIZE.
 * @param[in] delay_ms  Delay before execution in milliseconds (0 = as soon as possible).
 * @return os_status  OK on submission; INVALID_ARG for a NULL handler, OS_WAIT_FOREVER, an
 *                    oversized len, or a NULL data with a nonzero len; FULL when every registry
 *                    slot is occupied.
 */
os_status os_work_submit(os_work_handler_t handler, const void *data, size_t len, uint32_t delay_ms)
{
    os_status status = OS_STATUS_INVALID_ARG;

    if ((handler != NULL) && (delay_ms != OS_WAIT_FOREVER) &&
        (len <= OS_CONFIG_WORK_PAYLOAD_SIZE) &&
        ((len == 0U) || (data != NULL)))
    {
        uint32_t delay_ticks = OS_TICKS_FROM_MS(delay_ms);
        uint32_t slot;

        os_critical_enter();

        slot = os_work_registry_slot_free();

        if (slot >= OS_CONFIG_MAX_WORKS)
        {
            status = OS_STATUS_FULL;
        }
        else
        {
            os_work_registry[slot].handler     = handler;
            os_work_registry[slot].delay_ticks = delay_ticks;
            os_work_registry[slot].len         = len;
            os_work_registry[slot].ready       = (delay_ticks == 0U);

            if (len > 0U)
            {
                (void)memcpy(os_work_registry[slot].payload.bytes, data, len);
            }

            if (delay_ticks == 0U)
            {
                /* Direct-handle wake: skips the id lookup os_task_wake would do;
                 * safe here because os_critical_enter above already holds the
                 * kernel mask (and, on multi-core builds, the same spinlock
                 * os_task_wake_tcb requires the caller to hold). */
                os_task_wake_tcb(os_work_task_tcb);
            }

            status = OS_STATUS_OK;
        }

        os_critical_exit();
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Create and start the kernel work service task. Called from os_init.
 *
 * @return os_status  Status code.
 */
os_status os_work_system_init(void)
{
    uint32_t  slot;
    os_status status;

    os_task_config_t config =
    {
        os_work_task_entry,
        NULL,
        OS_CONFIG_WORK_PRIORITY,
        OS_CONFIG_WORK_CORE_AFFINITY
    };

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        os_work_registry[slot].handler     = NULL;
        os_work_registry[slot].delay_ticks = 0U;
        os_work_registry[slot].len         = 0U;
        os_work_registry[slot].ready       = false;
    }

    status = os_task_create_system(&tsk_work, &config);

    /* Each step only runs once the previous one succeeded, and the first failure
     * is what gets reported. */
    if (status == OS_STATUS_OK)
    {
        status = os_task_start(&tsk_work);
    }

    if (status == OS_STATUS_OK)
    {
        /* Resolved once: the work task is never deleted, so every later wake
         * (submit and the tick-time expiry path) can skip the id lookup. */
        os_work_task_tcb = os_task_tcb_resolve(tsk_work.id);
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Advance delayed work items by elapsed ticks. Called from the tick interrupt.
 *
 * @param[in] elapsed_ticks  Number of elapsed ticks.
 * @return None.
 */
void os_work_tick_process(uint32_t elapsed_ticks)
{
    uint32_t mask_state;
    uint32_t slot;
    bool     wake_needed = false;

    if (elapsed_ticks > 0U)
    {
        /* The kernel mask is raised so a preempting ISR submitting or cancelling
         * cannot interleave with the pending-check/ready-write pair below
         * (a cancel landing in between would be silently undone). Also covers
         * the tickless announce path, which calls this from task context. On
         * multi-core builds the cross-core spinlock additionally excludes the
         * other cores' os_work_submit callers, who hold it via
         * os_critical_enter - the local mask alone only stops this core's own
         * interrupts. Both are held across the os_task_wake_tcb below, which is
         * exactly what that call requires of its caller (unlike os_task_wake,
         * which takes the same non-recursive lock itself and so could not be
         * called from in here). */
        mask_state = os_arch_kernel_mask_save();
        os_critical_multicore_lock();

        for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
        {
            os_work_entry_t *entry = &os_work_registry[slot];

            /* Taken but not yet ready is exactly what "pending" used to mean. */
            if ((entry->handler == NULL) || entry->ready)
            {
                continue;
            }

            if (entry->delay_ticks > elapsed_ticks)
            {
                entry->delay_ticks -= elapsed_ticks;
            }
            else
            {
                entry->delay_ticks = 0U;
                entry->ready       = true;
                wake_needed        = true;
            }
        }

        if (wake_needed)
        {
            /* Direct-handle wake: skips both the id lookup and the nested
             * critical section os_task_wake would pay on every expiring tick;
             * safe here because the kernel mask and (multi-core) spinlock this
             * function already holds are exactly what os_task_wake_tcb
             * requires the caller to provide. */
            os_task_wake_tcb(os_work_task_tcb);
        }

        os_critical_multicore_unlock();
        os_arch_kernel_mask_restore(mask_state);
    }
}

/******************************************************************************************************/
/**
 * @brief Return ticks until the next delayed work item becomes ready (tickless planning).
 *
 * @return uint32_t  Minimum remaining delay in ticks, 0 when an item is
 *                   already ready, UINT32_MAX when nothing is registered.
 */
uint32_t os_work_next_ready_ticks_get(void)
{
    uint32_t slot;
    uint32_t minimum = UINT32_MAX;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        const os_work_entry_t *entry = &os_work_registry[slot];

        if (entry->handler == NULL)
        {
            continue;
        }

        if (entry->ready)
        {
            minimum = 0U;
            break;
        }

        if (entry->delay_ticks < minimum)
        {
            minimum = entry->delay_ticks;
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
 * @brief Work task body: execute ready items, sleep until woken otherwise.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_work_task_entry(void *context)
{
    (void)context;

    /* The payload is delivered on this task's own stack, which is why the slot can be released
     * before the handler runs: the handler reads this copy, not the registry. Sized by
     * OS_CONFIG_WORK_PAYLOAD_SIZE, so OS_CONFIG_WORK_STACK_SIZE has to allow for it. */
    union
    {
        uint64_t alignment;
        uint8_t  bytes[OS_CONFIG_WORK_PAYLOAD_SIZE];

    } payload;

    while (1)
    {
        os_work_handler_t handler;
        size_t            len;

        if (os_work_ready_fetch(&handler, payload.bytes, &len))
        {
            handler((len > 0U) ? (void *)payload.bytes : NULL, len);
            continue;
        }

        /* The emptiness check and the block form one atomic unit (outer
         * critical section), so a submission arriving in between cannot be
         * lost: it is seen here, or its wake lands after the block. */
        os_critical_enter();

        if (!os_work_ready_exists())
        {
            os_task_sleep_ticks(OS_WAIT_FOREVER);
        }

        os_critical_exit();
    }
}

/******************************************************************************************************/
/**
 * @brief Take one ready submission out of the registry, returning what to call and its payload.
 *
 * The payload is copied OUT to the caller's buffer before the slot is freed, and the slot is freed
 * before the handler runs. Both halves of that matter: freeing early means a handler may submit
 * again without needing the registry to have spare capacity, and copying first means the handler
 * is not reading a slot that the very next submission is free to overwrite.
 *
 * @param[out] handler_out  Handler to invoke, written only when true is returned.
 * @param[out] payload_out  Buffer of at least OS_CONFIG_WORK_PAYLOAD_SIZE bytes to copy into.
 * @param[out] len_out      Payload bytes copied, written only when true is returned.
 * @return bool  true when a ready submission was taken.
 */
static bool os_work_ready_fetch(os_work_handler_t *handler_out, void *payload_out, size_t *len_out)
{
    bool     found = false;
    uint32_t slot;

    os_critical_enter();

    for (slot = 0U; slot < OS_CONFIG_MAX_WORKS; slot++)
    {
        os_work_entry_t *entry = &os_work_registry[slot];

        if ((entry->handler != NULL) && entry->ready)
        {
            *handler_out = entry->handler;
            *len_out     = entry->len;

            if (entry->len > 0U)
            {
                (void)memcpy(payload_out, entry->payload.bytes, entry->len);
            }

            entry->handler     = NULL; /* frees the slot */
            entry->delay_ticks = 0U;
            entry->len         = 0U;
            entry->ready       = false;

            found = true;
            break;
        }
    }

    os_critical_exit();
    return found;
}

/******************************************************************************************************/
/**
 * @brief Check whether any submission is ready to run. Caller holds the critical section.
 *
 * @return bool  True when at least one awaits execution.
 */
static bool os_work_ready_exists(void)
{
    uint32_t slot;
    bool     exists = false;

    for (slot = 0U; (slot < OS_CONFIG_MAX_WORKS) && (!exists); slot++)
    {
        if ((os_work_registry[slot].handler != NULL) && os_work_registry[slot].ready)
        {
            exists = true;
        }
    }

    return exists;
}

/******************************************************************************************************/
/**
 * @brief Find the first free registry slot. Caller holds the critical section.
 *
 * One pass and one condition, where a handle-based API needed two - a scan for the item's own slot
 * so a re-submission rescheduled in place, plus a fallback to a free one. Without a handle there is
 * nothing to match: every submission is new, so the first slot with no handler will do.
 *
 * @return uint32_t  Slot index, or OS_CONFIG_MAX_WORKS when the registry is full.
 */
static uint32_t os_work_registry_slot_free(void)
{
    uint32_t slot;
    uint32_t found = OS_CONFIG_MAX_WORKS;

    for (slot = 0U; (slot < OS_CONFIG_MAX_WORKS) && (found == OS_CONFIG_MAX_WORKS); slot++)
    {
        if (os_work_registry[slot].handler == NULL)
        {
            found = slot;
        }
    }

    return found;
}

#endif /* OS_CONFIG_WORK_ENABLE */
