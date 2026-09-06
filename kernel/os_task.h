/**
 * @file os_task.h
 * @brief Tasks: declaration, lifecycle, priority, stack reporting, affinity (os_task.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_TASK_H
#define OS_TASK_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Task declaration macros
 * ***********************************************************************************************************
*/

/* Task stack alignment, taken from the port rather than fixed here: ARM AAPCS wants 8 and the
 * RISC-V ilp32 ABI wants 16, and os_task_create validates against OS_ARCH_STACK_ALIGNMENT_BYTES,
 * so a hardcoded 8 would produce a task the kernel then refuses to create.
 *
 * Order matters below: armclang also defines __clang__, and clang also defines __GNUC__. */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)
#define OS_STACK_ALIGNED        __attribute__((aligned(OS_ARCH_STACK_ALIGNMENT_BYTES)))  /* armclang */
#elif defined(__clang__)
#define OS_STACK_ALIGNED        __attribute__((aligned(OS_ARCH_STACK_ALIGNMENT_BYTES)))  /* clang    */
#elif defined(__GNUC__)
#define OS_STACK_ALIGNED        __attribute__((aligned(OS_ARCH_STACK_ALIGNMENT_BYTES)))  /* GCC      */
#else
#define OS_STACK_ALIGNED
#endif

/* Alignment for a byte array holding objects of a type the macro was never told about.
 * OS_QUEUE_DEFINE takes a size rather than a type, so nothing can be inferred from it. Eight covers
 * every fundamental type on these 32-bit targets, and matches what os_mem_alloc returns. */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)
#define OS_ITEM_ALIGNED         __attribute__((aligned(8)))  /* armclang */
#elif defined(__clang__)
#define OS_ITEM_ALIGNED         __attribute__((aligned(8)))  /* clang    */
#elif defined(__GNUC__)
#define OS_ITEM_ALIGNED         __attribute__((aligned(8)))  /* GCC      */
#else
#define OS_ITEM_ALIGNED
#endif

/** The .name line the two DEFINE macros below emit, or nothing at all.
 *
 *  A whole initializer rather than just its value, because OS_CONFIG_TASK_NAME_ENABLE at 0 takes
 *  the field out of os_task_storage_t as well - so there is no field left to give a value to. What
 *  that removes is the name AND the pointer to it: a string literal nothing references is never
 *  emitted, and the descriptor loses a word per task. */
#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
#define OS_TASK_NAME_INIT(task_name)        .name = #task_name,
#else
#define OS_TASK_NAME_INIT(task_name)
#endif

/** Define a task: its handle, its stack, and the storage descriptor tying the two together.
 *
 *  The handle is plain "task_name"; the stack gets "task_name_stack_buf", which nothing should name
 *  by hand. stack_size is in bytes, rounded up to a multiple of 8, at least OS_CONFIG_MIN_STACK_SIZE.
 *
 *      OS_TASK_DEFINE(worker, 512U);
 *      status = os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_1));
 *
 *  Parameters are task_name and stack_size, not name and stack_bytes: one named after a struct
 *  field would be substituted inside the initializers below. */
