/**
 * @file os_main_event.c
 * @brief Ahura kernel example: event (os_event_*).
 *
 * Two worker tasks each set a different bit on their own schedule; os_main
 * waits for both bits to be set at once (wait_all=true), consuming them
 * atomically on match (clear_on_exit=true) so it never double-counts a set
 * that happened between wait calls. Copy this file into the application
 * source tree as os_main.c to run it; needs OS_CONFIG_EVENT_ENABLE=1 in
 * os_config.h (the default).
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

#if !(OS_CONFIG_EVENT_ENABLE == 1U)
#error "os_main_event.c needs OS_CONFIG_EVENT_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

#define BIT_A (1UL << 0)
#define BIT_B (1UL << 1)

/* Every task states its core affinity on a multi-core build: the kernel asks for that argument
 * rather than defaulting it, so the decision is made on purpose at each creation site. These
 * examples have no placement preference, so they take any core. */
#if (OS_CONFIG_CORE_COUNT == 1U)
#define EXAMPLE_TASK(entry, context, priority)  OS_TASK_CONFIG((entry), (context), (priority))
#else
#define EXAMPLE_TASK(entry, context, priority)  \
    OS_TASK_CONFIG((entry), (context), (priority), OS_TASK_CORE_ANY)
#endif

OS_TASK_DEFINE(task_a, 512U);
OS_TASK_DEFINE(task_b, 512U);

static os_event_t os_main_event;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void task_a_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_delay_ms(300U);
        printf("[event] task_a setting BIT_A\r\n");
        (void)os_event_set_bits(&os_main_event, BIT_A);
    }
}

/******************************************************************************************************/
static void task_b_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_delay_ms(500U);
        printf("[event] task_b setting BIT_B\r\n");
        (void)os_event_set_bits(&os_main_event, BIT_B);
    }
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: waits for two independent event bits to both be set.
 *
 * @return None.
 */
void os_main(void)
{
    (void)os_event_init(&os_main_event);
    (void)os_task_create(&task_a, EXAMPLE_TASK(task_a_entry, NULL, OS_TASK_PRIO_1));
    (void)os_task_create(&task_b, EXAMPLE_TASK(task_b_entry, NULL, OS_TASK_PRIO_1));
    (void)os_task_start(&task_a);
    (void)os_task_start(&task_b);

    while (1)
    {
        uint32_t matched = 0U;

        (void)os_event_wait_bits(&os_main_event, BIT_A | BIT_B, true, true, &matched, OS_WAIT_FOREVER);
        printf("[event] both BIT_A and BIT_B observed (matched=%lu)\r\n", (unsigned long)matched);
    }
}
