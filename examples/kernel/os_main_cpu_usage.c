/**
 * @file os_main_cpu_usage.c
 * @brief Ahura kernel example: CPU load sampling (os_cpu_usage_get).
 *
 * A worker task alternates between blocking (lets the idle task run) and
 * busy-spinning (never blocks or yields) while os_main samples CPU usage
 * over a fixed window around each phase - the two numbers should contrast
 * sharply. Copy this file into the application source tree as os_main.c to
 * run it; needs OS_CONFIG_CPU_USAGE_ENABLE=1 in os_config.h (the default).
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

#if !(OS_CONFIG_CPU_USAGE_ENABLE == 1U)
#error "os_main_cpu_usage.c needs OS_CONFIG_CPU_USAGE_ENABLE=1 in os_config.h"
#endif

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

OS_TASK_DEFINE(spinner, 512U);

static __IO bool os_main_spin = false;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void spinner_entry(void *context)
{
    (void)context;

    while (1)
    {
        if (os_main_spin)
        {
            /* Tight loop: never blocks or yields, so it consumes every tick
             * the scheduler gives it while os_main is asleep below. */
        }
        else
        {
            os_delay_ms(20U);
        }
    }
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: contrasts idle vs busy CPU usage.
 *
 * @return None.
 */
void os_main(void)
{
    (void)os_task_create(&spinner, EXAMPLE_TASK(spinner_entry, NULL, OS_TASK_PRIO_1));
    (void)os_task_start(&spinner);

    while (1)
    {
        (void)os_cpu_usage_get(); /* reset the sampling window */
        os_main_spin = false;
        os_delay_ms(500U);
        printf("[cpu_usage] idle:  %lu%%\r\n", (unsigned long)os_cpu_usage_get());

        (void)os_cpu_usage_get(); /* reset the sampling window */
        os_main_spin = true;
        os_delay_ms(500U);
        printf("[cpu_usage] busy:  %lu%%\r\n", (unsigned long)os_cpu_usage_get());
    }
}
