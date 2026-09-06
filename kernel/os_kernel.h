/**
 * @file os_kernel.h
 * @brief Kernel lifecycle, scheduler lock, assertions and TrustZone (os_kernel.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_KERNEL_H
#define OS_KERNEL_H

#include "os_types.h"

/*
 * ***********************************************************************************************************
 * Kernel lifecycle
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize kernel subsystems. Call once before any other kernel API.
 */
void os_init(void);

/******************************************************************************************************/
/**
 * @brief Start the scheduler and switch to task context. Does not return.
 */
void os_start(void);

/******************************************************************************************************/
/**
 * @brief Return true once the scheduler has been started.
 */
bool os_kernel_is_running(void);

/******************************************************************************************************/
/**
 * @brief Default application task body (see OS_CONFIG_MAIN_TASK_* in os_config.h).
 *
 * os_init() creates and starts it, so the application must define it: copy template/os_main.c into
 * the project. The kernel ships no stub, so a missing definition is a link error rather than a task
 * that silently does nothing. Not referenced at all when OS_CONFIG_TEST_ENABLE is 1.
 */
void os_main(void);


/*
 * ***********************************************************************************************************
 * Scheduler lock
 * ***********************************************************************************************************
 *
 * The other preemption barrier, and the cheaper one when what you are guarding against is another
 * TASK. Pick by what shares the data:
 *   task <-> task   os_kernel_lock; interrupt latency is unaffected.
 *   task <-> ISR    os_critical_enter (or an atomic) - a scheduler lock excludes no interrupt.
 *   core <-> core   os_critical_enter, whose outermost level takes the cross-core spinlock.
 *
 * Both nest, and neither may be held across a blocking call.
*/

/******************************************************************************************************/
/**
 * @brief Defer context switches on the calling core, leaving interrupts enabled (nesting counted).
 *        Blocking calls degrade to non-blocking while held; a no-op from an ISR.
 */
void os_kernel_lock(void);

/******************************************************************************************************/
/**
 * @brief Release one level of scheduler lock, taking any switch deferred while it was held.
 */
void os_kernel_unlock(void);

/******************************************************************************************************/
/**
 * @brief Whether the calling core currently has its scheduler locked (ISR-safe).
 */
bool os_kernel_is_locked(void);


/*
 * ***********************************************************************************************************
 * Assertions         - OS_CONFIG_ASSERT_ENABLE
 * ***********************************************************************************************************
*/

/** OS_ASSERT(expr) checks a condition that must hold if the program is correct, and halts where it
 *  does not. Assertions only ADD checks: the kernel returns the same status codes either way. Use
 *  them for programming errors, never for conditions that can legitimately happen at runtime.
 *
 *  The expression is not evaluated when assertions are compiled out, so it must be side-effect
 *  free. */
#if (OS_CONFIG_ASSERT_ENABLE == 1U)

#define OS_ASSERT(expr)                                                       \
    do {                                                                      \
        if (!(expr))                                                          \
        {                                                                     \
            os_assert_failed(__FILE__, (uint32_t)__LINE__);                   \
        }                                                                     \
    } while (0)

/******************************************************************************************************/
/**
 * @brief Report a failed OS_ASSERT and halt. Calls os_assert_failed_cb, then parks the core
 *        with interrupts masked so a debugger stops at the cause. Never returns.
 */
void os_assert_failed(const char *file, uint32_t line);

/******************************************************************************************************/
/**
 * @brief Application hook for a failed assertion: record or print the location before the
 *        kernel halts. The application must define it (see template/os_cb.c) - the kernel
 *        ships no stub, because a silent one would leave an assertion with nothing to report.
 *        Runs with the failure's own context still intact, so keep it short and do not expect
 *        to return from the assertion.
 */
void os_assert_failed_cb(const char *file, uint32_t line);

#else /* OS_CONFIG_ASSERT_ENABLE == 0U */

#define OS_ASSERT(expr)         ((void)0)

#endif /* OS_CONFIG_ASSERT_ENABLE */


/*
 * ***********************************************************************************************************
 * TrustZone          - OS_CONFIG_TRUSTZONE
 * ***********************************************************************************************************
 *
 * Also declared by the arch port (os_arch_port_common.h), which calls them from
 * the context-switch path; repeated here because they are application-provided.
*/

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
/******************************************************************************************************/
/**
 * @brief TrustZone callback: bank the secure-side context of the task being switched out
 *        (task_id 0 = idle task, no secure context). You define it; the kernel ships no default,
 *        so leaving it out is a link error.
 */
void os_arch_tz_context_save_cb(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief TrustZone callback: restore the secure-side context of the task being switched in.
 *        You define it; the kernel ships no default.
 */
void os_arch_tz_context_restore_cb(uint32_t task_id);
#endif /* OS_CONFIG_TRUSTZONE_NON_SECURE */

#endif /* OS_KERNEL_H */