#define OS_TASK_DEFINE(task_name, stack_size)                   \
    static uint8_t task_name##_stack_buf[(((stack_size) + (OS_ARCH_STACK_ALIGNMENT_BYTES - 1U)) & ~(OS_ARCH_STACK_ALIGNMENT_BYTES - 1U))] OS_STACK_ALIGNED;  \
    static const os_task_storage_t task_name##_task_storage = { \
        OS_TASK_NAME_INIT(task_name)                            \
        .stack_memory = (void *)(task_name##_stack_buf),        \
        .stack_bytes  = sizeof(task_name##_stack_buf)           \
    };                                                          \
    os_task_t task_name = { .storage = &task_name##_task_storage }

/** OS_TASK_DEFINE, plus attributes on the stack: a named linker section, fast on-chip RAM, a
 *  no-init region that survives a reset. Identical in every other way, and OS_STACK_ALIGNED is
 *  already applied, so a section attribute cannot cost the stack its alignment.
 *
 *      OS_TASK_DEFINE_ATTR(rx_task, 1024U, __attribute__((section(".dtcm"))));
 *
 *  Variadic, so several attributes may be given whatever commas they contain. The section still
 *  has to exist in the linker script. */
#define OS_TASK_DEFINE_ATTR(task_name, stack_size, ...)             \
    static uint8_t task_name##_stack_buf[(((stack_size) + (OS_ARCH_STACK_ALIGNMENT_BYTES - 1U)) & ~(OS_ARCH_STACK_ALIGNMENT_BYTES - 1U))] \
        OS_STACK_ALIGNED __VA_ARGS__;                               \
    static const os_task_storage_t task_name##_task_storage = {     \
        OS_TASK_NAME_INIT(task_name)                                \
        .stack_memory = (void *)(task_name##_stack_buf),            \
        .stack_bytes  = sizeof(task_name##_stack_buf)               \
    };                                                              \
    os_task_t task_name = { .storage = &task_name##_task_storage }

/** Name a task defined in another file. Only the handle crosses; its stack and storage descriptor
 *  stay private to the file that defined them. */
#define OS_TASK_DECLARE(task_name)      extern os_task_t task_name

/** Task behaviour for os_task_create: what the task runs, with what, and at what priority. Its
 *  name and stack came from OS_TASK_DEFINE.
 *
 *    CORE_COUNT == 1   OS_TASK_CONFIG(entry, context, priority)
 *    CORE_COUNT  > 1   OS_TASK_CONFIG(entry, context, priority, core_affinity)
 *
 *  Raising OS_CONFIG_CORE_COUNT breaks every call site until it names an affinity, deliberately:
 *  placement gets decided once, on purpose. core_affinity is a bitmask (OS_TASK_CORE(n),
 *  OR-combinable; OS_TASK_CORE_ANY for any core); bits beyond the core count are INVALID_ARG.
 *
 *  Initialized positionally: a designated initializer would substitute inside ".entry". */
#if (OS_CONFIG_CORE_COUNT == 1U)
#define OS_TASK_CONFIG(entry, context, priority) \
    &(os_task_config_t) { \
        (entry), \
        (context), \
        (priority), \
        OS_TASK_CORE_ANY \
    }
#else
#define OS_TASK_CONFIG(entry, context, priority, core_affinity) \
    &(os_task_config_t) { \
        (entry), \
        (context), \
        (priority), \
        (core_affinity) \
    }
#endif


/*
 * ***********************************************************************************************************
 * Tasks
 * ***********************************************************************************************************
 *
 * A NULL task handle means THIS TASK wherever one is accepted below, the same shorthand FreeRTOS
 * uses. The exceptions are os_task_create, which needs somewhere to write the new handle, and
 * os_task_start, which needs a task that is not already running.
 *
 * os_task_pause and os_task_delete are task-only whatever handle they are given: an interrupt must
 * not tear down the context it is about to return into. NULL is refused with OS_ERR_INVALID_ARG
 * from an ISR and before the first dispatch, since there is no calling task in either case.
*/

/******************************************************************************************************/
/**
 * @brief Create a task; priority must be OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_30_HIGHEST.
 */
os_err_t os_task_create(os_task_t *task, const os_task_config_t *config);

/******************************************************************************************************/
/**
 * @brief Start a created task (make it ready to run).
 */
os_err_t os_task_start(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Pause a task (NULL means current running task). OS_ERR_BUSY for the idle task and for
 *        the kernel's own service tasks (timer, log).
 */
os_err_t os_task_pause(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Delete a task and release its TCB slot (NULL means current running task). OS_ERR_BUSY
 *        for the idle task and for the kernel's own service tasks (timer, log).
 */
os_err_t os_task_delete(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Yield the processor to another ready task of equal or higher priority.
 */
void os_task_yield(void);

/******************************************************************************************************/
/**
 * @brief Change a task's priority (NULL means the calling task); takes effect immediately, including
 *        for a task already queued on a mutex, semaphore, queue or event. Accepts only
 *        OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_30_HIGHEST; OS_ERR_BUSY for the idle task and the
 *        kernel's service tasks. A priority-inheritance boost in force is kept - the new value
 *        becomes the base the task returns to.
 */
os_err_t os_task_priority_set(os_task_t *task, os_task_priority_t priority);

/******************************************************************************************************/
/**
 * @brief Get a task's priority (NULL means the calling task): the priority the application set,
 *        not a priority-inheritance boost that may be in force right now.
 */
os_err_t os_task_priority_get(const os_task_t *task, os_task_priority_t *priority_out);

/******************************************************************************************************/
/**
 * @brief Get the current state of a task (NULL means current running task).
 */
os_task_state_t os_task_state_get(const os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Get a task's name (NULL means the calling task). NULL when the task is unknown, or in
 *        any build with OS_CONFIG_TASK_NAME_ENABLE at 0.
 */
const char* os_task_name_get(const os_task_t *task);


/*
 * ***********************************************************************************************************
 * Stack watermark    - OS_CONFIG_STACK_WATERMARK_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the minimum stack headroom a task has ever had, in bytes (NULL means current task).
 */
os_err_t os_task_stack_watermark_get(const os_task_t *task, size_t *min_free_bytes);
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */


/*
 * ***********************************************************************************************************
 * Stack overflow     - OS_CONFIG_STACK_CHECK_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Reported when a task is found to have overrun its stack, at the moment it is switched out.
 *        REQUIRED when OS_CONFIG_STACK_CHECK_ENABLE is 1; the kernel ships no default. The core
 *        parks immediately afterwards, which makes this the only chance to record which task it was.
 *
 *        Runs inside PendSV with the kernel's interrupts masked, so it must NOT call any kernel API.
 *
 * @param[in] task_name  Name of the offending task, as given to OS_TASK_DEFINE.
 */
void os_stack_overflow_cb(const char *task_name);
#endif /* OS_CONFIG_STACK_CHECK_ENABLE */


/*
 * ***********************************************************************************************************
 * Multi-core         - OS_CONFIG_CORE_COUNT > 1
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)

/******************************************************************************************************/
/**
 * @brief Enter the scheduler on a secondary core. Call after os_start() is running on core 0,
 *        from the secondary core, once the SoC layer has booted it with a vector table routing
 *        PendSV to the kernel handler. Does not return.
 */
void os_core_start(void);

/******************************************************************************************************/
/**
 * @brief Change which cores a task may run on (bitmask, OS_TASK_CORE_ANY = any core).
 */
os_err_t os_task_core_affinity_set(os_task_t *task, uint32_t core_affinity);

/* The two SoC callbacks below are also declared by the arch port
 * (os_arch_port_common.h), which calls them; repeated here because they are
 * application-provided. */

/******************************************************************************************************/
/**
 * @brief Multi-core SoC callback: return the index of the calling core (0-based).
 *        REQUIRED when OS_CONFIG_CORE_COUNT is above 1; the kernel ships no default.
 */
uint32_t os_arch_core_id_get_cb(void);

/******************************************************************************************************/
/**
 * @brief Multi-core SoC callback: interrupt another core so it re-evaluates scheduling.
 *        REQUIRED when OS_CONFIG_CORE_COUNT is above 1; the kernel ships no default.
 */
void os_arch_core_ipi_request_cb(uint32_t core_id);

/******************************************************************************************************/
/**
 * @brief Multi-core SoC callback: boot a secondary core so it reaches os_core_start().
 *        Called by os_start(), once per core above 0. REQUIRED when OS_CONFIG_CORE_COUNT is
 *        above 1; the kernel ships no default.
 */
void os_arch_core_launch_cb(uint32_t core_id);

/******************************************************************************************************/
/**
 * @brief Multi-core SoC callback: print whatever the SoC package knows about its own bring-up.
 *        Called only when something has already gone wrong, so it may take its time.
 *
 * Optional, with a weak empty default. It exists because whether a core was released, which
 * inter-core interrupt was armed and what a fault handler caught are facts only the package holds.
 * Called from task context, late enough that a USB console has been opened and can be read.
 */
void os_arch_soc_diagnose_cb(void);

#endif /* OS_CONFIG_CORE_COUNT > 1U */

/******************************************************************************************************/
/**
 * @brief SoC callback: wait for work on an idle core. Optional; the weak default is a plain WFI.
 *
 * On some parts WFI is the wrong instrument. On the RP2 family it gates the core's clock and stops
 * that core's SysTick with it, so an idle secondary core depends entirely on an inter-core
 * interrupt arriving and one missed signal sleeps it for good. WFE is the answer there because its
 * event register LATCHES, so an SEV arriving first makes the WFE return at once. A package
 * overriding this should pair it with an SEV wherever it signals another core.
 *
 * May return spuriously; the idle loop simply calls it again.
 */
void os_arch_soc_idle_cb(void);

#ifdef __cplusplus
}
#endif

#endif /* OS_TASK_H */
