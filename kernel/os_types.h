/**
 * @file os_types.h
 * @brief Status codes, handles and the values every other header needs.
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_TYPES_H
#define OS_TYPES_H

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Status codes and task types
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Common status code for Ahura kernel APIs.
 */
typedef enum
{
    OS_ERR_NONE        = 0, /**< Operation succeeded.                          */
    OS_ERR_ERROR       = 1, /**< Generic failure.                              */
    OS_ERR_INVALID_ARG = 2, /**< A required argument was invalid or NULL.      */
    OS_ERR_EMPTY       = 3, /**< Object holds no items/tokens.                 */
    OS_ERR_FULL        = 4, /**< Object cannot accept more items/tokens.       */
    OS_ERR_BUSY        = 5, /**< Object unavailable without blocking.          */
    OS_ERR_TIMEOUT     = 6, /**< Wait aborted because the timeout elapsed.     */
    OS_ERR_NOT_OWNER   = 7, /**< Caller does not own the object.               */
    OS_ERR_NO_MEMORY   = 8, /**< Kernel heap could not satisfy the request.    */
    OS_ERR_ISR         = 9, /**< Task-only call made from interrupt context.   */

} os_err_t;

/******************************************************************************************************/
/**
 * @brief Task lifecycle state.
 */
typedef enum
{
    OS_TASK_STATE_INACTIVE  = 0, /**< Not created / deleted.                    */
    OS_TASK_STATE_READY     = 1, /**< Runnable, waiting for the CPU.            */
    OS_TASK_STATE_RUNNING   = 2, /**< Currently executing.                      */
    OS_TASK_STATE_BLOCKED   = 3, /**< Waiting for a delay/timeout to expire.    */
    OS_TASK_STATE_SUSPENDED = 4, /**< Paused until os_task_start is called.     */

} os_task_state_t;

/******************************************************************************************************/
/**
 * @brief Task entry function signature.
 */
typedef void (*os_task_entry_t)(void *context);

/******************************************************************************************************/
/**
 * @brief What a task is called and where its stack lives.
 *
 * OS_TASK_DEFINE fills one of these at compile time and points the handle at it, which is why
 * os_task_create needs neither a name nor a stack. Const, so it costs flash rather than RAM.
 * OS_CONFIG_TASK_NAME_ENABLE at 0 removes the name field outright, so a hand-rolled descriptor
 * naming .name in such a build fails to compile.
 */
typedef struct
{
#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
    const char *name;           /**< Handle's own spelling, as written in OS_TASK_DEFINE. */
#endif
    void       *stack_memory;
    size_t     stack_bytes;

} os_task_storage_t;

/******************************************************************************************************/
/**
 * @brief Public task handle object. Declare one with OS_TASK_DEFINE, never by hand: os_task_create
 *        refuses a handle whose storage the macro has not filled in.
 */
typedef struct
{
    uint32_t                id;
    const os_task_storage_t *storage;

} os_task_t;

/******************************************************************************************************/
/**
 * @brief Task creation parameters: what the task does, as opposed to what it is called and where
 *        its stack lives, which the handle already carries. Built with OS_TASK_CONFIG.
 */
typedef struct
{
    os_task_entry_t entry;
    void            *context;
    uint32_t        priority;
    uint32_t        core_affinity; /**< Bitmask of cores the task may run on;
                                        OS_TASK_CORE_ANY (0) = any core.
                                        Ignored on single-core builds. */

} os_task_config_t;


/*
 * ***********************************************************************************************************
 * Timeouts, task priorities and core affinity
 * ***********************************************************************************************************
*/

/** Timeout value: wait forever (never time out). */
#define OS_WAIT_FOREVER         0xFFFFFFFFU

/** Timeout value: do not wait, fail immediately when unavailable. */
#define OS_WAIT_NOTHING         0U

