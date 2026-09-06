/**
 * @file os_sem.h
 * @brief Counting and binary semaphores (os_sem.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_SEM_H
#define OS_SEM_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Semaphore          - OS_CONFIG_SEM_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_SEM_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Semaphore object.
 */
typedef struct
{
    uint32_t  count;
    uint32_t  max_count;
    os_list_t waiters; /**< Tasks blocked waiting for a token. */

} os_sem_t;

/******************************************************************************************************/
/**
 * @brief Initialize a semaphore object.
 */
os_err_t os_sem_init(os_sem_t *semaphore, uint32_t initial_count, uint32_t max_count);

/******************************************************************************************************/
/**
 * @brief Give one token to semaphore (ISR-safe, never blocks).
 */
os_err_t os_sem_give(os_sem_t *semaphore);

/******************************************************************************************************/
/**
 * @brief Take one token from semaphore, waiting up to timeout_ms when empty.
 */
os_err_t os_sem_take(os_sem_t *semaphore, uint32_t timeout_ms);

#endif /* OS_CONFIG_SEM_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_SEM_H */
