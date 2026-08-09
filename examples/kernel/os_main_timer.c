/**
 * @file os_main_timer.c
 * @brief Ahura kernel example: software timers (os_timer_*).
 *
 * Starts a one-shot timer and a periodic timer; both callbacks run on the
 * kernel timer task (tsk_timer), not on os_main. Once the periodic timer
 * has fired four times, os_main stops it with os_timer_stop(). Copy this
 * file into the application source tree as os_main.c to run it; needs
 * OS_CONFIG_TIMER_ENABLE=1 in os_config.h (the default).
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
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

static os_timer_t        os_main_timer_oneshot;
static os_timer_t        os_main_timer_periodic;
static __IO uint32_t os_main_periodic_count = 0U;

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void oneshot_cb(void *context)
{
    (void)context;
    printf("[timer] one-shot fired\r\n");
}

/******************************************************************************************************/
static void periodic_cb(void *context)
{
    (void)context;
    os_main_periodic_count++;
    printf("[timer] periodic fired (count=%lu)\r\n", (unsigned long)os_main_periodic_count);
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: runs a one-shot timer and a periodic timer.
 *
 * @return None.
 */
void os_main(void)
{
    (void)os_timer_init(&os_main_timer_oneshot, OS_TICKS_FROM_MS(500U), OS_TIMER_MODE_ONE_SHOT, oneshot_cb, NULL);
    (void)os_timer_init(&os_main_timer_periodic, OS_TICKS_FROM_MS(1000U), OS_TIMER_MODE_PERIODIC, periodic_cb, NULL);

    (void)os_timer_start(&os_main_timer_oneshot);
    (void)os_timer_start(&os_main_timer_periodic);

    while (1)
    {
        os_delay_ms(5000U);

        if (os_main_periodic_count >= 4U)
        {
            break;
        }
    }

    printf("[timer] stopping the periodic timer\r\n");
    (void)os_timer_stop(&os_main_timer_periodic);

    while (1)
    {
        os_delay_ms(1000U);
    }
}
