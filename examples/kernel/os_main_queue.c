/**
 * @file os_main_queue.c
 * @brief Ahura kernel example: message queue (os_queue_*), both storage kinds.
 *
 * os_main is the producer: it sends an incrementing value every 300 ms. A
 * higher-priority consumer task blocks in os_queue_receive() and drains each
 * item as soon as it arrives.
 *
 * Two queues carry the same items, to show the only thing that differs between
 * them, which is where the item buffer comes from:
 *
 *   - os_main_static_queue  OS_QUEUE_DEFINE_STATIC: sized and initialized at compile time,
 *                     usable with nothing to call first.
 *   - os_main_dynamic_queue OS_QUEUE_DEFINE_DYNAMIC + os_queue_init_dynamic(), buffer taken
 *                     from the kernel heap, so the capacity could just as well be a
 *                     run-time value.
 *
 * Every send and receive call is identical for both. Copy this file into the
 * application source tree as os_main.c to run it; needs OS_CONFIG_QUEUE_ENABLE=1
 * in os_config.h (the default), and OS_CONFIG_ALLOC_ENABLE=1 for the dynamic
 * half.
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

#if !(OS_CONFIG_QUEUE_ENABLE == 1U)
#error "os_main_queue.c needs OS_CONFIG_QUEUE_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(consumer, 512U);

#define QUEUE_CAPACITY 4U

/* Declares the queue AND its buffer, and initializes both at compile time - there is nothing to
 * call before the first send. The buffer is os_main_static_queue_BUFFER and should never be named by
 * hand; the item size and capacity come from this declaration, so they cannot disagree with the
 * storage that actually exists. */
OS_QUEUE_DEFINE_STATIC(os_main_static_queue, uint32_t, QUEUE_CAPACITY);

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
/* Declares the queue object only: os_queue_init_dynamic() below allocates the item buffer. Keeping
 * the object out of the allocation makes its lifetime obvious and means a failed init leaves
 * nothing to clean up. */
OS_QUEUE_DEFINE_DYNAMIC(os_main_dynamic_queue);
#endif

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
        uint32_t value;

        /* Blocks until the producer sends. Nothing here is aware of where either queue keeps its
         * items: a queue behaves the same whichever way it got its buffer. */
        if (os_queue_receive(&os_main_static_queue, &value, OS_WAIT_FOREVER) == OS_STATUS_OK)
        {
            printf("[queue] consumer received %lu from the static queue\r\n", (unsigned long)value);
        }

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
        if (os_queue_receive(&os_main_dynamic_queue, &value, OS_WAIT_FOREVER) == OS_STATUS_OK)
        {
            printf("[queue] consumer received %lu from the dynamic queue\r\n", (unsigned long)value);
        }
#endif
    }
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: sends values for a higher-priority consumer to receive.
 *
 * @return None.
 */
void os_main(void)
{
    uint32_t next_value = 0U;

    /* os_main_static_queue needs no setup: OS_QUEUE_DEFINE_STATIC initialized it at compile time, and
     * there is no status to check because nothing can fail. Only the dynamic queue has an init
     * call, and only it can fail. */

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    /* The geometry is passed as ordinary arguments, so it could come from a config value read at
     * boot rather than a compile-time constant. Worth checking the status: unlike the static
     * queue, this one can fail because the kernel heap is exhausted. */
    if (os_queue_init_dynamic(&os_main_dynamic_queue, sizeof(uint32_t), QUEUE_CAPACITY) != OS_STATUS_OK)
    {
        printf("[queue] dynamic queue init failed (kernel heap exhausted?)\r\n");
        return;
    }

    printf("[queue] dynamic queue allocated, %u bytes of kernel heap left\r\n",
           (unsigned)os_mem_free_get());
#endif

    (void)os_task_create(&consumer, OS_TASK_CONFIG(consumer_entry, NULL, OS_TASK_PRIO_2));
    (void)os_task_start(&consumer);

    while (1)
    {
        /* count and free are the two halves of the same picture: what is waiting to be received,
         * and how much room is left. os_queue_free_get is the one back-pressure asks for - a
         * producer can slow down before a send has to block or report OS_STATUS_FULL. Both are
         * snapshots, so treat a nonzero free count as "worth trying", not as a guarantee. */
        printf("[queue] producer sending %lu (count=%lu, free=%lu before send)\r\n",
               (unsigned long)next_value,
               (unsigned long)os_queue_count_get(&os_main_static_queue),
               (unsigned long)os_queue_free_get(&os_main_static_queue));
        (void)os_queue_send(&os_main_static_queue, &next_value, OS_WAIT_FOREVER);

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
        (void)os_queue_send(&os_main_dynamic_queue, &next_value, OS_WAIT_FOREVER);
#endif

        next_value++;
        os_delay_ms(300U);
    }

    /* Never reached here, but a queue that outlives its usefulness is torn down with
     * os_queue_cleanup(&os_main_dynamic_queue), which returns the buffer to the kernel heap. The same
     * call on os_main_static_queue just empties it, freeing nothing and leaving it usable, so teardown
     * code does not care which kind it is holding. It refuses with OS_STATUS_BUSY while any task
     * is still blocked on the queue. */
}
