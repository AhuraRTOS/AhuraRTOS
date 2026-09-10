/**
 * @file os_msg.h
 * @brief Variable-length message buffer (os_msg.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_MSG_H
#define OS_MSG_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Message buffer     - OS_CONFIG_MSG_ENABLE
 * ***********************************************************************************************************
 *
 * A queue for messages whose LENGTH varies. Where os_queue_t stores N items of one fixed size, this
 * stores as many messages as fit in a byte budget, each exactly as long as it is:
 *
 *     OS_MSG_DEFINE(cmd_buf, 256U);
 *     os_msg_send(&cmd_buf, frame, frame_len, 10U);
 *     os_msg_receive(&cmd_buf, rx, sizeof(rx), &rx_len, OS_WAIT_FOREVER);
 *
 * Reach for it when the length is data rather than a constant, and for a queue when every item is
 * the same struct. Capacity is in BYTES, and each message costs its own length plus a 2-byte header;
 * os_msg_send() adds that itself and answers OS_ERR_FULL when the message does not fit.
 *
 * Messages arrive whole and in order, one per os_msg_receive(): never a fragment, never two joined.
*/

#if (OS_CONFIG_MSG_ENABLE == 1U)

/** Bytes of overhead each stored message carries: its length header.
 *
 *  Two, so a message may be up to 64 KiB - far past anything an MCU link sends in one piece - while
 *  costing short messages almost nothing. The width is fixed rather than configurable on purpose:
 *  it is the difference between a 12-byte message costing 14 bytes and costing 16, and a knob for
 *  that is a decision every project would have to make and none would benefit from. */
#define OS_MSG_HEADER_BYTES     2U

/** Longest single message, in bytes: what OS_MSG_HEADER_BYTES can express. os_msg_send refuses
 *  anything longer with OS_ERR_INVALID_ARG rather than truncating it. */
#define OS_MSG_LENGTH_MAX       0xFFFFU

/** Storage one message of "length" bytes occupies: its bytes plus its header.
 *
 *  Not something callers should need. os_msg_send() does this arithmetic itself and answers
 *  OS_ERR_FULL, and a budget is written as "so many messages of so many bytes, plus 2 each",
 *  which is the same sum in the terms the application already thinks in. Kept because the
 *  kernel's own size checks are written against it. */
#define OS_MSG_SPACE(length)    ((size_t)(length) + (size_t)OS_MSG_HEADER_BYTES)

/******************************************************************************************************/
/**
 * @brief Message buffer object: a byte ring carrying whole variable-length messages.
 */
typedef struct
{
    uint8_t   *buffer;
    size_t    capacity;        /**< Storage in bytes, headers included.          */
    size_t    head;            /**< Read offset into buffer.                     */
    size_t    tail;            /**< Write offset into buffer.                    */
    size_t    used;            /**< Bytes currently in use, headers included.    */
    size_t    count;           /**< Whole messages currently stored.             */
    os_list_t send_waiters;    /**< Tasks blocked because the message would not fit. */
    os_list_t receive_waiters; /**< Tasks blocked because nothing is waiting.    */
    bool      buffer_owned;    /**< Buffer came from os_msg_init_dynamic: os_msg_cleanup frees it. */

} os_msg_t;

/** Compile-time initializer binding a message buffer to a byte array, shared by the two macros
 *  below so they cannot drift apart. Everything omitted is zero-initialized by the C rules for
 *  static storage, which is exactly the empty buffer an init call would write.
 *
 *  Its only parameter is "array" on purpose: one named after a struct field would be substituted
 *  inside the designated initializers, turning ".capacity" into ".256". */
/* --- Compile-time storage: the capacity is read off the array --------------------------------- */

#define OS_MSG_INITIALIZER(array)              \
    {                                          \
        .buffer   = (uint8_t *)(array),        \
        .capacity = sizeof(array),             \
    }

