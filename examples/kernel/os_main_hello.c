/**
 * @file os_main_hello.c
 * @brief Ahura kernel example: hello world.
 *
 * The smallest possible Ahura application. os_init()/os_start() (called from
 * the platform's main()) create and start this file's os_main() as the
 * default application task, then hand control to it - nothing else to wire
 * up. Copy this file into the application source tree as os_main.c to run
 * it; depends on nothing but ahura.h, so it builds against any os_config.h.
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
 * @brief Default application task body: prints a greeting once, then a heartbeat every second.
 *
 * @return None.
 */
void os_main(void)
{
    printf("Hello, Ahura!\r\n");

    while (1)
    {
        printf("[hello] tick=%lu\r\n", (unsigned long)os_tick_get());
        os_delay_ms(1000U);
    }
}
