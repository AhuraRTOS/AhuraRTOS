/**
 * @file os_main_work.c
 * @brief Ahura kernel example: deferrable work items (os_work_*).
 *
 * Submits functions to run later on the kernel work task (tsk_work): one after
 * a delay, then two at once to show that each submission is its own call. There
 * is no work object to declare - os_work_submit takes the handler, its context
 * and a delay, and the kernel keeps the submission until it runs.
 *
 * Copy this file into the application source tree as os_main.c to run it; needs
 * OS_CONFIG_WORK_ENABLE=1 in os_config.h (the default).
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

#if !(OS_CONFIG_WORK_ENABLE == 1U)
#error "os_main_work.c needs OS_CONFIG_WORK_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

static __IO uint32_t os_main_work_run_count = 0U;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void work_handler(void *data, size_t len)
{
    const uint32_t label = (len == sizeof(uint32_t)) ? *(const uint32_t *)data : 0U;

    os_main_work_run_count++;
    printf("[work] handler ran with label %lu (count=%lu)\r\n",
           (unsigned long)label, (unsigned long)os_main_work_run_count);
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: submits deferred work, immediate and delayed.
 *
 * @return None.
 */
void os_main(void)
{
    while (1)
    {
        /* Both the handler and the payload are copied into a kernel slot, so these locals may go
         * out of scope the moment os_work_submit returns - there is nothing to keep alive. */
        uint32_t label_a = 1U;
        uint32_t label_b = 2U;

        printf("[work] submitting one to run in 200 ms\r\n");
        (void)os_work_submit(work_handler, &label_a, sizeof(label_a), 200U);
        os_delay_ms(500U);

        /* Each submission is its own call, so the same handler queued twice runs twice - there is
         * no work item for the second submission to reschedule. */
        printf("[work] submitting two at once, one now and one in 100 ms\r\n");
        (void)os_work_submit(work_handler, &label_a, sizeof(label_a), 0U);
        (void)os_work_submit(work_handler, &label_b, sizeof(label_b), 100U);

        /* And a payload-free submission: NULL and 0 when the call needs no data. */
        (void)os_work_submit(work_handler, NULL, 0U, 300U);
        os_delay_ms(2000U);
    }
}
