/**
 * @file os_mutex.h
 * @brief Mutex with priority inheritance (os_mutex.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_MUTEX_H
#define OS_MUTEX_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Mutex              - OS_CONFIG_MUTEX_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MUTEX_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Mutex object.
 */
typedef struct
{
    bool           locked;     /**< True while held.                                  */
    uint32_t       owner_id;   /**< Task id of the holder, 0 when free/unknown owner. */
    os_list_t      waiters;    /**< Tasks blocked waiting for the mutex.              */
    os_list_node_t owner_node; /**< Links into the owner's owned-mutex list (priority inheritance). */

} os_mutex_t;

/******************************************************************************************************/
/**
 * @brief Initialize a mutex object.
 */
os_err_t os_mutex_init(os_mutex_t *mutex);

/******************************************************************************************************/
/**
 * @brief Acquire a mutex, waiting up to timeout_ms when contended.
 */
os_err_t os_mutex_lock(os_mutex_t *mutex, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Release a mutex object (only the owner may unlock).
 */
os_err_t os_mutex_unlock(os_mutex_t *mutex);

#endif /* OS_CONFIG_MUTEX_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_MUTEX_H */
