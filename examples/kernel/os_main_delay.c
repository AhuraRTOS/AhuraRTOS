/**
 * @file os_main_delay.c
 * @brief Ahura kernel example: blocking delays (os_delay_ms / os_delay_us).
 *
 * Demonstrates the three delay flavors and the kernel tick counter. Copy
 * this file into the application source tree as os_main.c to run it - no
 * extra os_config.h switch needed, delays are always available.
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

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: cycles through millisecond, microsecond and second delays.
 *
 * @return None.
 */
void os_main(void)
{
    while (1)
    {
        uint32_t before = os_tick_get();

        printf("[delay] os_delay_ms(500)...\r\n");
        os_delay_ms(500U);

        printf("[delay] os_delay_us(200)...\r\n");
        os_delay_us(200U);

        printf("[delay] %lu ticks elapsed so far\r\n", (unsigned long)(os_tick_get() - before));

        printf("[delay] os_delay_ms(1000)...\r\n");
        os_delay_ms(1000U);
    }
}
