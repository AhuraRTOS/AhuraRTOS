/**
 * @file os_main_kernel_lock.c
 * @brief Ahura kernel example: the scheduler lock (os_kernel_lock / os_kernel_unlock).
 *
 * A higher-priority worker is started while the scheduler is locked, and does not run
 * until the unlock - even though interrupts stay live throughout, which the tick count
 * rising across the locked region proves. The contrast with os_critical_enter is the
 * point: a critical section stops interrupts and so stops everything; this stops only
 * other tasks. Copy this file into the application source tree as os_main.c to run it -
 * no extra os_config.h switch needed, the scheduler lock is always available.
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

#include "ahura.h"

#include <stdio.h>

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

/* Every task states its core affinity on a multi-core build: the kernel asks for that argument
 * rather than defaulting it, so the decision is made on purpose at each creation site. These
 * examples have no placement preference, so they take any core. */
#if (OS_CONFIG_CORE_COUNT == 1U)
#define EXAMPLE_TASK(entry, context, priority)  OS_TASK_CONFIG((entry), (context), (priority))
#else
#define EXAMPLE_TASK(entry, context, priority)  \
    OS_TASK_CONFIG((entry), (context), (priority), OS_TASK_CORE_ANY)
#endif

OS_TASK_DEFINE(worker, 512U);

static __IO uint32_t os_main_worker_runs = 0U;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Worker entry: runs at a HIGHER priority than os_main, so the only thing that can keep
 *        it off the CPU is the scheduler lock.
 */
static void worker_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_main_worker_runs++;
        os_delay_ms(10U);
    }
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Example entry point.
 *
 * @return None.
 */
void os_main(void)
{
    uint32_t tick_before;
    uint32_t tick_after;
    uint32_t runs_during_lock;

    (void)os_task_create(&worker, EXAMPLE_TASK(worker_entry, NULL, OS_TASK_PRIO_3));

    printf("[lock] locked before start: %s\r\n", os_kernel_is_locked() ? "yes" : "no");

    os_kernel_lock();

    /* Starting a task that outranks this one would normally switch away on the spot. It does not:
     * the worker becomes READY and waits for the unlock. */
    (void)os_task_start(&worker);

    tick_before = os_tick_get();

    /* os_delay_ms cannot block here - blocking means switching away, which the lock forbids - so
     * it busy-waits instead. The delay still elapses in real time; it just costs the CPU. That is
     * exactly why a locked region should be short. */
    os_delay_ms(50U);

    tick_after       = os_tick_get();
    runs_during_lock = os_main_worker_runs;

    os_kernel_unlock();   /* the switch deferred above is taken right here */

    printf("[lock] worker ran %lu times while locked (expected 0)\r\n",
           (unsigned long)runs_during_lock);
    printf("[lock] ticks advanced %lu during the lock - interrupts never stopped\r\n",
           (unsigned long)(tick_after - tick_before));

    os_delay_ms(50U);
    printf("[lock] worker ran %lu times once unlocked\r\n", (unsigned long)os_main_worker_runs);

    /* Nesting is counted: only the outermost unlock reopens the scheduler. */
    os_kernel_lock();
    os_kernel_lock();
    os_kernel_unlock();
    printf("[lock] still locked after one of two unlocks: %s\r\n",
           os_kernel_is_locked() ? "yes" : "no");
    os_kernel_unlock();
    printf("[lock] released after the outermost unlock: %s\r\n",
           os_kernel_is_locked() ? "no" : "yes");

    /* Which barrier to reach for:
     *
     *   os_critical_enter()  data an ISR (or another core) also touches. Masks interrupts, so it
     *                        costs interrupt latency for as long as it is held.
     *   os_kernel_lock()     data only other TASKS touch. Interrupts and the tick keep running;
     *                        no other task gets the CPU until the outermost unlock.
     *
     * The lock excludes no ISR and no other core, so it never replaces a critical section - it
     * only avoids paying for one where no interrupt is involved. */

    while (1)
    {
        os_delay_ms(1000U);
    }
}