/** Every task priority level, one name per level, value N for level N.
 *
 *  Applications may use OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_30_HIGHEST. The two outside that range
 *  are kernel-owned and rejected with OS_ERR_INVALID_ARG: OS_TASK_PRIO_IDLE (0) belongs to the idle
 *  task alone, OS_TASK_PRIO_MAX (31) to the kernel's service tasks.
 *
 *  An enum constant is not a macro, so #if reads it as 0. A configured priority written as a name
 *  has to be checked with _Static_assert instead. */
typedef enum
{
    /* Kernel-owned, below every user task. */
    OS_TASK_PRIO_IDLE       = 0U,

    OS_TASK_PRIO_1_LOWEST   = 1U,
    OS_TASK_PRIO_1          = 1U,
    OS_TASK_PRIO_2          = 2U,
    OS_TASK_PRIO_3          = 3U,
    OS_TASK_PRIO_4          = 4U,
    OS_TASK_PRIO_5          = 5U,
    OS_TASK_PRIO_6          = 6U,
    OS_TASK_PRIO_7          = 7U,
    OS_TASK_PRIO_8          = 8U,
    OS_TASK_PRIO_9          = 9U,
    OS_TASK_PRIO_10         = 10U,
    OS_TASK_PRIO_11         = 11U,
    OS_TASK_PRIO_12         = 12U,
    OS_TASK_PRIO_13         = 13U,
    OS_TASK_PRIO_14         = 14U,
    OS_TASK_PRIO_15         = 15U,
    OS_TASK_PRIO_16         = 16U,
    OS_TASK_PRIO_17         = 17U,
    OS_TASK_PRIO_18         = 18U,
    OS_TASK_PRIO_19         = 19U,
    OS_TASK_PRIO_20         = 20U,
    OS_TASK_PRIO_21         = 21U,
    OS_TASK_PRIO_22         = 22U,
    OS_TASK_PRIO_23         = 23U,
    OS_TASK_PRIO_24         = 24U,
    OS_TASK_PRIO_25         = 25U,
    OS_TASK_PRIO_26         = 26U,
    OS_TASK_PRIO_27         = 27U,
    OS_TASK_PRIO_28         = 28U,
    OS_TASK_PRIO_29         = 29U,
    OS_TASK_PRIO_30         = 30U,
    OS_TASK_PRIO_30_HIGHEST = 30U,  /**< Highest a user task may request. */

    /* Kernel-owned, above every user task: os_task_create rejects it, and it is what
     * OS_CONFIG_TIMER_PRIORITY defaults to. */
    OS_TASK_PRIO_MAX        = 31U

} os_task_priority_t;

/* The user range must sit exactly between the two kernel-owned levels, with no gap on either side.
 * A gap would mean a level no task could ever occupy - wasted ready-list and bitmap space - and an
 * overlap would put a user task where the idle fallback or a service task belongs. Checked rather
 * than derived so that renumbering one end without the other fails to build. */
OS_STATIC_ASSERT(((uint32_t)OS_TASK_PRIO_1_LOWEST == ((uint32_t)OS_TASK_PRIO_IDLE + 1U)) &&
               ((uint32_t)OS_TASK_PRIO_30_HIGHEST == ((uint32_t)OS_TASK_PRIO_MAX - 1U)),
               "the user priority range must be contiguous with OS_TASK_PRIO_IDLE and OS_TASK_PRIO_MAX");

/* The ready bitmap is one 32-bit word, one bit per level, so the top level has to fit in it. */
OS_STATIC_ASSERT((uint32_t)OS_TASK_PRIO_MAX < 32U,
               "OS_TASK_PRIO_MAX must fit the 32-bit ready bitmap");

/** Core affinity: the task may run on any core - the empty mask, so no core is
 *  named and none is excluded. What the kernel's own idle, timer and log tasks
 *  use, and what a single-core OS_TASK_CONFIG fills in for you. */
#define OS_TASK_CORE_ANY        0U

/** Core affinity: the task may run only on core n. Combine with | for a set
 *  of allowed cores. Cores are numbered from 0, so a dual-core part is
 *  OS_TASK_CORE(0) and OS_TASK_CORE(1). */
#define OS_TASK_CORE(n)         (1UL << (n))

#ifdef __cplusplus
}
#endif

#endif /* OS_TYPES_H */