/** Define a message buffer with statically allocated storage, ready to use where it stands. The
 *  object is plain "name"; the array gets "name_msg_buf", which nothing should name by hand.
 *
 *      OS_MSG_DEFINE(cmd_buf, 256U);
 *      status = os_msg_send(&cmd_buf, frame, frame_len, 10U);
 *
 *  byte_size is a BYTE budget, not a message count, and every message costs 2 bytes more than its
 *  length for its header. So those 256 bytes hold two 126-byte messages, eight 30-byte ones, or any
 *  mix that fits; os_msg_send() does that arithmetic itself.
 *
 *  File scope only. The array is static, the object is not, so a header can share it with
 *  OS_MSG_DECLARE and the name has to be unique across the link. */
#define OS_MSG_DEFINE(name, byte_size)                 \
    static uint8_t  name##_msg_buf[(byte_size)];       \
    os_msg_t name = OS_MSG_INITIALIZER(name##_msg_buf)

/** OS_MSG_DEFINE, plus attributes on the byte array: a named linker section, DMA-capable RAM, a
 *  particular alignment. Identical in every other way.
 *
 *      OS_MSG_DEFINE_ATTR(rx_buf, 512U, __attribute__((section(".dma_buffers"))));
 *
 *  Variadic, so several attributes may be given whatever commas they contain. */
#define OS_MSG_DEFINE_ATTR(name, byte_size, ...)             \
    static uint8_t  name##_msg_buf[(byte_size)] __VA_ARGS__; \
    os_msg_t name = OS_MSG_INITIALIZER(name##_msg_buf)

/** Name a message buffer defined in another file. Only the object crosses; its byte array stays
 *  private to the file that defined it. */
#define OS_MSG_DECLARE(name)            extern os_msg_t name

/* --- Dynamic storage: the byte buffer comes from the kernel heap ------------------------------ */

/* A dynamic message buffer needs no DEFINE macro either: it is a plain os_msg_t and
 * os_msg_init_dynamic() obtains the storage. That call expects the object zeroed. */

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a message buffer over storage allocated from the kernel heap, for a capacity
 *        not known until run time. byte_size is a BYTE budget exactly as in OS_MSG_DEFINE,
 *        and every message costs 2 bytes more than its length. Only the buffer is allocated; the
 *        object itself is the caller's, and os_msg_cleanup releases what this obtained.
 */
os_err_t os_msg_init_dynamic(os_msg_t *msg, size_t byte_size);
#endif /* OS_CONFIG_ALLOC_ENABLE */

/******************************************************************************************************/
/**
 * @brief Send one message, waiting up to timeout_ms while it does not fit. A message longer than
 *        the whole buffer is refused with OS_ERR_INVALID_ARG rather than waited on, since no
 *        receiver could ever make room for it.
 */
os_err_t os_msg_send(os_msg_t *msg, const void *data, size_t length, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Receive the oldest message, waiting up to timeout_ms while there is none. A destination
 *        too small is OS_ERR_INVALID_ARG with the message left in place and length_out set to the
 *        size it needs - nothing is truncated.
 */
os_err_t os_msg_receive(os_msg_t *msg, void *data, size_t data_size, size_t *length_out, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Get how many whole messages are waiting.
 */
size_t os_msg_count_get(const os_msg_t *msg);

/******************************************************************************************************/
/**
 * @brief Get how many bytes of storage are still free - raw bytes, headers not deducted. A
 *        message of L bytes needs L + 2 of them. Usually there is nothing to check here: send it
 *        and read the status.
 */
size_t os_msg_free_get(const os_msg_t *msg);

/******************************************************************************************************/
/**
 * @brief Get the length of the next message without consuming it; 0 when none is waiting.
 */
size_t os_msg_peek_size(const os_msg_t *msg);

/******************************************************************************************************/
/**
 * @brief Tear down a message buffer of any storage kind: every stored message is discarded, and a
 *        heap buffer goes back to the heap while a compile-time one is left empty and immediately
 *        usable. Refuses with OS_ERR_BUSY while tasks are blocked on it.
 */
os_err_t os_msg_cleanup(os_msg_t *msg);

#endif /* OS_CONFIG_MSG_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_MSG_H */
