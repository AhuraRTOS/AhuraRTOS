/**
 * @file os_main_notify.c
 * @brief Ahura kernel example: task notifications (os_notify_*).
 *
 * os_main is the giver: it delivers an incrementing value to a higher-priority
 * receiver task every 500 ms. The receiver blocks in os_notify_wait() and
 * wakes immediately each time a value is given - no separate semaphore/queue
 * object needed. Copy this file into the application source tree as os_main.c
 * to run it; needs OS_CONFIG_NOTIFY_ENABLE=1 in os_config.h (the default).
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

#if !(OS_CONFIG_NOTIFY_ENABLE == 1U)
#error "os_main_notify.c needs OS_CONFIG_NOTIFY_ENABLE=1 in os_config.h"
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

OS_TASK_DEFINE(receiver, 512U);

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void receiver_entry(void *context)
{
    (void)context;

    while (1)
    {
        uint32_t value;

        if (os_notify_wait(OS_WAIT_FOREVER, &value) == OS_ERR_NONE)
        {
            printf("[notify] receiver got value=%lu\r\n", (unsigned long)value);
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
 * @brief Default application task body: gives an incrementing value to a receiver task.
 *
 * @return None.
 */
void os_main(void)
{
    uint32_t counter = 0U;

    (void)os_task_create(&receiver, EXAMPLE_TASK(receiver_entry, NULL, OS_TASK_PRIO_2));
    (void)os_task_start(&receiver);

    while (1)
    {
        printf("[notify] os_main giving value=%lu\r\n", (unsigned long)counter);
        (void)os_notify_give(&receiver, counter);
        counter++;
        os_delay_ms(500U);
    }
}
