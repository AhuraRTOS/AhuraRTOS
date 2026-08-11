/**
 * @file os_main_critical.c
 * @brief Ahura kernel example: critical sections (os_critical_enter / os_critical_exit).
 *
 * os_main and a worker task both hammer the same non-atomic counter,
 * incrementing it from inside a critical section every time. Since the
 * section excludes both tasks and interrupts, the final total is always
 * exactly the sum of both loop counts - proving no update was lost to a
 * torn read-modify-write. Copy this file into the application source tree
 * as os_main.c to run it - no extra os_config.h switch needed, critical
 * sections are always available.
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

OS_TASK_DEFINE(worker, 512U);

#define ITERATIONS 100000UL

static __IO uint32_t os_main_shared_counter = 0U;
static __IO bool     os_main_worker_done    = false;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void worker_entry(void *context)
{
    uint32_t i;

    (void)context;

    for (i = 0U; i < ITERATIONS; i++)
    {
        os_critical_enter();
        os_main_shared_counter++;
        os_critical_exit();
    }

    os_main_worker_done = true;

    while (1)
    {
        os_task_yield();
    }
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: races a worker task to increment a shared counter.
 *
 * @return None.
 */
void os_main(void)
{
    uint32_t i;

    (void)os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_1));
    (void)os_task_start(&worker);

    for (i = 0U; i < ITERATIONS; i++)
    {
        os_critical_enter();
        os_main_shared_counter++;
        os_critical_exit();
    }

    while (!os_main_worker_done)
    {
        os_delay_ms(10U);
    }

    printf("[critical] expected=%lu actual=%lu (%s)\r\n", (unsigned long)(2UL * ITERATIONS),
           (unsigned long)os_main_shared_counter, (os_main_shared_counter == (2UL * ITERATIONS)) ? "OK" : "CORRUPTED");

    while (1)
    {
        os_delay_ms(1000U);
    }
}
