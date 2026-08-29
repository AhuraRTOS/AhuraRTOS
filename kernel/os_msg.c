/**
 * @file os_msg.c
 * @brief Variable-length message buffer implementation with timeouts.
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

#if (OS_CONFIG_MSG_ENABLE == 1U)

/* WHY THIS IS NOT A QUEUE
 *
 * os_queue stores N items of one fixed size, and that fixed size is the whole of what makes it
 * cheap: a slot address is one multiplication, every item costs the same, and the storage the
 * queue needs is settled once at the definition. Variable-length messages break all three.
 *
 * Sizing a queue for the LONGEST message a link can carry and then sending mostly short ones
 * spends the difference on every slot - a 256-byte protocol frame whose typical payload is 12
 * bytes wastes most of the buffer - while padding short messages up to that size throws away the
 * one thing the sender needed to tell the receiver, which is how many of those bytes are real.
 * Carrying the length as a field inside the item does not fix it either: the storage cost is
 * already paid by then.
 *
 * So this module keeps its own ring, and it is a ring of BYTES rather than of slots. Each message
 * is stored as a small length header followed by exactly its own bytes, and the next message
 * begins where the last one ended. Capacity is therefore stated in bytes and shared: one buffer
 * holds many short messages or few long ones, whichever the traffic turns out to be, with no slot
 * count to guess in advance.
 *
 * What it gives up is the queue's uniformity, and both places that shows are handled below rather
 * than hidden. There is no honest "how many more messages fit" - only how many bytes are free, see
 * os_msg_free_get. And a sender woken by a receive may still not fit, because the message that
 * left could be smaller than the one waiting to arrive, which is why the retry re-tests the
 * condition instead of trusting the wake.
 */

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void   os_msg_ring_write(os_msg_t *msg, const uint8_t *source, size_t length);
static void   os_msg_ring_read(os_msg_t *msg, uint8_t *destination, size_t length);
static void   os_msg_length_write(os_msg_t *msg, size_t length);
static size_t os_msg_length_peek(const os_msg_t *msg);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Send one message, waiting up to timeout_ms while it does not fit.
 *
 * A message is stored as a length header followed by its bytes, and a send writes the whole of
 * both or nothing at all: a half-written message is never visible to a receiver, and two senders
 * cannot interleave, because the copy happens inside one critical section. Keep length modest for
 * that reason - the copy runs with the kernel's interrupts masked, exactly as os_queue_send's does.
 *
 * A message larger than the buffer could hold even when completely empty is refused immediately
 * with OS_ERR_INVALID_ARG rather than blocking on it. Waiting for room that can never exist is a
 * hang, and a hang is the one outcome a timeout does not save the caller from - OS_WAIT_FOREVER
 * would simply never return.
 *
 * @param[in,out] msg         Message buffer object.
 * @param[in]     data        Bytes to copy in.
 * @param[in]     length      How many, 1..OS_MSG_LENGTH_MAX.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_err_t  OK on send; INVALID_ARG for a bad argument or a message this buffer can never
 *                    hold; FULL when it does not fit and the call may not block; TIMEOUT when the
 *                    wait elapsed with the room still unavailable.
 */
