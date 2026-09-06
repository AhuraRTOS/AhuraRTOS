/**
 * @file os_mem.h
 * @brief The kernel heap (os_mem.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_MEM_H
#define OS_MEM_H

#include "os_types.h"

/*
 * ***********************************************************************************************************
 * Kernel heap        - OS_CONFIG_ALLOC_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_ALLOC_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Allocate memory from the kernel heap (8-byte aligned; NULL when exhausted).
 */
void* os_mem_alloc(size_t size);

/******************************************************************************************************/
/**
 * @brief Return memory obtained from os_mem_alloc to the kernel heap (NULL is ignored).
 */
void os_mem_free(void *memory);

/******************************************************************************************************/
/**
 * @brief Get the number of bytes currently free in the kernel heap.
 */
size_t os_mem_free_get(void);

/******************************************************************************************************/
/**
 * @brief Get the smallest amount of free heap ever observed (worst case since boot).
 */
size_t os_mem_watermark_get(void);

#endif /* OS_CONFIG_ALLOC_ENABLE */

#endif /* OS_MEM_H */
