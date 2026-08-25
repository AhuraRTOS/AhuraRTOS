/**
 * @file os_main_msg.c
 * @brief Ahura kernel example: variable-length message buffer (os_msg_*).
 *
 * os_main is the producer: it sends console-style lines of whatever length each
 * one happens to be - 5 bytes, then 40, then 12 - every 300 ms. A
 * higher-priority consumer task blocks in os_msg_receive() and prints each one
 * as it arrives, with the length the sender gave it.
 *
 * The point of the example is what a queue could not do here. One buffer of 192
 * bytes carries all of those lines; a queue would have to be sized for the
 * LONGEST of them and would then spend that much on the 5-byte one too. Watch
 * the free-byte count in the output: it moves by a different amount for every
 * message, because each one costs exactly its own length plus a small header
 * (OS_MSG_SPACE) rather than a fixed slot.
 *
 * It also shows the two things a byte budget makes the caller responsible for:
 * checking os_msg_peek_size() before committing to a destination, and reading
 * os_msg_free_get() against OS_MSG_SPACE() rather than against a raw length.
 *
 * Copy this file into the application source tree as os_main.c to run it; needs
 * OS_CONFIG_MSG_ENABLE=1 in os_config.h (the default).
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
#include <string.h>

#if !(OS_CONFIG_MSG_ENABLE == 1U)
#error "os_main_msg.c needs OS_CONFIG_MSG_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(consumer, 768U);

/* Longest line this example ever sends, and the destination the consumer brings to every receive.
 * Kept as one constant because the two have to agree: a destination smaller than a waiting message
 * is refused rather than truncated, which is the right answer but is not one the consumer wants to
 * meet at run time. */
#define LINE_MAX        48U

/* The whole storage budget, in BYTES rather than in messages - which is the difference this object
 * exists for. It holds four 40-byte lines, or sixteen 10-byte ones, or any mix that fits; nothing
 * commits it to one shape in advance. OS_MSG_SPACE adds the per-message header so the arithmetic
 * says what it means. */
OS_MSG_DEFINE_STATIC(line_buf, 4U * OS_MSG_SPACE(LINE_MAX));

/* The lines themselves, deliberately of very different lengths. */
static const char *const os_main_lines[] =
{
    "boot",
    "sensor 3 reading 21.75 C, humidity 48 %RH",
    "tick ok",
    "link down, retrying in 5s",
    "ready"
};

#define LINE_COUNT      (sizeof(os_main_lines) / sizeof(os_main_lines[0]))

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Consumer: block until a line arrives, then print it at its own length.
 *
 * Runs above os_main, so it is waiting inside os_msg_receive() before the first send and wakes the
 * moment one lands. The receive is what makes the length trustworthy: whatever the sender passed
 * comes back in length_out, so the bytes can be terminated and printed without a convention about
 * where the line ends.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void consumer_entry(void *context)
{
    char   line[LINE_MAX + 1U];
    size_t length;

    (void)context;

    while (1)
    {
        if (os_msg_receive(&line_buf, line, LINE_MAX, &length, OS_WAIT_FOREVER) == OS_ERR_NONE)
        {
            line[length] = '\0';   /* the buffer is one byte longer than any message, for this */

            printf("  <- %2u bytes: \"%s\"\r\n", (unsigned)length, line);
        }
    }
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task: send lines of varying length and report what each one cost.
 *
 * @return None.
 */
void os_main(void)
{
    uint32_t index = 0U;

    printf("\r\nAhura example: variable-length message buffer\r\n");
    printf("buffer holds %u bytes; each message costs its own length + %u\r\n\r\n",
           (unsigned)os_msg_free_get(&line_buf), (unsigned)OS_MSG_HEADER_BYTES);

    if (os_task_create(&consumer, OS_TASK_CONFIG(consumer_entry, NULL, OS_TASK_PRIO_3)) != OS_ERR_NONE)
    {
        printf("consumer task could not be created\r\n");
        return;
    }

    (void)os_task_start(&consumer);

    while (1)
    {
        const char *text   = os_main_lines[index];
        size_t      length = strlen(text);

        /* Read against OS_MSG_SPACE, never against the raw length: the message pays for its own
         * header too, so a buffer with exactly `length` bytes free still has no room for it. */
        if (os_msg_free_get(&line_buf) >= OS_MSG_SPACE(length))
        {
            printf("-> %2u bytes\r\n", (unsigned)length);
            (void)os_msg_send(&line_buf, text, length, 100U);
        }
        else
        {
            printf("-> %2u bytes would not fit right now (%u free)\r\n",
                   (unsigned)length, (unsigned)os_msg_free_get(&line_buf));
        }

        index = (index + 1U) % LINE_COUNT;

        os_delay_ms(300U);
    }
}
