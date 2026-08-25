/**
 * @file os_log.c
 * @brief Buffered debug logging: printf-style calls queue into a ring buffer and a
 *        low-priority kernel task hands finished bytes to the application.
 *
 * The point of the buffer is that a log call must not cost the caller a UART transmission:
 * formatting happens at the call site (bounded by OS_CONFIG_LOG_LINE_MAX), the bytes go into the
 * ring, and os_log_write returns. The log task drains it at OS_CONFIG_LOG_TASK_PRIORITY -
 * deliberately low - and calls os_log_output_cb, which owns the transport.
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

#if (OS_CONFIG_LOG_ENABLE == 1U)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_LOG_BUFFER_SIZE < 64U)
#error "OS_CONFIG_LOG_BUFFER_SIZE is too small to hold a useful log line."
#endif

#if (OS_CONFIG_LOG_LINE_MAX < 32U)
#error "OS_CONFIG_LOG_LINE_MAX is too small to hold a useful log line."
#endif

/* The ring can never hold more than SIZE-1 bytes: head == tail has to mean
 * empty, so one slot stays unused to keep "full" distinguishable. */
#define OS_LOG_CAPACITY         (OS_CONFIG_LOG_BUFFER_SIZE - 1U)

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(tsk_log, OS_CONFIG_LOG_TASK_STACK_SIZE);

/* Resolved once in os_log_system_init: the log task is never deleted, so every
 * later wake skips the id lookup (the same trick os_timer.c uses). */
static void      *os_log_task_tcb = NULL;

/* Byte ring. head is the next write position, tail the next read position, so
 * head == tail means empty. Both are only ever touched inside the kernel
 * critical section, which is what makes os_log_write safe from an ISR. */
static uint8_t   os_log_buffer[OS_CONFIG_LOG_BUFFER_SIZE];
static size_t    os_log_head    = 0U;
static size_t    os_log_tail    = 0U;
static uint32_t  os_log_dropped = 0U;

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void   os_log_task_entry(void *context);
static size_t os_log_free_space(void);
static void   os_log_put(const char *data, size_t length);
static void   os_log_queue(const char *data, size_t length);
static void   os_log_emit_dropped(uint32_t dropped);
static size_t os_log_append_text(char *dst, size_t offset, const char *text);
static size_t os_log_append_u32(char *dst, size_t offset, uint32_t value, size_t width);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Format a log line and queue it for transmission (never blocks).
 *
 * Queueing is ISR-safe - the ring buffer is written under a critical section and a full buffer
 * drops rather than waits. FORMATTING is where the caveats are, and both come from this function
 * calling into the C library's printf engine rather than owning one:
 *
 *   Stack.       The scratch line below is OS_CONFIG_LOG_LINE_MAX bytes, but the real requirement
 *                is that PLUS whatever vsnprintf needs underneath it - several hundred bytes with
 *                newlib, and substantially more once a float conversion is reachable. Any task or
 *                ISR that logs has to be sized for both. Sizing only for LOG_LINE_MAX is the
 *                mistake this note exists to prevent.
 *
 *   Reentrancy.  Stock newlib stdio reaches through _impure_ptr, which is shared unless the project
 *                retargets it (__getreent, or a reentrant libc configuration). An OS_LOG_* from an
 *                ISR that lands mid-vsnprintf in a task then shares that state. "%f" can also reach
 *                malloc - the TOOLCHAIN's heap, not os_mem, and not protected by anything here.
 *
 * So: logging from an ISR is supported, but on a single-threaded libc it is only safe if no task
 * can be inside vsnprintf at the time - which in practice means either retargeting the libc or
 * keeping OS_LOG_* out of interrupt context. A float-free printf configuration removes the malloc
 * path and most of the stack cost. The alternative, and the better long-term answer for a kernel,
 * is an in-tree formatter: os_log_append_text/os_log_append_u32 below are most of one already.
 *
 * @param[in] level  OS_LOG_LEVEL_ERROR..DEBUG; only used to pick the severity letter, since the
 *                   OS_LOG_* macros already dropped anything above OS_CONFIG_LOG_LEVEL.
 * @param[in] fmt    printf-style format string.
 * @return None.
 */
