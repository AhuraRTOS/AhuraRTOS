/**
 * @file os_queue.h
 * @brief Fixed-item-size message queue (os_queue.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_QUEUE_H
#define OS_QUEUE_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Queue              - OS_CONFIG_QUEUE_ENABLE
 * ***********************************************************************************************************
 *
 * A queue is an object plus an item buffer. How it is declared decides where that buffer comes
 * from, and that is the only difference between the three kinds:
 *
 *   STATIC    OS_QUEUE_DEFINE(sensor_q, sizeof(sample_t), 8);
 *   ATTR      OS_QUEUE_DEFINE_ATTR(rx_q, sizeof(sample_t), 8, __attribute__((section(".dma"))));
 *   DYNAMIC   os_queue_t log_q;  then os_queue_init_dynamic(&log_q, sizeof(sample_t), capacity);
 *
 * All three take the item size the same way, as a byte count. Only the dynamic kind has an init
 * call; every call after that is the same for all three, teardown included.
*/

#if (OS_CONFIG_QUEUE_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief What a send does about an item when the queue is already full.
 */
typedef enum
{
    OS_QUEUE_MODE_NORMAL    = 0, /**< Full means wait or refuse, exactly as timeout_ms says. */
    OS_QUEUE_MODE_OVERWRITE = 1  /**< Full means drop the oldest item rather than lose this one. */

} os_queue_mode_t;

/******************************************************************************************************/
/**
 * @brief Queue object.
 */
typedef struct
{
    uint8_t         *buffer;
    size_t          item_size;
    size_t          capacity;
    size_t          head;
    size_t          tail;
    size_t          count;
    os_list_t       send_waiters;    /**< Tasks blocked because the queue is full.  */
    os_list_t       receive_waiters; /**< Tasks blocked because the queue is empty. */
    bool            buffer_owned;    /**< Buffer came from os_queue_init_dynamic: os_queue_cleanup frees it. */
    os_queue_mode_t mode;            /**< What a send does when full; OS_QUEUE_MODE_NORMAL is the zero. */

} os_queue_t;

/* --- Compile-time storage: the geometry is read off the array --------------------------------- */

/** Compile-time initializer binding a queue object to an item array, shared by the two macros below
 *  so they cannot drift apart. Everything omitted is zero-initialized by the C rules for static
 *  storage, which is exactly the empty queue an init call would otherwise write.
 *
 *  Neither parameter is named after a struct field: one that was would be substituted inside the
 *  designated initializers, turning ".capacity" into ".8". Capacity is divided back out of the
 *  array, so it cannot disagree with the storage that exists. */
#define OS_QUEUE_INITIALIZER(array, item_bytes)                \
    {                                                          \
        .buffer    = (uint8_t *)(array),                       \
        .item_size = (item_bytes),                             \
        .capacity  = (sizeof(array) / (item_bytes)),           \
    }

/** Define a queue with statically allocated storage, ready to use where it stands. The object is
 *  plain "name"; the array gets "name_queue_buf", which nothing should name by hand.
 *
 *      OS_QUEUE_DEFINE(sensor_q, sizeof(sensor_sample_t), 8);
 *      status = os_queue_send(&sensor_q, &sample, 10U);
 *
 *  The item size is a byte count, as os_queue_init_dynamic takes it; capacity is divided back out
 *  of the array. Sends copy through memcpy, so nothing checks that what goes in is what the queue
 *  was sized for. File scope only, and the name has to be unique across the link. */
#define OS_QUEUE_DEFINE(name, item_bytes, item_count)                                \
    static uint8_t    name##_queue_buf[(item_bytes) * (item_count)] OS_ITEM_ALIGNED; \
    os_queue_t name = OS_QUEUE_INITIALIZER(name##_queue_buf, (item_bytes))

/** OS_QUEUE_DEFINE, plus attributes on the item array: a named linker section, DMA-capable RAM, a
 *  particular alignment. Identical in every other way.
 *
 *      OS_QUEUE_DEFINE_ATTR(rx_q, sizeof(sample_t), 8, __attribute__((section(".dma_buffers"))));
 *
 *  Variadic, so several attributes may be given whatever commas they contain. The section still has
 *  to exist in the linker script. */
#define OS_QUEUE_DEFINE_ATTR(name, item_bytes, item_count, ...)                                  \
    static uint8_t    name##_queue_buf[(item_bytes) * (item_count)] OS_ITEM_ALIGNED __VA_ARGS__; \
    os_queue_t name = OS_QUEUE_INITIALIZER(name##_queue_buf, (item_bytes))

/** Name a queue defined in another file. Only the queue crosses; its item array stays private to
 *  the file that defined it, and the name has to match the DEFINE exactly. */
#define OS_QUEUE_DECLARE(name)          extern os_queue_t name

/* --- Dynamic storage: the item buffer comes from the kernel heap ------------------------------ */

/* A dynamic queue needs no DEFINE macro: it is a plain os_queue_t, declared wherever its lifetime
 * wants, and os_queue_init_dynamic() obtains the buffer. That call expects the object zeroed,
 * which static storage gives for free and any other placement gets from a { 0 } initializer. */

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a queue over an item buffer allocated from the kernel heap, for a geometry
 *        not known until run time. Only the buffer is allocated; the queue object itself is the
 *        caller's, and os_queue_cleanup releases what this obtained.
 */
os_err_t os_queue_init_dynamic(os_queue_t *queue, size_t item_size, size_t capacity);
#endif /* OS_CONFIG_ALLOC_ENABLE */

/* --- Operations: identical for every storage kind --------------------------------------------- */

/******************************************************************************************************/
/**
 * @brief Send one item into queue, waiting up to timeout_ms when full.
 *
 * OS_QUEUE_MODE_NORMAL answers OS_ERR_FULL or OS_ERR_TIMEOUT. OS_QUEUE_MODE_OVERWRITE spends the
 * same timeout and then drops the OLDEST item instead of refusing, so the timeout reads as "how
 * long to try not to lose anything" and OS_WAIT_NOTHING never waits and never fails.
 */
os_err_t os_queue_send(os_queue_t *queue, const void *item, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Choose what a send does when the queue is full. Every queue starts in
 *        OS_QUEUE_MODE_NORMAL, which the zero of static storage gives for free.
 *
 * Takes effect from the next send. A sender already blocked on this queue keeps the timeout it
 * started with and is not woken: when that timeout expires it re-reads the mode and acts on
 * whatever it is by then, which is the same answer it would have reached had the mode been set
 * a moment earlier.
 */
os_err_t os_queue_mode_set(os_queue_t *queue, os_queue_mode_t mode);

/******************************************************************************************************/
/**
 * @brief Receive one item from queue, waiting up to timeout_ms when empty.
 */
os_err_t os_queue_receive(os_queue_t *queue, void *item_out, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Get current queue item count.
 */
size_t os_queue_count_get(const os_queue_t *queue);

/******************************************************************************************************/
/**
 * @brief Get the number of item slots the queue can still accept (capacity minus count).
 */
size_t os_queue_free_get(const os_queue_t *queue);

/******************************************************************************************************/
/**
 * @brief Tear down a queue of any kind: empty it, and release the item buffer only when
 *        os_queue_init_dynamic allocated it. A queue that owns no buffer keeps its storage and
 *        stays usable, so a statically defined queue needs no init call after this either.
 *        Refuses with OS_ERR_BUSY while tasks are blocked on the queue.
 */
os_err_t os_queue_cleanup(os_queue_t *queue);

#endif /* OS_CONFIG_QUEUE_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_QUEUE_H */