os_err_t os_msg_send(os_msg_t *msg, const void *data, size_t length, uint32_t timeout_ms)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((msg != NULL) && (data != NULL) && (length != 0U) && (length <= OS_MSG_LENGTH_MAX))
    {
        size_t   needed          = length + OS_MSG_HEADER_BYTES;
        uint32_t budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
        uint32_t start_tick      = os_tick_get();
        uint32_t remaining_ticks = budget_ticks;
        bool     waiting         = true;

        /* Retry loop with one exit (MISRA Rule 15.5), mirroring os_queue_send: each arm records
         * the outcome in status and clears the loop flag rather than returning for itself. */
        while (waiting)
        {
            os_critical_enter();

            if (needed > msg->capacity)
            {
                /* Not satisfiable by anything any receiver could do. Tested in here rather than
                 * once before the loop because capacity is also 0 on an object no storage was ever
                 * bound to, and that case has to answer the same way. */
                os_task_wait_end();
                os_critical_exit();

                status  = OS_ERR_INVALID_ARG;
                waiting = false;
            }
            else if ((msg->capacity - msg->used) >= needed)
            {
                os_msg_length_write(msg, length);
                os_msg_ring_write(msg, (const uint8_t *)data, length);

                msg->used += needed;
                msg->count++;

                /* A message arrived: release the highest-priority receiver. */
                (void)os_task_waiters_wake_one(&msg->receive_waiters);

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
                /* Join the senders' waiter list inside the same critical section that saw there
                 * was no room (no lost-wakeup window). */
                os_task_wait_begin(&msg->send_waiters, remaining_ticks);
                os_critical_exit();

                /* Resumed: a receive freed bytes (retry) or the wait timed out. A wake is not a
                 * promise that THIS message now fits - the message that left may have been shorter
                 * than this one - which is why the loop re-tests rather than assuming. The budget
                 * is recomputed against the wall clock so READY time counts toward the timeout. */
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
 * @brief Receive the oldest message, waiting up to timeout_ms while there is none.
 *
 * Messages come out whole and in order, one per call: the length the sender wrote is the length
 * that comes back, never part of a message and never two of them joined.
 *
 * A destination too small for the waiting message is refused with OS_ERR_INVALID_ARG and the
 * message is LEFT WHERE IT IS - nothing is truncated, nothing is dropped. length_out still
 * receives the size that message needs, so one failed call tells the caller exactly how big a
 * buffer to come back with; os_msg_peek_size() answers the same question without spending a call.
 * Note the consequence: a receiver that keeps offering the same too-small buffer keeps meeting the
 * same message, so this status is a bug to fix rather than a condition to retry around.
 *
 * @param[in,out] msg         Message buffer object.
 * @param[out]    data        Destination for the message bytes.
 * @param[in]     data_size   Capacity of that destination, in bytes.
 * @param[out]    length_out  Receives the message length; on INVALID_ARG from a too-small
 *                            destination, the length that message WOULD have needed.
 * @param[in]     timeout_ms  OS_WAIT_NOTHING, a duration in ms, or OS_WAIT_FOREVER.
 * @return os_err_t  OK on receive; INVALID_ARG for a bad argument or a destination too small for
 *                    the waiting message; EMPTY when none is waiting and the call may not block;
 *                    TIMEOUT when the wait elapsed with none delivered.
 */
os_err_t os_msg_receive(os_msg_t *msg, void *data, size_t data_size, size_t *length_out, uint32_t timeout_ms)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((msg != NULL) && (data != NULL) && (length_out != NULL))
    {
        uint32_t budget_ticks    = os_internal_timeout_to_ticks(timeout_ms);
        uint32_t start_tick      = os_tick_get();
        uint32_t remaining_ticks = budget_ticks;
        bool     waiting         = true;

        /* Retry loop with one exit (MISRA Rule 15.5), mirroring os_queue_receive. */
        while (waiting)
        {
            os_critical_enter();

            if (msg->count > 0U)
            {
                size_t length = os_msg_length_peek(msg);

                *length_out = length;

                if (length > data_size)
                {
                    /* Untouched: head has not moved, and the header was only read. */
                    status = OS_ERR_INVALID_ARG;
                }
                else
                {
                    /* Step head past the header, then take the payload behind it. */
                    msg->head = (msg->head + OS_MSG_HEADER_BYTES) % msg->capacity;
                    os_msg_ring_read(msg, (uint8_t *)data, length);

                    msg->used -= (length + OS_MSG_HEADER_BYTES);
                    msg->count--;

                    /* Room freed up: release the highest-priority sender. */
                    (void)os_task_waiters_wake_one(&msg->send_waiters);

                    status = OS_ERR_NONE;
                }

                os_task_wait_end();
                os_critical_exit();

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
                /* Join the receivers' waiter list inside the same critical section that saw the
                 * buffer empty (no lost-wakeup window). */
                os_task_wait_begin(&msg->receive_waiters, remaining_ticks);
                os_critical_exit();

                /* Resumed: a send delivered a message (retry) or the wait timed out. The budget
                 * is recomputed against the wall clock so READY time counts toward the timeout. */
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
 * @brief Get how many whole messages are waiting.
 *
 * @param[in] msg  Message buffer object.
 * @return size_t  Messages currently stored.
 */
size_t os_msg_count_get(const os_msg_t *msg)
{
    size_t count = 0U;

    /* No status to return here: a NULL object would read as an empty one, so the assert is the
     * only way the caller ever hears about it. */
    OS_ASSERT(msg != NULL);

    if (msg != NULL)
    {
        os_critical_enter();
        count = msg->count;
        os_critical_exit();
    }

    return count;
}

/******************************************************************************************************/
/**
 * @brief Get how many bytes of storage are still free, length headers included.
 *
 * The number back-pressure actually wants, but it has to be read against the payload length PLUS
 * its header: a message of L bytes fits when this returns at least L + 2, because every message
 * carries its own two-byte length. A snapshot either way - anything that sends or
 * receives in between changes the answer - so treat a large enough result as "worth trying", not
 * as a guarantee that the next send cannot report OS_ERR_FULL.
 *
 * @param[in] msg  Message buffer object.
 * @return size_t  Free bytes; 0 for an object with no storage bound to it.
 */
size_t os_msg_free_get(const os_msg_t *msg)
{
    size_t free_bytes = 0U;

    /* No status to return here: a NULL object would read as a full one, so the assert is the only
     * way the caller ever hears about it. */
    OS_ASSERT(msg != NULL);

    if (msg != NULL)
    {
        /* used never exceeds capacity (every send checks first), so the subtraction cannot wrap -
         * including on an object with no storage, where both are still 0. */
        os_critical_enter();
        free_bytes = msg->capacity - msg->used;
        os_critical_exit();
    }

    return free_bytes;
}

/******************************************************************************************************/
/**
 * @brief Get the length of the next message without consuming it.
 *
 * What a receiver needs in order to size a destination before committing to one, or to decide
 * whether the message is worth taking at all. Non-destructive: the message stays exactly where it
 * was, and the next os_msg_receive() still returns it.
 *
 * @param[in] msg  Message buffer object.
 * @return size_t  Length of the oldest waiting message in bytes, or 0 when none is waiting. A
 *                 stored message is never 0 bytes long - os_msg_send refuses that - so 0 means
 *                 empty and nothing else.
 */
size_t os_msg_peek_size(const os_msg_t *msg)
{
    size_t length = 0U;

    /* No status to return here: a NULL object would read as an empty one, so the assert is the
     * only way the caller ever hears about it. */
    OS_ASSERT(msg != NULL);

    if (msg != NULL)
    {
        os_critical_enter();

        if (msg->count > 0U)
        {
            length = os_msg_length_peek(msg);
        }

        os_critical_exit();
    }

    return length;
}

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a message buffer over storage allocated from the kernel heap.
 *
 * There is no geometry product to overflow here, unlike os_queue_init_dynamic: a message buffer is
 * sized in bytes directly, so byte_size is already the allocation size. What it does check is that
 * the budget can hold at least one message - a buffer of two bytes or fewer has room for a header
 * and nothing else, so every send against it would be refused and the object would be useless in a
 * way only visible at run time.
 *
 * @param[in,out] msg        Message buffer object, on zero-initialized storage.
 * @param[in]     byte_size  Storage to allocate, in bytes, exactly as for OS_MSG_DEFINE.
 *                           Every message stored in it costs 2 bytes more than its length.
 * @return os_err_t  OS_ERR_NONE, OS_ERR_INVALID_ARG for NULL or a budget too small to hold one
 *                    message, OS_ERR_NO_MEMORY if the heap could not supply it, or OS_ERR_BUSY if
 *                    tasks are already blocked on the object.
 */
os_err_t os_msg_init_dynamic(os_msg_t *msg, size_t byte_size)
{
    os_err_t status = OS_ERR_INVALID_ARG;

    if ((msg != NULL) && (byte_size >= OS_MSG_SPACE(1U)))
    {
        void *buffer = os_mem_alloc(byte_size);

        if (buffer == NULL)
        {
            status = OS_ERR_NO_MEMORY;
        }
        else
        {
            /* One critical section covers both the initialization and the ownership flag.
             *
             * Setting buffer_owned in a second, separate critical section would leave the object fully
             * usable but still claiming it does not own its buffer. An os_msg_cleanup landing in that
             * gap would empty the buffer and, seeing buffer_owned false, walk away without freeing the
             * allocation just made - a permanent leak of byte_size bytes with nothing to report it.
             * The window is only a few instructions wide, which is exactly the kind that survives
             * testing and fails in the field. */
            os_critical_enter();

            if ((msg->send_waiters.head != NULL) || (msg->receive_waiters.head != NULL))
            {
                /* Re-initializing underneath blocked tasks would park them on list nodes in memory
                 * the heap can hand out again. */
                status = OS_ERR_BUSY;
            }
            else
            {
                msg->buffer       = (uint8_t *)buffer;
                msg->capacity     = byte_size;
                msg->head         = 0U;
                msg->tail         = 0U;
                msg->used         = 0U;
                msg->count        = 0U;
                msg->buffer_owned = true;

                os_list_init(&msg->send_waiters);
                os_list_init(&msg->receive_waiters);

                status = OS_ERR_NONE;
            }

            os_critical_exit();

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
 * @brief Tear down a message buffer of any storage kind.
 *
 * Every kind converges here, so a caller tearing down a mixed set need not track which is which.
 * A heap buffer (os_msg_init_dynamic) goes back with its capacity, and re-use means another init
 * call; a buffer from the compile-time macros has nothing to release, so the object is left empty
 * and immediately usable - which is what makes their "never needs an init call" promise hold in
 * both directions.
 *
 * Refuses while tasks are blocked on it: freeing underneath them would park them on list nodes in
 * memory the heap can hand out again, and waking them would need a status senders and receivers
 * cannot tell from a real transfer. Drain it and let the waiters time out first.
 *
 * @param[in,out] msg  Message buffer to tear down.
 * @return os_err_t  OS_ERR_NONE, OS_ERR_INVALID_ARG for NULL, or OS_ERR_BUSY if any task is
 *                    currently blocked on the object.
 */
os_err_t os_msg_cleanup(os_msg_t *msg)
{
#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    void *buffer_to_free = NULL;
#endif

    os_err_t status = OS_ERR_INVALID_ARG;

    if (msg != NULL)
    {
        os_critical_enter();

        if ((msg->send_waiters.head != NULL) || (msg->receive_waiters.head != NULL))
        {
            status = OS_ERR_BUSY;
        }
        else
        {
            /* Emptied either way. The waiter lists are already empty - the check above just proved
             * it - so re-initializing them only guarantees that a tail left behind by a list bug
             * cannot survive a teardown. */
            msg->head  = 0U;
            msg->tail  = 0U;
            msg->used  = 0U;
            msg->count = 0U;

            os_list_init(&msg->send_waiters);
            os_list_init(&msg->receive_waiters);

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
            if (msg->buffer_owned)
            {
                /* Dropped before the critical section ends so the object cannot be used against a
                 * buffer that is about to be released; the freeing itself happens outside, since
                 * os_mem_free walks the heap free list and there is no reason to hold interrupts
                 * off for it. */
                buffer_to_free    = msg->buffer;
                msg->buffer       = NULL;
                msg->capacity     = 0U;
                msg->buffer_owned = false;
            }
#endif

            status = OS_ERR_NONE;
        }

        os_critical_exit();

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
        /* NULL is ignored, so the non-owning case (and the BUSY path, which never set it) needs
         * no branch. */
        os_mem_free(buffer_to_free);
#endif
    }

    return status;
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Copy length bytes into the ring at tail, wrapping at most once, and advance tail.
 *
 * The caller has already established that the bytes fit, which is what makes this unable to
 * overwrite unread data and what lets it advance tail unconditionally.
 *
 * @param[in,out] msg     Message buffer object.
 * @param[in]     source  Bytes to copy in.
 * @param[in]     length  How many.
 * @return None.
 */
static void os_msg_ring_write(os_msg_t *msg, const uint8_t *source, size_t length)
{
    size_t first = msg->capacity - msg->tail;

    if (first > length)
    {
        first = length;
    }

    (void)memcpy(&msg->buffer[msg->tail], source, first);

    if (length > first)
    {
        (void)memcpy(msg->buffer, &source[first], length - first);
    }

    /* tail is below capacity and length is at most capacity, so the sum stays under twice it and
     * one modulo is enough - no loop, and no branch on whether the write actually wrapped. */
    msg->tail = (msg->tail + length) % msg->capacity;
}

/******************************************************************************************************/
/**
 * @brief Copy length bytes out of the ring at head, wrapping at most once, and advance head.
 *
 * @param[in,out] msg          Message buffer object.
 * @param[out]    destination  Where the bytes go.
 * @param[in]     length       How many.
 * @return None.
 */
static void os_msg_ring_read(os_msg_t *msg, uint8_t *destination, size_t length)
{
    size_t first = msg->capacity - msg->head;

    if (first > length)
    {
        first = length;
    }

    (void)memcpy(destination, &msg->buffer[msg->head], first);

    if (length > first)
    {
        (void)memcpy(&destination[first], msg->buffer, length - first);
    }

    msg->head = (msg->head + length) % msg->capacity;
}

/******************************************************************************************************/
/**
 * @brief Write a message's length header into the ring at tail, and advance tail past it.
 *
 * One byte at a time, least significant first, through the same ring write every payload uses.
 * Two things follow from that. The header may straddle the wrap like any other bytes, so no space
 * is ever left unused at the end of the buffer to keep a header contiguous - the accounting stays
 * exactly "the sum of OS_MSG_SPACE(length)", which is what makes os_msg_free_get answerable. And
 * the byte order is this file's own convention rather than the CPU's, written here and read by the
 * function below and never seen anywhere else, so nothing depends on the target's endianness.
 *
 * @param[in,out] msg     Message buffer object.
 * @param[in]     length  Message length, already checked against OS_MSG_LENGTH_MAX.
 * @return None.
 */
static void os_msg_length_write(os_msg_t *msg, size_t length)
{
    uint8_t header[OS_MSG_HEADER_BYTES];
    size_t  index;

    for (index = 0U; index < OS_MSG_HEADER_BYTES; index++)
    {
        header[index] = (uint8_t)((length >> (8U * index)) & 0xFFU);
    }

    os_msg_ring_write(msg, header, OS_MSG_HEADER_BYTES);
}

/******************************************************************************************************/
/**
 * @brief Read the oldest message's length header WITHOUT consuming it.
 *
 * head is walked with a local copy rather than advanced, which is what lets os_msg_peek_size() and
 * the too-small-destination path in os_msg_receive() leave the message exactly where it was.
 *
 * @param[in] msg  Message buffer object, with at least one message stored.
 * @return size_t  Length of that message, in bytes.
 */
static size_t os_msg_length_peek(const os_msg_t *msg)
{
    size_t index  = msg->head;
    size_t length = 0U;
    size_t byte;

    for (byte = 0U; byte < OS_MSG_HEADER_BYTES; byte++)
    {
        length |= ((size_t)msg->buffer[index]) << (8U * byte);
        index   = (index + 1U) % msg->capacity;
    }

    return length;
}

#endif /* OS_CONFIG_MSG_ENABLE */
