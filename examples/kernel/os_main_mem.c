/**
 * @file os_main_mem.c
 * @brief Ahura kernel example: kernel heap (os_mem_*).
 *
 * Allocates two blocks from the kernel heap, frees them, and prints the
 * free-byte count and the worst-case watermark around each step - freeing
 * both blocks restores the exact starting free count, since the allocator
 * coalesces adjacent free blocks back together. Copy this file into the
 * application source tree as os_main.c to run it; needs
 * OS_CONFIG_ALLOC_ENABLE=1 in os_config.h (the default).
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

#if !(OS_CONFIG_ALLOC_ENABLE == 1U)
#error "os_main_mem.c needs OS_CONFIG_ALLOC_ENABLE=1 in os_config.h"
#endif

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: allocates, frees, and reports kernel heap stats.
 *
 * @return None.
 */
void os_main(void)
{
    while (1)
    {
        void *a;
        void *b;

        printf("[mem] free=%lu bytes, watermark=%lu bytes\r\n", (unsigned long)os_mem_free_get(),
               (unsigned long)os_mem_watermark_get());

        a = os_mem_alloc(128U);
        b = os_mem_alloc(64U);
        printf("[mem] allocated 128 + 64 bytes, free=%lu bytes\r\n", (unsigned long)os_mem_free_get());

        os_mem_free(a);
        os_mem_free(b);
        printf("[mem] freed both: free=%lu bytes, watermark=%lu bytes\r\n", (unsigned long)os_mem_free_get(),
               (unsigned long)os_mem_watermark_get());

        os_delay_ms(2000U);
    }
}
