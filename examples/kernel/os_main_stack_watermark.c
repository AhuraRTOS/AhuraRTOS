/**
 * @file os_main_stack_watermark.c
 * @brief Ahura kernel example: task stack watermark (os_task_stack_watermark_get).
 *
 * A worker task burns some stack depth once, then idles; os_main polls the
 * worst-case stack headroom for both the worker and itself. The watermark
 * only ever shrinks (it records the deepest usage ever seen), so it is safe
 * to poll from a lower-frequency monitoring loop like this one. Copy this
 * file into the application source tree as os_main.c to run it; needs
 * OS_CONFIG_STACK_WATERMARK_ENABLE=1 in os_config.h (the default).
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

#if !(OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
#error "os_main_stack_watermark.c needs OS_CONFIG_STACK_WATERMARK_ENABLE=1 in os_config.h"
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

OS_TASK_DEFINE(worker, 512U);

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void worker_entry(void *context)
{
    __IO uint8_t local_buffer[128]; /* burn some stack depth so the watermark has something to report */
    size_t            i;

    (void)context;

    for (i = 0U; i < sizeof(local_buffer); i++)
    {
        local_buffer[i] = (uint8_t)i;
    }

    while (1)
    {
        os_delay_ms(500U);
    }
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: reports worst-case stack headroom for two tasks.
 *
 * @return None.
 */
void os_main(void)
{
    (void)os_task_create(&worker, EXAMPLE_TASK(worker_entry, NULL, OS_TASK_PRIO_1));
    (void)os_task_start(&worker);
    os_delay_ms(50U); /* let the worker reach its own idle loop at least once */

    while (1)
    {
        size_t min_free;

        (void)os_task_stack_watermark_get(&worker, &min_free);
        printf("[stack_watermark] worker task:  %lu bytes free at minimum\r\n", (unsigned long)min_free);

        (void)os_task_stack_watermark_get(NULL, &min_free); /* NULL = the calling task, i.e. os_main */
        printf("[stack_watermark] os_main task: %lu bytes free at minimum\r\n", (unsigned long)min_free);

        os_delay_ms(1000U);
    }
}