void os_log_write(uint32_t level, const char *fmt, ...)
{
    /* Scratch lives on the CALLER's stack: every task that logs needs
     * OS_CONFIG_LOG_LINE_MAX bytes of headroom for it, PLUS the formatter's
     * own frame - see the stack note in this function's doc comment. */
    char line[OS_CONFIG_LOG_LINE_MAX];
    char severity;
    int  prefix_len = -1;

    switch (level)
    {
    case OS_LOG_LEVEL_ERROR: severity = 'E'; break;
    case OS_LOG_LEVEL_WARN:  severity = 'W'; break;
    case OS_LOG_LEVEL_DEBUG: severity = 'D'; break;
    default:                 severity = 'I'; break;
    }

    if (fmt != NULL)
    {
        /* Timestamp first so lines are orderable even when the transport reorders
         * nothing - the tick is read here, at the call site, not at drain time. */
        prefix_len = snprintf(line, sizeof(line), "[%8lu] %c ", (unsigned long)os_tick_get(), severity);
    }

    /* Cannot happen with a sane LINE_MAX, but never index past the buffer. */
    if ((prefix_len >= 0) && ((size_t)prefix_len < sizeof(line)))
    {
        va_list args;
        int     body_len;

        va_start(args, fmt);
        body_len = vsnprintf(&line[prefix_len], sizeof(line) - (size_t)prefix_len, fmt, args);
        va_end(args);

        /* A negative length is an encoding error in the format string: nothing to queue. */
        if (body_len >= 0)
        {
            size_t total = (size_t)prefix_len + (size_t)body_len;

            /* vsnprintf reports what it WOULD have written: clamp to what it actually
             * did, so an over-long line is truncated rather than read out of bounds. */
            if (total > (sizeof(line) - 1U))
            {
                total = sizeof(line) - 1U;
            }

            /* Room for the line ending is reserved from the same budget, so a full
             * line still terminates properly instead of running into the next one. */
            if (total > (sizeof(line) - 3U))
            {
                total = sizeof(line) - 3U;
            }

            line[total]      = '\r';
            line[total + 1U] = '\n';
            total            += 2U;

            os_log_queue(line, total);
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Number of log lines dropped so far because the buffer was full.
 *
 * @return uint32_t  Cumulative dropped-line count.
 */
uint32_t os_log_dropped_get(void)
{
    uint32_t dropped;

    os_critical_enter();
    dropped = os_log_dropped;
    os_critical_exit();

    return dropped;
}

/******************************************************************************************************/
/**
 * @brief Create and start the kernel log service task. Called from os_init.
 *
 * @return os_err_t  Status code.
 */
os_err_t os_log_system_init(void)
{
    os_err_t status;

    os_task_config_t config =
    {
        os_log_task_entry,
        NULL,
        OS_CONFIG_LOG_TASK_PRIORITY,
        OS_CONFIG_LOG_CORE_AFFINITY
    };

    os_log_head    = 0U;
    os_log_tail    = 0U;
    os_log_dropped = 0U;

    status = os_task_create_system(&tsk_log, &config);

    /* Each step only runs once the previous one succeeded, and the first failure
     * is what gets reported. */
    if (status == OS_ERR_NONE)
    {
        status = os_task_start(&tsk_log);
    }

    if (status == OS_ERR_NONE)
    {
        os_log_task_tcb = os_task_tcb_resolve(tsk_log.id);
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
 * @brief Log task body: drain the ring into the output hook, sleep when it is empty.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_log_task_entry(void *context)
{
    (void)context;

    while (1)
    {
        const uint8_t *chunk = NULL;
        size_t        length = 0U;
        uint32_t      dropped = 0U;

        os_critical_enter();

        if (os_log_head != os_log_tail)
        {
            /* Longest run that does not wrap; a wrapped ring is simply
             * delivered as two calls rather than copied straight. */
            size_t end = (os_log_head > os_log_tail) ? os_log_head : OS_CONFIG_LOG_BUFFER_SIZE;

            chunk  = &os_log_buffer[os_log_tail];
            length = end - os_log_tail;
        }

        os_critical_exit();

        if (length != 0U)
        {
            /* Outside the critical section on purpose: the application may
             * block here on a UART or kick off a DMA transfer. The bytes stay
             * valid because the tail only advances below, so producers still
             * count this region as occupied and cannot overwrite it. */
            os_log_output_cb(chunk, length);

            os_critical_enter();
            os_log_tail = (os_log_tail + length) % OS_CONFIG_LOG_BUFFER_SIZE;
            os_critical_exit();

            continue;
        }

        /* Ring is empty. Report anything lost while it was full, once, and
         * only now that there is room for the notice itself. */
        os_critical_enter();
        dropped        = os_log_dropped;
        os_log_dropped = 0U;
        os_critical_exit();

        if (dropped != 0U)
        {
            os_log_emit_dropped(dropped);
            continue;
        }

        /* The emptiness check and the block form one atomic unit (outer
         * critical section), so a line arriving in between cannot be lost: it
         * is seen here, or its wake lands after the block. */
        os_critical_enter();

        if (os_log_head == os_log_tail)
        {
            os_task_sleep_ticks(OS_WAIT_FOREVER);
        }

        os_critical_exit();
    }
}

/******************************************************************************************************/
/**
 * @brief Copy finished bytes into the ring and wake the log task, or count a drop.
 *
 * Split out of os_log_write so the log task itself has a way to emit a line without going back
 * through the formatter. See os_log_emit_dropped for why that matters.
 *
 * @param[in] data    Finished line, line ending included.
 * @param[in] length  Number of bytes.
 * @return None.
 */
static void os_log_queue(const char *data, size_t length)
{
    os_critical_enter();

    if (os_log_free_space() < length)
    {
        /* Drop the whole line rather than half of it: a partial line would
         * corrupt the one already in the buffer and the one after it. */
        os_log_dropped++;
    }
    else
    {
        os_log_put(data, length);

        /* Direct-handle wake: the critical section already holds everything
         * os_task_wake_tcb requires of its caller. NULL before the log task
         * exists (early boot logging), which the wake itself tolerates. */
        os_task_wake_tcb(os_log_task_tcb);
    }

    os_critical_exit();
}

/******************************************************************************************************/
/**
 * @brief Emit the "N log lines dropped" notice, formatted without libc.
 *
 * Deliberately does NOT call os_log_write: that path costs its own frame plus whatever vsnprintf
 * needs, on a stack sized for the output callback rather than the formatter - it overflowed the
 * log task and faulted exactly when the first drop happened. Hand-formatting keeps the worst-case
 * stack shallow and independent of libc. Only called with the ring empty, so the notice itself
 * cannot be the line that gets dropped.
 *
 * @param[in] dropped  Number of lines lost while the buffer was full.
 * @return None.
 */
static void os_log_emit_dropped(uint32_t dropped)
{
    /* Worst case 53 bytes: "[" + 10 digit tick + "] W *** " + 10 digit count
     * + " log lines dropped ***\r\n". */
    char   line[64];
    size_t length = 0U;

    length = os_log_append_text(line, length, "[");
    length = os_log_append_u32(line, length, (uint32_t)os_tick_get(), 8U);
    length = os_log_append_text(line, length, "] W *** ");
    length = os_log_append_u32(line, length, dropped, 0U);
    length = os_log_append_text(line, length, " log lines dropped ***\r\n");

    os_log_queue(line, length);
}

/******************************************************************************************************/
/**
 * @brief Append a NUL-terminated string. Caller guarantees the destination has room.
 *
 * @param[out] dst     Destination buffer.
 * @param[in]  offset  Write position.
 * @param[in]  text    String to append.
 * @return size_t  New write position.
 */
static size_t os_log_append_text(char *dst, size_t offset, const char *text)
{
    while (*text != '\0')
    {
        dst[offset] = *text;
        offset++;
        text++;
    }

    return offset;
}

/******************************************************************************************************/
/**
 * @brief Append an unsigned decimal, right-aligned in at least width columns.
 *
 * @param[out] dst     Destination buffer.
 * @param[in]  offset  Write position.
 * @param[in]  value   Value to render.
 * @param[in]  width   Minimum field width, space padded; 0 for no padding.
 * @return size_t  New write position.
 */
static size_t os_log_append_u32(char *dst, size_t offset, uint32_t value, size_t width)
{
    char   digits[10];
    size_t count = 0U;

    /* Emitted least significant first, then reversed below. */
    do
    {
        digits[count] = (char)('0' + (value % 10U));
        count++;
        value /= 10U;
    } while (value != 0U);

    while (width > count)
    {
        dst[offset] = ' ';
        offset++;
        width--;
    }

    while (count > 0U)
    {
        count--;
        dst[offset] = digits[count];
        offset++;
    }

    return offset;
}

/******************************************************************************************************/
/**
 * @brief Bytes the ring can still accept. Caller holds the critical section.
 *
 * @return size_t  Free bytes, at most OS_LOG_CAPACITY.
 */
static size_t os_log_free_space(void)
{
    size_t used;

    if (os_log_head >= os_log_tail)
    {
        used = os_log_head - os_log_tail;
    }
    else
    {
        used = OS_CONFIG_LOG_BUFFER_SIZE - (os_log_tail - os_log_head);
    }

    return OS_LOG_CAPACITY - used;
}

/******************************************************************************************************/
/**
 * @brief Copy a line into the ring, wrapping as needed. Caller holds the critical section and
 *        has already checked that it fits.
 *
 * @param[in] data    Bytes to store.
 * @param[in] length  Number of bytes.
 * @return None.
 */
static void os_log_put(const char *data, size_t length)
{
    size_t first = OS_CONFIG_LOG_BUFFER_SIZE - os_log_head;

    if (first > length)
    {
        first = length;
    }

    (void)memcpy(&os_log_buffer[os_log_head], data, first);

    if (length > first)
    {
        (void)memcpy(&os_log_buffer[0], &data[first], length - first);
    }

    os_log_head = (os_log_head + length) % OS_CONFIG_LOG_BUFFER_SIZE;
}

#endif /* OS_CONFIG_LOG_ENABLE */
