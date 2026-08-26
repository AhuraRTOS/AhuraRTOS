/**
 * @file os_main_task.c
 * @brief Ahura kernel example: task lifecycle (create / start / pause / resume / delete).
 *
 * Creates one worker task and walks its handle through every state
 * os_task_state_get() can report: SUSPENDED right after os_task_create(),
 * READY/RUNNING after os_task_start(), back to SUSPENDED after os_task_pause(),
 * and finally INACTIVE after os_task_delete(). Copy this file into the
 * application source tree as os_main.c to run it - no extra os_config.h
 * switch needed, tasks are always available.
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

/* The same definition, with attributes on the stack - for when WHERE it lives matters as much as
 * how big it is. Attributes are taken as the rest of the line, so several may be given whatever
 * commas they contain.
 *
 * This one asks for .ram2 - a second SRAM bank, the usual reason to place a stack by hand. The
 * section has to exist in YOUR linker script; nothing here can create it, and without it the
 * linker only places it as an orphan wherever it sees fit. Add it next to the other RAM output
 * sections, pointing at whichever region the part actually has:
 *
 *      .ram2 (NOLOAD) :
 *      {
 *        . = ALIGN(8);
 *        *(.ram2)
 *        *(.ram2*)
 *        . = ALIGN(8);
 *      } >RAM2
 *
 * NOLOAD because nothing here is copied from flash or zeroed at startup - which is also what
 * makes such a section useful for state that must survive a reset. Several attributes are fine
 * too, whatever commas they contain:
 *
 *      OS_TASK_DEFINE_ATTR(placed, 512U, __attribute__((aligned(32), section(".ram2"))));
 *
 * Everything else matches OS_TASK_DEFINE, including the 8-byte alignment it already applies: a
 * section attribute carries no alignment of its own, and os_task_create refuses a stack whose
 * address or size is not 8-byte aligned. */
OS_TASK_DEFINE_ATTR(placed, 512U, __attribute__((section(".ram2"))));

static __IO uint32_t os_main_worker_iterations = 0U;
static __IO uint32_t os_main_placed_iterations = 0U;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static const char* task_state_name(os_task_state_t state)
{
    switch (state)
    {
        case OS_TASK_STATE_INACTIVE:  return "INACTIVE";
        case OS_TASK_STATE_READY:     return "READY";
        case OS_TASK_STATE_RUNNING:   return "RUNNING";
        case OS_TASK_STATE_BLOCKED:   return "BLOCKED";
        case OS_TASK_STATE_SUSPENDED: return "SUSPENDED";
        default:                      return "?";
    }
}

/******************************************************************************************************/
/**
 * @brief Worker entry: counts its own iterations and yields, so os_main can watch it progress.
 */
static void worker_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_main_worker_iterations++;
        os_task_yield();
    }
}

/******************************************************************************************************/
/**
 * @brief Entry for the task whose stack carries attributes: nothing about it differs, which is
 *        the point - only where its stack sits changed.
 */
static void placed_entry(void *context)
{
    (void)context;

    while (1)
    {
        os_main_placed_iterations++;
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
 * @brief Default application task body: drives the worker task through its full lifecycle.
 *
 * @return None.
 */
void os_main(void)
{
    (void)os_task_create(&worker, EXAMPLE_TASK(worker_entry, NULL, OS_TASK_PRIO_1));
    printf("[task] created: state=%s\r\n", task_state_name(os_task_state_get(&worker)));

    (void)os_task_start(&worker);
    os_delay_ms(20U);
    printf("[task] started: state=%s, iterations=%lu\r\n", task_state_name(os_task_state_get(&worker)),
           (unsigned long)os_main_worker_iterations);

    (void)os_task_pause(&worker);
    printf("[task] paused:  state=%s\r\n", task_state_name(os_task_state_get(&worker)));
    os_delay_ms(20U);
    printf("[task] iterations frozen while paused: %lu\r\n", (unsigned long)os_main_worker_iterations);

    (void)os_task_start(&worker); /* os_task_start() also resumes a paused task */
    os_delay_ms(20U);
    printf("[task] resumed: state=%s, iterations=%lu\r\n", task_state_name(os_task_state_get(&worker)),
           (unsigned long)os_main_worker_iterations);

    /* Priority is not fixed at creation. The change takes effect immediately - including for a
     * task already queued on a mutex, semaphore, queue or event, which is re-sorted into its new
     * place in that object's waiter list. */
    {
        os_task_priority_t priority = OS_TASK_PRIO_1;

        (void)os_task_priority_get(&worker, &priority);
        printf("[task] priority as created: %u\r\n", (unsigned)priority);

        (void)os_task_priority_set(&worker, OS_TASK_PRIO_3);
        (void)os_task_priority_get(&worker, &priority);
        printf("[task] priority after set:  %u\r\n", (unsigned)priority);

        /* Only user levels are accepted: 0 is the idle task's and OS_TASK_PRIO_MAX belongs to the
         * kernel's own service tasks, so neither is the application's to hand out. */
        printf("[task] out-of-range priority refused: %s\r\n",
               (os_task_priority_set(&worker, (os_task_priority_t)OS_TASK_PRIO_MAX) ==
                OS_ERR_INVALID_ARG) ? "yes" : "no");

        (void)os_task_priority_set(&worker, OS_TASK_PRIO_1);
    }

    (void)os_task_delete(&worker);
    printf("[task] deleted: state=%s\r\n", task_state_name(os_task_state_get(&worker)));

    /* The attributed task runs exactly like any other - os_task_create validated its stack the
     * same way, and nothing below knows the difference. */
    (void)os_task_create(&placed, EXAMPLE_TASK(placed_entry, NULL, OS_TASK_PRIO_1));
    (void)os_task_start(&placed);
    os_delay_ms(20U);
    printf("[task] attributed-stack task: state=%s, iterations=%lu\r\n",
           task_state_name(os_task_state_get(&placed)), (unsigned long)os_main_placed_iterations);
    (void)os_task_delete(&placed);

    while (1)
    {
        os_delay_ms(1000U);
    }
}
