/**
 * @file os_main_sem.c
 * @brief Ahura kernel example: counting semaphore (os_sem_*).
 *
 * os_main is the producer: it gives a few tokens, then sleeps. A
 * higher-priority consumer task blocks in os_sem_take() and wakes
 * immediately each time a token becomes available. Copy this file into the
 * application source tree as os_main.c to run it; needs
 * OS_CONFIG_SEM_ENABLE=1 in os_config.h (the default).
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

#if !(OS_CONFIG_SEM_ENABLE == 1U)
#error "os_main_sem.c needs OS_CONFIG_SEM_ENABLE=1 in os_config.h"
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

OS_TASK_DEFINE(consumer, 512U);

static os_sem_t os_main_sem;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void consumer_entry(void *context)
{
    (void)context;

    while (1)
    {
        if (os_sem_take(&os_main_sem, OS_WAIT_FOREVER) == OS_ERR_NONE)
        {
            printf("[semaphore] consumer took a token\r\n");
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
 * @brief Default application task body: gives tokens for a higher-priority consumer to take.
 *
 * @return None.
 */
void os_main(void)
{
    /* Starts empty (0 tokens), holds at most 4 - os_sem_give() beyond
     * that would return OS_ERR_FULL. */
    (void)os_sem_init(&os_main_sem, 0U, 4U);
    (void)os_task_create(&consumer, EXAMPLE_TASK(consumer_entry, NULL, OS_TASK_PRIO_2));
    (void)os_task_start(&consumer);

    while (1)
    {
        uint32_t i;

        for (i = 0U; i < 3U; i++)
        {
            printf("[semaphore] producer giving a token\r\n");
            (void)os_sem_give(&os_main_sem);
            os_delay_ms(200U);
        }
        os_delay_ms(1000U);
    }
}
