/**
 * @file os_notify.h
 * @brief Direct-to-task notifications (os_notify.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_NOTIFY_H
#define OS_NOTIFY_H

#include "os_types.h"

/*
 * ***********************************************************************************************************
 * Task notifications - OS_CONFIG_NOTIFY_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Deliver a value to a task's notification mailbox (overwrite: last write wins), waking
 *        it if it is currently blocked in os_notify_wait; ISR-safe. NULL means the calling task,
 *        which an ISR does not have and is refused for.
 */
os_err_t os_notify_give(os_task_t *task, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Wait for this task's notification mailbox, up to timeout_ms. Task-only. value_out may
 *        be NULL to take the wake-up and discard the value; it is consumed either way.
 */
os_err_t os_notify_wait(uint32_t timeout_ms, uint32_t *value_out);

#endif /* OS_CONFIG_NOTIFY_ENABLE */

#endif /* OS_NOTIFY_H */
