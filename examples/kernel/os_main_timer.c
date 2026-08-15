/**
 * @file os_main_timer.c
 * @brief Ahura kernel example: software timers and deferred calls.
 *
 * Three macros, and the macro's name is the behaviour - there is no mode to pass and no init call:
 *
 *   OS_TIMER_PERIODIC_DEFINE   fires every period until stopped
 *   OS_TIMER_ONESHOT_DEFINE    fires once
 *   OS_TIMER_SUBMIT_DEFINE     a pool of deferred calls, for work coming off an interrupt
 *
 * The one distinction worth learning is between the last two, because picking wrong is a bug
 * rather than a matter of taste:
 *
 *   os_timer_start   RESCHEDULES. Starting a timer that is already pending replaces its data and
 *                    pushes the deadline back, so ONE callback runs, carrying the latest event.
 *                    That is a debounce.
 *
 *   os_timer_submit  QUEUES. Each call takes its own slot, so an interrupt firing three times runs
 *                    the callback THREE times, each with its own data. That is deferring work.
 *
 * Copy this file into the application source tree as os_main.c to run it; needs
 * OS_CONFIG_TIMER_ENABLE=1 in os_config.h (the default).
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

#if !(OS_CONFIG_TIMER_ENABLE == 1U)
#error "os_main_timer.c needs OS_CONFIG_TIMER_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Private objects
 * ***********************************************************************************************************
*/

/* Stands in for whatever the work is about - a device, a buffer, a driver instance. Passed as
 * context, never copied, so it has to outlive the run. A file-scope object always does. */
static uint32_t os_main_device = 0xD0D0U;

static void on_blink(void *context, uint32_t value);
static void on_timeout(void *context, uint32_t value);
static void on_event(void *context, uint32_t value);

OS_TIMER_PERIODIC_DEFINE(os_main_blinker, 250U, on_blink);
OS_TIMER_ONESHOT_DEFINE(os_main_timeout,  500U, on_timeout);

/* Four calls may be in flight at once, each run as soon as possible. Both numbers are settled here,
 * so os_timer_submit itself does no arithmetic - which is what an interrupt path wants. */
OS_TIMER_SUBMIT_DEFINE(os_main_events, 4U, 0U, on_event);

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Every callback runs on the kernel timer task, never in an interrupt - so printf is fine
 *        here, and so is blocking.
 */
static void on_blink(void *context, uint32_t value)
{
    (void)context;
    printf("[timer] blink %lu\r\n", (unsigned long)value);
}

/******************************************************************************************************/
static void on_timeout(void *context, uint32_t value)
{
    (void)value;
    printf("[timer] one-shot fired (device=0x%lX)\r\n", (unsigned long)(*(uint32_t *)context));
}

/******************************************************************************************************/
static void on_event(void *context, uint32_t value)
{
    (void)context;
    printf("[timer] deferred event %lu handled\r\n", (unsigned long)value);
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body.
 *
 * @return None.
 */
void os_main(void)
{
    uint32_t event;

    /* 1. Periodic. context and value belong to THIS run, so one callback can serve many timers and
     *    still know which fired. */
    (void)os_timer_start(&os_main_blinker, &os_main_device, 1U);

    /* 2. One-shot: fires 500 ms from now, then stops. */
    (void)os_timer_start(&os_main_timeout, &os_main_device, 0U);

    os_delay_ms(1200U);
    (void)os_timer_stop(&os_main_blinker);

    /* 3. Deferring work. In a real system these lines are in an ISR. All three run, each with its
     *    own value - nothing is lost, because each took its own slot. */
    printf("[timer] three events back to back\r\n");
    for (event = 1U; event <= 3U; event++)
    {
        if (os_timer_submit(&os_main_events, &os_main_device, event) != OS_STATUS_OK)
        {
            /* Only reachable once all four slots are in flight - a real overrun, worth counting. */
            printf("[timer] event %lu dropped: pool full\r\n", (unsigned long)event);
        }
    }

    os_delay_ms(100U);

    /* 4. The contrast. The same three events through os_timer_start produce ONE callback carrying
     *    only the last value: each start replaced the one before it. That is exactly what a
     *    debounce wants, and exactly what deferring work must not do. */
    printf("[timer] the same three through os_timer_start\r\n");
    for (event = 1U; event <= 3U; event++)
    {
        (void)os_timer_restart(&os_main_timeout, &os_main_device, event);
    }

    while (1)
    {
        os_delay_ms(1000U);
    }
}
