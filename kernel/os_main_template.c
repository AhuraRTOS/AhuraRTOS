/**
 * @file os_main_template.c
 * @brief Template for the application's default task body.
 *
 * NOT part of the kernel build (like os_cb_template.c): copy this file into the application source
 * tree as os_main.c, add it to the APPLICATION build, and write the application's own code inside
 * os_main(). Its prototype is already in ahura.h.
 *
 * The "_cb" suffix used elsewhere is reserved for callbacks the kernel queries for platform
 * behaviour; os_main() is different in kind - it is where the application's code runs - even
 * though it is supplied the same way.
 *
 * The task itself is created by os_init(), unconditionally except in self-test builds, and sized
 * by OS_CONFIG_MAIN_TASK_STACK_SIZE / OS_CONFIG_MAIN_TASK_PRIORITY. Nothing to call from main().
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

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: runs once os_start() hands control to task context.
 *        Replace the body with the application's own code.
 *
 * @return None.
 */
void os_main(void)
{
    while (1)
    {
        /* TODO: replace with the application's own code. */
        os_delay_ms(1000U);
    }
}
