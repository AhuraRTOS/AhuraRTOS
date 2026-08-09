/**
 * @file os_main_atomic.c
 * @brief Ahura kernel example: atomic operations (os_atomic_*).
 *
 * An atomic operation is one no other task, ISR or core can observe half-finished. The classic
 * need is a counter shared by two writers: "count = count + 1" is a load, an add and a store, and
 * anything that preempts between the load and the store makes both writers compute from the same
 * starting value, so one of the two increments simply disappears.
 *
 * This example runs that race deliberately. Two tasks of equal priority round-robin on every tick
 * while both increment the same two counters, one through os_atomic_inc() and one with an ordinary
 * read-modify-write. The atomic total is always exact; the plain one usually is not.
 *
 * On cores with LDREX and STREX (ARMv7-M, ARMv8-M) atomics are lock-free and never mask
 * interrupts. ARMv6-M has no such
 * instructions, so there the operation briefly disables interrupts instead, which is worth
 * knowing before putting one in an ARMv6-M hot path.
 *
 * Copy this file into the application source tree as os_main.c to run it.
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

#if !(OS_CONFIG_ATOMIC_ENABLE == 1U)
#error "os_main_atomic.c needs OS_CONFIG_ATOMIC_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define INCREMENTS_PER_TASK 20000UL
#define WRITER_COUNT        2UL

/* Bit indices for the flag word below, to show os_atomic_*_bit on a shared set of flags. */
#define FLAG_FIRST_WRITER_DONE  0U
#define FLAG_SECOND_WRITER_DONE 1U

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(writer_a, 512U);
OS_TASK_DEFINE(writer_b, 512U);

/* Declare shared counters as os_atomic_t and reach them only through os_atomic_*. Declaring one
 * as a plain (or even volatile) int and casting at the call site is how a counter that looks
 * atomic stops being one. */
static os_atomic_t os_main_atomic_counter = OS_ATOMIC_INIT(0);
static os_atomic_t os_main_flags          = OS_ATOMIC_INIT(0);
static os_atomic_t os_main_writers_done   = OS_ATOMIC_INIT(0);

/* The control case, deliberately not atomic. */
static __IO int32_t os_main_plain_counter = 0;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void writer_entry(void *context)
{
    uint32_t bit = (uint32_t)(uintptr_t)context;
    uint32_t i;

    for (i = 0U; i < INCREMENTS_PER_TASK; i++)
    {
        /* Indivisible: no other writer can slip between the read and the write. */
        (void)os_atomic_inc(&os_main_atomic_counter);

        /* Three separate steps, and the scheduler is free to preempt between them. */
        os_main_plain_counter = os_main_plain_counter + 1;
    }

    /* Set this writer's completion bit without disturbing the other one's, which a
     * read-modify-write on a shared flag word could not promise. */
    os_atomic_set_bit(&os_main_flags, bit);

    (void)os_atomic_inc(&os_main_writers_done);
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: races two writers and compares atomic against plain.
 *
 * @return None.
 */
void os_main(void)
{
    const int32_t expected = (int32_t)(INCREMENTS_PER_TASK * WRITER_COUNT);

    /* --- Single-threaded behaviour first, to show the return-value convention --- */

    {
        os_atomic_t value = OS_ATOMIC_INIT(0);

        /* Every read-modify-write returns the value from BEFORE the operation, not after it.
         * A return of 0 here means the word now reads 1. */
        printf("[atomic] inc() returned %ld, the word now reads %ld\r\n",
               (long)os_atomic_inc(&value), (long)os_atomic_get(&value));

        (void)os_atomic_set(&value, 100);
        printf("[atomic] cas(100 -> 200) %s, cas(100 -> 300) %s\r\n",
               os_atomic_cas(&value, 100, 200) ? "took" : "refused",
               os_atomic_cas(&value, 100, 300) ? "took" : "refused");
    }

    /* --- Now the race --- */

    printf("[atomic] two tasks, %lu increments each, %ld expected in total\r\n",
           (unsigned long)INCREMENTS_PER_TASK, (long)expected);

    (void)os_task_create(&writer_a, OS_TASK_CONFIG(writer_entry,
                                                  (void *)(uintptr_t)FLAG_FIRST_WRITER_DONE,
                                                  OS_TASK_PRIO_3));
    (void)os_task_create(&writer_b, OS_TASK_CONFIG(writer_entry,
                                                  (void *)(uintptr_t)FLAG_SECOND_WRITER_DONE,
                                                  OS_TASK_PRIO_3));
    (void)os_task_start(&writer_a);
    (void)os_task_start(&writer_b);

    while (os_atomic_get(&os_main_writers_done) < (int32_t)WRITER_COUNT)
    {
        os_delay_ms(10U);
    }

    printf("[atomic] os_atomic_inc()  reached %ld of %ld\r\n",
           (long)os_atomic_get(&os_main_atomic_counter), (long)expected);
    printf("[atomic] plain ++         reached %ld of %ld (%ld lost)\r\n",
           (long)os_main_plain_counter, (long)expected, (long)(expected - os_main_plain_counter));

    printf("[atomic] completion flags: writer A %s, writer B %s\r\n",
           os_atomic_test_bit(&os_main_flags, FLAG_FIRST_WRITER_DONE)  ? "done" : "pending",
           os_atomic_test_bit(&os_main_flags, FLAG_SECOND_WRITER_DONE) ? "done" : "pending");

    while (1)
    {
        os_delay_ms(1000U);
    }
}
