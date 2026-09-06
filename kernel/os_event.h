/**
 * @file os_event.h
 * @brief Event groups (os_event.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_EVENT_H
#define OS_EVENT_H

#include "os_types.h"

/*
 * ***********************************************************************************************************
 * Events             - OS_CONFIG_EVENT_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_EVENT_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Event object: 32 bits several tasks can wait on.
 */
typedef struct
{
    uint32_t  flags;
    os_list_t waiters; /**< Tasks blocked waiting for bits to match. */

} os_event_t;

/******************************************************************************************************/
/**
 * @brief Initialize an event object.
 */
os_err_t os_event_init(os_event_t *event);

/******************************************************************************************************/
/**
 * @brief Set event bits (ISR-safe).
 */
os_err_t os_event_set_bits(os_event_t *event, uint32_t bits);

/******************************************************************************************************/
/**
 * @brief Clear event bits (ISR-safe).
 */
os_err_t os_event_clear_bits(os_event_t *event, uint32_t bits);

/******************************************************************************************************/
/**
 * @brief Wait for event bits, waiting up to timeout_ms until they match. clear_on_exit true
 *        consumes the requested bits atomically with the match (no lost set between the
 *        wait returning and a separate manual clear).
 */
os_err_t os_event_wait_bits(os_event_t *event, uint32_t bits, bool wait_all, bool clear_on_exit, uint32_t *matched_bits, uint32_t timeout_ms);

#endif /* OS_CONFIG_EVENT_ENABLE */

#endif /* OS_EVENT_H */
