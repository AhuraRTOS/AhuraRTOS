/**
 * @file os_main_mutex.c
 * @brief Ahura kernel example: mutual exclusion (os_mutex_*).
 *
 * os_main and a worker task both run a read-delay-write sequence on a
 * shared value while holding the same mutex, so the two halves of each
 * update can never interleave - also exercises os_mutex_try_lock() on an
 * uncontended mutex. Copy this file into the application source tree as
 * os_main.c to run it; needs OS_CONFIG_MUTEX_ENABLE=1 in os_config.h
 * (the default).
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

#if !(OS_CONFIG_MUTEX_ENABLE == 1U)
#error "os_main_mutex.c needs OS_CONFIG_MUTEX_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(worker, 512U);

static os_mutex_t        os_main_mutex;
static __IO uint32_t os_main_shared_value = 0U;
static __IO bool     os_main_worker_done  = false;

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

    for (i = 0U; i < 5U; i++)
    {
        if (os_mutex_lock(&os_main_mutex, OS_WAIT_FOREVER) == OS_STATUS_OK)
        {
            uint32_t value = os_main_shared_value;

            printf("[mutex] worker  read=%lu\r\n", (unsigned long)value);
            os_delay_ms(10U); /* widen the window: a broken mutex would let os_main interleave here */
            os_main_shared_value = value + 1U;
            (void)os_mutex_unlock(&os_main_mutex);
        }
        os_delay_ms(5U);
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
 * @brief Default application task body: contends with a worker task for the same mutex.
 *
 * @return None.
 */
void os_main(void)
{
    uint32_t i;

    (void)os_mutex_init(&os_main_mutex);
    (void)os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_1));
    (void)os_task_start(&worker);

    for (i = 0U; i < 5U; i++)
    {
        if (os_mutex_lock(&os_main_mutex, OS_WAIT_FOREVER) == OS_STATUS_OK)
        {
            uint32_t value = os_main_shared_value;

            printf("[mutex] os_main read=%lu\r\n", (unsigned long)value);
            os_delay_ms(10U);
            os_main_shared_value = value + 1U;
            (void)os_mutex_unlock(&os_main_mutex);
        }
        os_delay_ms(5U);
    }

    while (!os_main_worker_done)
    {
        os_delay_ms(10U);
    }

    printf("[mutex] final value=%lu (expect 10 - every read-modify-write stayed atomic)\r\n",
           (unsigned long)os_main_shared_value);

    printf("[mutex] os_mutex_try_lock() on an unheld mutex -> %d (OS_STATUS_OK)\r\n",
           (int)os_mutex_try_lock(&os_main_mutex));
    (void)os_mutex_unlock(&os_main_mutex);

    while (1)
    {
        os_delay_ms(1000U);
    }
}
