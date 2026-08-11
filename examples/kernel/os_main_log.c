/**
 * @file os_main_log.c
 * @brief Ahura kernel example: buffered debug logging (OS_LOG_*).
 *
 * The point of OS_LOG_* is that logging costs the calling task almost nothing.
 * The line is formatted at the call site into a ring buffer and the call
 * returns; the low-priority kernel task tsk_log drains the buffer in the
 * background and hands finished bytes to os_log_output_cb(), which owns the
 * transport. printf, by contrast, pushes every byte to the UART before it
 * returns, so at 115200 baud an 80-character line stalls the caller for roughly
 * 7 ms, which is more than seven scheduler ticks.
 *
 * This example measures that difference, then shows what happens when logging
 * outruns the transport.
 *
 * Copy this file into the application source tree as os_main.c to run it. Needs
 * OS_CONFIG_LOG_ENABLE=1 in os_config.h, and the application must define
 * os_log_output_cb() (see template/os_cb.c) or the output goes nowhere.
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

#if !(OS_CONFIG_LOG_ENABLE == 1U)
#error "os_main_log.c needs OS_CONFIG_LOG_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define LOG_BURST_LINES 200U

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: contrasts buffered logging with blocking printf.
 *
 * @return None.
 */
void os_main(void)
{
    uint32_t iteration = 0U;

    /* Severity picks the letter in the line and, more importantly, whether the call survives
     * OS_CONFIG_LOG_LEVEL at all. Anything above the configured level compiles to nothing at the
     * call site, arguments included, so an OS_LOG_DEBUG left in a release build costs neither
     * flash nor the time to evaluate what it would have printed. */
    OS_LOG_ERROR("this is an error line");
    OS_LOG_WARN("this is a warning line");
    OS_LOG_INFO("printf-style formatting works: %s = %d, %#x", "value", 42, 0xB0Bu);
    OS_LOG_DEBUG("only compiled in when OS_CONFIG_LOG_LEVEL is OS_LOG_LEVEL_DEBUG");

    /* --- Cost at the call site --- */
    {
        uint32_t t_log;
        uint32_t t_printf;
        uint32_t start;

        start = os_tick_get();
        OS_LOG_INFO("measuring how long this call takes to return");
        t_log = os_tick_get() - start;

        start = os_tick_get();
        printf("[log] measuring how long this printf takes to return\r\n");
        t_printf = os_tick_get() - start;

        /* Expect the log call to be at or near 0 ticks while printf shows the transmission. The
         * bytes of that log line are still going out after the call returned, on tsk_log. */
        OS_LOG_INFO("OS_LOG_INFO returned in %lu ticks, printf in %lu",
                    (unsigned long)t_log, (unsigned long)t_printf);
    }

    /* --- Overrun behaviour --- */

    /* Burst far more than the ring can hold. Since this task outranks tsk_log, the drain task
     * cannot run until this loop blocks, so the buffer fills. A line that does not fit is dropped
     * WHOLE and counted, never truncated into the buffer: half a line would corrupt both the line
     * already there and the one after it. */
    for (iteration = 0U; iteration < LOG_BURST_LINES; iteration++)
    {
        OS_LOG_INFO("burst line %lu of %lu", (unsigned long)iteration, (unsigned long)LOG_BURST_LINES);
    }

    /* Blocking here lets tsk_log run and drain what did fit. Once the ring empties it reports the
     * drops itself, as a "*** N log lines dropped ***" line. */
    os_delay_ms(500U);

    printf("[log] %lu of %lu burst lines were dropped by a full buffer\r\n",
           (unsigned long)os_log_dropped_get(), (unsigned long)LOG_BURST_LINES);
    printf("[log] raise OS_CONFIG_LOG_BUFFER_SIZE, or log less, to reduce that\r\n");

    /* --- Steady state --- */

    iteration = 0U;

    while (1)
    {
        /* One line every second drains long before the next arrives, so nothing is ever dropped
         * and this task never waits on the UART. */
        OS_LOG_INFO("heartbeat %lu, tick %lu", (unsigned long)iteration, (unsigned long)os_tick_get());
        iteration++;
        os_delay_ms(1000U);
    }
}
