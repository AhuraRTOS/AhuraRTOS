/**
 * @file os_main_msg.c
 * @brief Ahura kernel example: variable-length message buffer (os_msg_*).
 *
 * Two message buffers of the same size, differing only in where their storage
 * came from:
 *
 *   static_buf   OS_MSG_DEFINE_STATIC  - an array the macro declares
 *   dynamic_buf  OS_MSG_DEFINE_DYNAMIC - bytes from the kernel heap
 *
 * os_main sends to one, then the other, over and over. Two reader tasks each
 * block on their own buffer and print what arrives. The messages are of
 * different lengths on purpose: a message buffer stores each one at its own
 * length, where a queue would need a slot sized for the longest.
 *
 * Copy this file into the application source tree as os_main.c to run it.
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

#if !(OS_CONFIG_ALLOC_ENABLE == 1U)
#error "os_main_msg.c also shows the dynamic form, which needs OS_CONFIG_ALLOC_ENABLE=1"
#endif

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

/* Storage in BYTES, not in messages: 128 bytes hold a few long messages or many short ones. */
#define BUF_BYTES   128U

OS_MSG_DEFINE_STATIC(static_buf, BUF_BYTES);   /* storage is an array, ready to use          */
OS_MSG_DEFINE_DYNAMIC(dynamic_buf);            /* storage comes from os_msg_init_dynamic()   */

/* Every task states its core affinity on a multi-core build: the kernel asks for that argument
 * rather than defaulting it, so the decision is made on purpose at each creation site. These
 * examples have no placement preference, so they take any core. */
#if (OS_CONFIG_CORE_COUNT == 1U)
#define EXAMPLE_TASK(entry, context, priority)  OS_TASK_CONFIG((entry), (context), (priority))
#else
#define EXAMPLE_TASK(entry, context, priority)  \
    OS_TASK_CONFIG((entry), (context), (priority), OS_TASK_CORE_ANY)
#endif

OS_TASK_DEFINE(reader_static, 768U);
OS_TASK_DEFINE(reader_dynamic, 768U);


/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Reader for the static buffer: block until a message arrives, then print it.
 *
 * length_out is what makes the bytes printable - the receiver is told how many arrived, so there
 * is no terminator to agree on.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void reader_static_entry(void *context)
{
    char   text[64];
    size_t length;

    (void)context;

    while (1)
    {
        if (os_msg_receive(&static_buf, text, sizeof(text) - 1U, &length, OS_WAIT_FOREVER) == OS_ERR_NONE)
        {
            text[length] = '\0';

            printf("  static  <- %s\r\n", text);
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Reader for the dynamic buffer: identical code, different storage.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void reader_dynamic_entry(void *context)
{
    char   text[64];
    size_t length;

    (void)context;

    while (1)
    {
        if (os_msg_receive(&dynamic_buf, text, sizeof(text) - 1U, &length, OS_WAIT_FOREVER) == OS_ERR_NONE)
        {
            text[length] = '\0';

            printf("  dynamic <- %s\r\n", text);
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
 * @brief Default application task: send to the static buffer, then the dynamic one, in turn.
 *
 * @return None.
 */
void os_main(void)
{
    char     text[64];
    uint32_t counter = 1U;

    printf("\r\nAhura example: message buffers, static and dynamic\r\n\r\n");

    /* The only line the two buffers do not share. After it, both are used identically. */
    if (os_msg_init_dynamic(&dynamic_buf, BUF_BYTES) != OS_ERR_NONE)
    {
        printf("dynamic buffer could not be allocated\r\n");
        return;
    }

    if ((os_task_create(&reader_static, EXAMPLE_TASK(reader_static_entry, NULL, OS_TASK_PRIO_3)) != OS_ERR_NONE) ||
        (os_task_create(&reader_dynamic, EXAMPLE_TASK(reader_dynamic_entry, NULL, OS_TASK_PRIO_3)) != OS_ERR_NONE))
    {
        printf("reader tasks could not be created\r\n");
        return;
    }

    (void)os_task_start(&reader_static);
    (void)os_task_start(&reader_dynamic);

    while (1)
    {
        (void)snprintf(text, sizeof(text), "hello %lu", (unsigned long)counter);
        printf("static  -> %s\r\n", text);
        (void)os_msg_send(&static_buf, text, strlen(text), 100U);

        os_delay_ms(500U);

        (void)snprintf(text, sizeof(text), "a longer message, number %lu", (unsigned long)counter);
        printf("dynamic -> %s\r\n", text);
        (void)os_msg_send(&dynamic_buf, text, strlen(text), 100U);

        os_delay_ms(500U);

        counter++;
    }
}
