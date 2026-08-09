/**
 * @file os_main_semaphore.c
 * @brief Ahura kernel example: counting semaphore (os_semaphore_*).
 *
 * os_main is the producer: it gives a few tokens, then sleeps. A
 * higher-priority consumer task blocks in os_semaphore_take() and wakes
 * immediately each time a token becomes available. Copy this file into the
 * application source tree as os_main.c to run it; needs
 * OS_CONFIG_SEMAPHORE_ENABLE=1 in os_config.h (the default).
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

#include "ahura.h"

#include <stdio.h>

#if !(OS_CONFIG_SEMAPHORE_ENABLE == 1U)
#error "os_main_semaphore.c needs OS_CONFIG_SEMAPHORE_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(consumer, 512U);

static os_semaphore_t os_main_sem;

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
        if (os_semaphore_take(&os_main_sem, OS_WAIT_FOREVER) == OS_STATUS_OK)
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
    /* Starts empty (0 tokens), holds at most 4 - os_semaphore_give() beyond
     * that would return OS_STATUS_FULL. */
    (void)os_semaphore_init(&os_main_sem, 0U, 4U);
    (void)os_task_create(&consumer, OS_TASK_CONFIG(consumer_entry, NULL, OS_TASK_PRIO_2));
    (void)os_task_start(&consumer);

    while (1)
    {
        uint32_t i;

        for (i = 0U; i < 3U; i++)
        {
            printf("[semaphore] producer giving a token\r\n");
            (void)os_semaphore_give(&os_main_sem);
            os_delay_ms(200U);
        }
        os_delay_ms(1000U);
    }
}
