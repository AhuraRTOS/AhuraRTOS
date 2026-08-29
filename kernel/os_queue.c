/**
 * @file os_queue.c
 * @brief Queue module implementation with timeouts.
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

#include "os_internal.h"

#include <string.h>

#if (OS_CONFIG_QUEUE_ENABLE == 1U)

#if (OS_CONFIG_ALLOC_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static os_err_t os_queue_bind_buffer(os_queue_t *queue, void *buffer, size_t item_size, size_t capacity);

#endif /* OS_CONFIG_ALLOC_ENABLE */

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Send one item into queue, waiting up to timeout_ms when full.
 *
 * The item copy happens inside a critical section: keep item_size small.
 * Nonzero timeouts are only honored from task context after os_start.
 *
 * In OS_QUEUE_MODE_OVERWRITE the same timeout is spent the same way, but when it runs out the
 * oldest item is dropped instead of the send being refused, so this returns OK on every path a
 * bound queue can reach. That makes timeout_ms read as "how long to try not to lose anything".
 *
 * @param[in,out] queue       Queue object.
 * @param[in]     item        Item data to copy.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_err_t  OK on send, FULL when no space without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_err_t os_queue_send(os_queue_t *queue, const void *item, uint32_t timeout_ms)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((queue != NULL) && (item != NULL))
    {
        uint32_t budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
        uint32_t start_tick      = os_tick_get();
        uint32_t remaining_ticks = budget_ticks;
        bool     waiting         = true;

        /* Retry loop with one exit (MISRA Rule 15.5): each arm records the outcome
         * in status and clears the loop flag rather than returning for itself. */
        while (waiting)
        {
            os_critical_enter();

            if (queue->count < queue->capacity)
            {
                uint8_t *slot = &queue->buffer[queue->tail * queue->item_size];

                (void)memcpy(slot, item, queue->item_size);
                queue->tail = (queue->tail + 1U) % queue->capacity;
                queue->count++;

                /* An item arrived: release the highest-priority receiver. */
                (void)os_task_waiters_wake_one(&queue->receive_waiters);

                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_NONE;
                waiting = false;
            }
            else if ((queue->mode == OS_QUEUE_MODE_OVERWRITE) && (queue->capacity != 0U) &&
                     ((timeout_ms == OS_WAIT_NOTHING) || (!os_internal_can_block()) ||
                      (remaining_ticks == 0U)))
            {
                /* Full, and out of patience: the oldest item makes room for this one.
                 *
                 * Full means head == tail, so the slot tail names IS the oldest item, and
                 * writing there overwrites exactly it. Both cursors then move together and
                 * count stays at capacity, because nothing left the queue - one item simply
                 * took another's place.
                 *
                 * The capacity test is not defensive about the arithmetic above it: an unbound
                 * queue has capacity 0, counts as full, and would divide by zero here. It falls
                 * through to OS_ERR_FULL instead, exactly as it does in the default mode. */
                uint8_t *slot = &queue->buffer[queue->tail * queue->item_size];

                queue->head = (queue->head + 1U) % queue->capacity;

                (void)memcpy(slot, item, queue->item_size);
                queue->tail = (queue->tail + 1U) % queue->capacity;

                /* Same wake as an ordinary insert. A full queue can hold no blocked receiver, so
                 * this normally finds an empty list; it is here so the two insert paths cannot
                 * drift apart. */
                (void)os_task_waiters_wake_one(&queue->receive_waiters);

                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_NONE;
                waiting = false;
            }
            else if ((timeout_ms == OS_WAIT_NOTHING) || (!os_internal_can_block()))
            {
                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_FULL;
                waiting = false;
            }
            else if (remaining_ticks == 0U)
            {
                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_TIMEOUT;
                waiting = false;
            }
            else
            {
                /* Join the senders' waiter list inside the same critical section
                 * that saw the queue full (no lost-wakeup window). */
                os_task_wait_begin(&queue->send_waiters, remaining_ticks);
                os_critical_exit();

                /* Resumed: a receive freed a slot (retry) or the wait timed out.
                 * The budget is recomputed against the wall clock so READY time
                 * counts toward the timeout. */
                if (os_task_wait_signaled())
                {
                    remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
                }
                else
                {
                    /* The wait ran out. Rather than answer here, go round once more with no
                     * budget left and let the branches above decide while they hold the critical
                     * section - which is what makes dropping an item safe in OVERWRITE mode, and
                     * is why that decision cannot be taken out here.
                     *
                     * A receive that landed in this window is picked up on the way past, so the
                     * send succeeds rather than reporting a timeout it no longer had. */
                    os_task_wait_end();

                    remaining_ticks = 0U;
                }
            }
        }
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Receive one item from queue, waiting up to timeout_ms when empty.
 *
 * @param[in,out] queue       Queue object.
 * @param[out]    item_out    Destination buffer.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_err_t  OK on receive, EMPTY when no items without waiting,
 *                    TIMEOUT when the wait elapsed.
 */
os_err_t os_queue_receive(os_queue_t *queue, void *item_out, uint32_t timeout_ms)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((queue != NULL) && (item_out != NULL))
    {
        uint32_t budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
        uint32_t start_tick      = os_tick_get();
        uint32_t remaining_ticks = budget_ticks;
        bool     waiting         = true;

        /* Retry loop with one exit (MISRA Rule 15.5), mirroring os_queue_send. */
        while (waiting)
        {
            os_critical_enter();

            if (queue->count > 0U)
            {
                const uint8_t *slot = &queue->buffer[queue->head * queue->item_size];

                (void)memcpy(item_out, slot, queue->item_size);
                queue->head = (queue->head + 1U) % queue->capacity;
                queue->count--;

                /* A slot freed up: release the highest-priority sender. */
                (void)os_task_waiters_wake_one(&queue->send_waiters);

                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_NONE;
                waiting = false;
            }
            else if ((timeout_ms == OS_WAIT_NOTHING) || (!os_internal_can_block()))
            {
                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_EMPTY;
                waiting = false;
            }
            else if (remaining_ticks == 0U)
            {
                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_TIMEOUT;
                waiting = false;
            }
            else
            {
                /* Join the receivers' waiter list inside the same critical section
                 * that saw the queue empty (no lost-wakeup window). */
                os_task_wait_begin(&queue->receive_waiters, remaining_ticks);
                os_critical_exit();

                /* Resumed: a send delivered an item (retry) or the wait timed out.
                 * The budget is recomputed against the wall clock so READY time
                 * counts toward the timeout. */
                if (os_task_wait_signaled())
                {
                    remaining_ticks = os_internal_wait_remaining(budget_ticks, start_tick);
                }
                else
                {
                    os_task_wait_end();

                    status  = OS_ERR_TIMEOUT;
                    waiting = false;
                }
            }
        }
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Choose what a send does when the queue is full.
 *
 * Under the critical section because a send on another core reads this field to decide whether
 * it may drop an item, and a half-written answer there is the difference between losing data and
 * refusing it.
 *
 * Blocked senders are deliberately NOT woken: each keeps the timeout it started with and re-reads
 * the mode when that expires, which is the same answer it would have reached had this call landed
 * a moment sooner.
 *
 * @param[in,out] queue  Queue to configure.
 * @param[in]     mode   OS_QUEUE_MODE_NORMAL or OS_QUEUE_MODE_OVERWRITE.
 * @return os_err_t  OS_ERR_NONE, or OS_ERR_INVALID_ARG for a NULL queue or an unknown mode.
 */
os_err_t os_queue_mode_set(os_queue_t *queue, os_queue_mode_t mode)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((queue != NULL) &&
        ((mode == OS_QUEUE_MODE_NORMAL) || (mode == OS_QUEUE_MODE_OVERWRITE)))
    {
        os_critical_enter();
        queue->mode = mode;
        os_critical_exit();

        status = OS_ERR_NONE;
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Get current queue item count.
 *
 * @param[in] queue  Queue object.
 * @return size_t    Number of items currently stored.
 */
size_t os_queue_count_get(const os_queue_t *queue)
{
    size_t count = 0U;

    /* No status to return here: a NULL queue would read as an empty one, so the assert is the
     * only way the caller ever hears about it. */
    OS_ASSERT(queue != NULL);

    if (queue != NULL)
    {
        os_critical_enter();
        count = queue->count;
        os_critical_exit();
    }

    return count;
}

/******************************************************************************************************/
/**
 * @brief Get the number of item slots the queue can still accept.
 *
 * The companion to os_queue_count_get, and the one back-pressure actually asks for: how many more
 * items fit right now, without having to pair a count with the capacity the caller declared
 * somewhere else. Both are snapshots - anything that sends or receives in between changes the
 * answer - so treat a nonzero result as "worth trying", not as a guarantee that the next send
 * cannot report OS_ERR_FULL.
 *
 * @param[in] queue  Queue object.
 * @return size_t    Free item slots; 0 for a dynamic queue with no buffer bound yet.
 */
size_t os_queue_free_get(const os_queue_t *queue)
{
    size_t free_slots = 0U;

    /* No status to return here: a NULL queue would read as an empty one, so the assert is the
     * only way the caller ever hears about it. */
    OS_ASSERT(queue != NULL);

    if (queue != NULL)
    {
        /* count never exceeds capacity (every send checks first), so the
         * subtraction cannot wrap - including on an unbound queue, where both
         * are still 0. */
        os_critical_enter();
        free_slots = queue->capacity - queue->count;
        os_critical_exit();
    }

    return free_slots;
}

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a queue over an item buffer allocated from the kernel heap.
 *
 * For a geometry not known until run time: the caller declares a plain os_queue_t, this gives it a
 * buffer, and os_queue_cleanup releases that buffer because this is what obtained it.
 * Everything else behaves identically. The object itself is still the caller's - only the buffer
 * comes from the heap - so a failed call leaves nothing to clean up.
 *
 * @param[out] queue      Queue object, on zero-initialized storage.
 * @param[in]  item_size  Size of one item in bytes.
 * @param[in]  capacity   Number of items the queue can hold.
 * @return os_err_t  OS_ERR_NONE, OS_ERR_INVALID_ARG for a zero/overflowing geometry,
 *                    OS_ERR_BUSY if the queue still has blocked waiters, or
 *                    OS_ERR_NO_MEMORY when the heap cannot satisfy the request.
 */
os_err_t os_queue_init_dynamic(os_queue_t *queue, size_t item_size, size_t capacity)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    /* The geometry check rejects a byte count that does not fit in size_t before anything is
     * allocated: the product would otherwise wrap to a small, successful allocation that every
     * send and receive then indexes far beyond. It is folded into the same condition as the
     * argument checks, which is safe because item_size is already known nonzero by then - C
     * evaluates && left to right and stops at the first false. */
    if ((queue != NULL) && (item_size != 0U) && (capacity != 0U) &&
        (capacity <= (SIZE_MAX / item_size)))
    {
        void *buffer = os_mem_alloc(item_size * capacity);

        if (buffer == NULL)
        {
            status = OS_ERR_NO_MEMORY;
        }
        else
        {
            /* One critical section covers both the initialization and the ownership flag.
             *
             * Setting buffer_owned in a second, separate critical section would leave the queue fully
             * usable but still claiming it does not own its buffer. An os_queue_cleanup landing in that gap
             * would reset the queue and, seeing buffer_owned false, walk away without freeing the
             * allocation just made - a permanent leak of item_size * capacity bytes with nothing to
             * report it. The window is only a few instructions wide, which is exactly the kind that
             * survives testing and fails in the field.
             *
             * os_queue_bind_buffer performs the waiter check and every field assignment, so this path
             * leaves the object holding exactly what OS_QUEUE_INITIALIZER writes for the compile-time
             * ones. It only fails here if the queue still has blocked waiters, in which case the
             * allocation has to go back rather than leak. */
            os_critical_enter();

            status = os_queue_bind_buffer(queue, buffer, item_size, capacity);

            if (status == OS_ERR_NONE)
            {
                queue->buffer_owned = true;
            }

            os_critical_exit();

            /* status carries the bind's own answer, not a guess at it:
             * os_queue_bind_buffer also reports OS_ERR_INVALID_ARG, and
             * hardcoding BUSY here would be correct only for as long as every
             * INVALID_ARG precondition happens to be checked above. */
            if (status != OS_ERR_NONE)
            {
                os_mem_free(buffer);
            }
        }
    }

    return status;
}
#endif /* OS_CONFIG_ALLOC_ENABLE */

/******************************************************************************************************/
/**
 * @brief Tear down a queue of any storage kind.
 *
 * Every kind converges here, so a caller tearing down a mixed set need not track which is which.
 * A heap buffer (os_queue_init_dynamic) goes back with its geometry, and re-use means another
 * init call; a buffer from the compile-time macros has nothing to release, so the queue is left
 * empty and immediately usable - which is what makes their "never needs an init call" promise hold
 * in both directions.
 *
 * Refuses while tasks are blocked on it: freeing underneath them would park them on list nodes in
 * memory the heap can hand out again, and waking them would need a status senders and receivers
 * cannot tell from a real transfer. Drain it and let the waiters time out first.
 *
 * @param[in,out] queue  Queue to tear down.
 * @return os_err_t  OS_ERR_NONE, OS_ERR_INVALID_ARG for NULL, or OS_ERR_BUSY if any task
 *                    is currently blocked on the queue.
 */
os_err_t os_queue_cleanup(os_queue_t *queue)
{
#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    void *buffer_to_free = NULL;
#endif

    os_err_t status = OS_ERR_INVALID_ARG;

    if (queue != NULL)
    {
        os_critical_enter();

        if ((queue->send_waiters.head != NULL) || (queue->receive_waiters.head != NULL))
        {
            status = OS_ERR_BUSY;
        }
        else
        {
        /* Emptied either way. The waiter lists are already empty - the check above just proved it -
         * so re-initializing them only guarantees a tail left behind by a list bug cannot survive. */
        queue->head  = 0U;
        queue->tail  = 0U;
        queue->count = 0U;
        os_list_init(&queue->send_waiters);
        os_list_init(&queue->receive_waiters);

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
        if (queue->buffer_owned)
        {
            /* Dropped before the critical section ends so the queue cannot be used against a buffer
             * that is about to be released; the freeing itself happens outside, since os_mem_free
             * walks the heap free list and there is no reason to hold interrupts off for it. */
            buffer_to_free      = queue->buffer;
            queue->buffer       = NULL;
            queue->item_size    = 0U;
            queue->capacity     = 0U;
            queue->buffer_owned = false;
        }
#endif

            status = OS_ERR_NONE;
        }

        os_critical_exit();

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
        /* NULL is ignored, so the non-owning case (and the BUSY path, which never
         * set it) needs no branch. */
        os_mem_free(buffer_to_free);
#endif
    }

    return status;
}

#if (OS_CONFIG_ALLOC_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Bind a queue to an item buffer at run time.
 *
 * Not public, deliberately: the compile-time macros read item size and capacity off the array, so
 * an API taking them again would only be a chance for the two to disagree. That leaves one genuine
 * run-time case - a buffer the heap hands over - so os_queue_init_dynamic is the only caller, and
 * keeping it separate writes a queue's initial field set in exactly one place.
 *
 * @param[in,out] queue      Queue object to initialize.
 * @param[in]     buffer     Backing storage buffer.
 * @param[in]     item_size  Size of one item in bytes.
 * @param[in]     capacity   Number of items buffer can hold.
 * @return os_err_t    Status code.
 */
static os_err_t os_queue_bind_buffer(os_queue_t *queue, void *buffer, size_t item_size, size_t capacity)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((queue != NULL) && (buffer != NULL) && (item_size != 0U) && (capacity != 0U))
    {
        os_critical_enter();

        /* Re-initializing with queued waiters would strand them on dangling
         * intrusive nodes and corrupt the lists (first-time init must run on
         * zero-initialized storage - static objects are). */
        if ((queue->send_waiters.head != NULL) || (queue->receive_waiters.head != NULL))
        {
            status = OS_ERR_BUSY;
        }
        else
        {
            queue->buffer       = (uint8_t *)buffer;
            queue->item_size    = item_size;
            queue->capacity     = capacity;
            queue->head         = 0U;
            queue->tail         = 0U;
            queue->count        = 0U;
            queue->buffer_owned = false; /* the caller claims ownership after this, inside its own
                                          * critical section - see os_queue_init_dynamic */
            os_list_init(&queue->send_waiters);
            os_list_init(&queue->receive_waiters);

            status = OS_ERR_NONE;
        }

        os_critical_exit();
    }

    return status;
}

#endif /* OS_CONFIG_ALLOC_ENABLE */

#endif /* OS_CONFIG_QUEUE_ENABLE */
