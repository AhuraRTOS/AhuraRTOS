/**
 * @file os_log.h
 * @brief Buffered logging (os_log.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_LOG_H
#define OS_LOG_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Logging            - OS_CONFIG_LOG_ENABLE
 * ***********************************************************************************************************
*/

/** Severity values for OS_CONFIG_LOG_LEVEL, outside the guard below because an os_config.h
 *  selects one by name even though it is read before this header: a macro body is only expanded
 *  where it is used, and every comparison against these lives below. They are compared
 *  numerically in #if, so the increasing order is part of the contract, not just a convention. */
#define OS_LOG_LEVEL_NONE       0U
#define OS_LOG_LEVEL_ERROR      1U
#define OS_LOG_LEVEL_WARN       2U
#define OS_LOG_LEVEL_INFO       3U
#define OS_LOG_LEVEL_DEBUG      4U

/** Buffered log calls (see OS_CONFIG_LOG_ENABLE / OS_CONFIG_LOG_LEVEL). Each
 *  formats like printf, returns immediately, and is safe from tasks and ISRs.
 *  Calls above the configured level expand to nothing, arguments included, so
 *  a disabled OS_LOG_DEBUG costs neither code nor the cost of its arguments. */
#if (OS_CONFIG_LOG_ENABLE == 1U)

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_ERROR)
#define OS_LOG_ERROR(...)       os_log_write(OS_LOG_LEVEL_ERROR, __VA_ARGS__)
#else
#define OS_LOG_ERROR(...)       ((void)0)
#endif

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_WARN)
#define OS_LOG_WARN(...)        os_log_write(OS_LOG_LEVEL_WARN, __VA_ARGS__)
#else
#define OS_LOG_WARN(...)        ((void)0)
#endif

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_INFO)
#define OS_LOG_INFO(...)        os_log_write(OS_LOG_LEVEL_INFO, __VA_ARGS__)
#else
#define OS_LOG_INFO(...)        ((void)0)
#endif

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_DEBUG)
#define OS_LOG_DEBUG(...)       os_log_write(OS_LOG_LEVEL_DEBUG, __VA_ARGS__)
#else
#define OS_LOG_DEBUG(...)       ((void)0)
#endif

/******************************************************************************************************/
/**
 * @brief Format a log line and queue it for transmission. Prefer the OS_LOG_ERROR/WARN/INFO/
 *        DEBUG macros, which also drop the call entirely below OS_CONFIG_LOG_LEVEL.
 *
 * Safe from tasks and ISRs, and never blocks: the line is formatted, copied into the ring
 * buffer, and the caller returns. A line that does not fit is dropped whole and counted
 * (os_log_dropped_get), never written in part.
 */
void os_log_write(uint32_t level, const char *fmt, ...);

/******************************************************************************************************/
/**
 * @brief Number of log lines dropped so far because the buffer was full. Also reported into
 *        the log itself once space frees up, so this is only needed for programmatic checks.
 */
uint32_t os_log_dropped_get(void);

/******************************************************************************************************/
/**
 * @brief Application hook that transmits finished log bytes; called from the kernel log task,
 *        never from an ISR or a critical section, so it may block or start a DMA transfer.
 *        REQUIRED when OS_CONFIG_LOG_ENABLE is 1: the kernel ships no default, so a log with
 *        nowhere to go is a link error rather than silence.
 *
 * @param[in] data    Bytes to transmit; valid only for the duration of the call.
 * @param[in] length  Number of bytes.
 */
void os_log_output_cb(const uint8_t *data, size_t length);

#else /* OS_CONFIG_LOG_ENABLE == 0U */

#define OS_LOG_ERROR(...)       ((void)0)
#define OS_LOG_WARN(...)        ((void)0)
#define OS_LOG_INFO(...)        ((void)0)
#define OS_LOG_DEBUG(...)       ((void)0)

#endif /* OS_CONFIG_LOG_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_LOG_H */
