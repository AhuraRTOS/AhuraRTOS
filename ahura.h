/**
 * @file ahura.h
 * @brief Ahura kernel umbrella public API.
 *
 * Two parts. PART 1 is always available. PART 2 is one group per OS_CONFIG_ option, each behind a
 * single guard, so a disabled feature takes its whole API surface with it.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef AHURA_H
#define AHURA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* os_arch_port.h includes and validates the application's os_config.h
 * (copy template/os_config.h, see doc/integration.md "Configuration"). */
#include "os_arch_port.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Portability
 * ***********************************************************************************************************
*/

/* The compile-time assertion, spelled the way the language in use spells it: _Static_assert in
 * C11, static_assert in C++, which does not declare the C spelling at all. The extern "C" block
 * fixes the linkage, not the syntax. Both forms take the same two arguments. */
#ifdef __cplusplus
#define OS_STATIC_ASSERT(condition, message)    static_assert(condition, message)
#else
#define OS_STATIC_ASSERT(condition, message)    _Static_assert(condition, message)
#endif

/*
 * ***********************************************************************************************************
 * Kernel version
 * ***********************************************************************************************************
*/

/* MAJOR.MINOR.PATCH, semantic versioning: MAJOR for a breaking API change, MINOR for additions that
 * keep existing code compiling, PATCH for fixes that change no interface.
 *
 * Plain ints, deliberately: no U suffix, because OS_VERSION_STRING stringifies these very tokens and
 * a suffix would come out literally as "0U.0U.0U". They are only ever compared against small
 * constants, so nothing here needs the unsigned type. */
#define OS_VERSION_MAJOR                0
#define OS_VERSION_MINOR                0
#define OS_VERSION_PATCH                0

/* Two steps, and both are necessary. # freezes its argument before macro expansion, so a
 * single-step version of this would yield "OS_VERSION_MAJOR" instead of "0"; the outer macro exists
 * only to force the argument through one expansion first. */
#define OS_VERSION_STRINGIFY_(value)    #value
#define OS_VERSION_STRINGIFY(value)     OS_VERSION_STRINGIFY_(value)

/* "0.0.0" - a string literal, so it concatenates with adjacent literals:
 *     OS_LOG_INFO("Ahura " OS_VERSION_STRING " starting"); */
#define OS_VERSION_STRING               OS_VERSION_STRINGIFY(OS_VERSION_MAJOR) "." \
                                        OS_VERSION_STRINGIFY(OS_VERSION_MINOR) "." \
                                        OS_VERSION_STRINGIFY(OS_VERSION_PATCH)

/* One ordered integer, so application code can gate on a kernel version at compile time:
 *     #if (OS_VERSION >= OS_VERSION_MAKE(1, 2, 0))
 * Eight bits per field. Past 255 it is the scheme that wants revisiting, not the number. */
#define OS_VERSION_MAKE(major, minor, patch) \
    ((((major) & 0xFF) << 16) | (((minor) & 0xFF) << 8) | ((patch) & 0xFF))

#define OS_VERSION                      OS_VERSION_MAKE(OS_VERSION_MAJOR, \
                                                        OS_VERSION_MINOR, \
                                                        OS_VERSION_PATCH)


/*
 * ***********************************************************************************************************
 * PART 1 - ALWAYS AVAILABLE (no configuration option removes any of this)
 * ***********************************************************************************************************
*/


#include "kernel/os_types.h"
#include "kernel/os_list.h"
#include "kernel/os_kernel.h"
#include "kernel/os_task.h"
#include "kernel/os_critical.h"
#include "kernel/os_tick.h"
#include "kernel/os_delay.h"

/*
 * ***********************************************************************************************************
 * PART 2 - CONFIGURABLE (each group compiles away with its OS_CONFIG_ option)
 * ***********************************************************************************************************
 *
 * Same order as the option list in os_config.h, one guard per group.
*/


#include "kernel/os_mutex.h"
#include "kernel/os_sem.h"
#include "kernel/os_queue.h"
#include "kernel/os_msg.h"
#include "kernel/os_event.h"
#include "kernel/os_timer.h"
#include "kernel/os_notify.h"
#include "kernel/os_mem.h"
#include "kernel/os_atomic.h"
#include "kernel/os_log.h"

/*
 * ***********************************************************************************************************
 * Self-test          - OS_CONFIG_TEST_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TEST_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Kernel self-test suite entry point (see OS_CONFIG_TEST_* in os_config.h). os_init()
 *        creates and starts a task that calls this automatically, so link the AhuraRTOS/test
 *        library (CMake target "os_test") to supply it (see doc/testing.md "Self-test suite"). The
 *        kernel ships no stub, which is what lets a plain static-library link pull the suite in
 *        and turns "forgot to link it" into a link error. Not a "_cb" hook, same reasoning as
 *        os_main().
 */
void os_test(void);

/******************************************************************************************************/
/**
 * @brief The self-test suite body that has to run in INTERRUPT context, so the ISR-safe APIs are
 *        exercised from a real ISR rather than from a task pretending to be one.
 *
 * Call this from the SVC handler when the application owns that vector and the suite therefore does
 * not (OS_CONFIG_TEST_SVC_VECTOR set to 0). With the default of 1 the suite installs its own vector
 * and nothing here needs calling. See the option in test/os_test.c for why both routes exist.
 */
void os_test_isr_entry(void);

/**
 * @brief Entries into os_tick_handler() on the core that owns the time base, counted so the suite
 *        can prove a tickless window really SUPPRESSED the tick rather than merely arriving at the
 *        right answer.
 *
 * Compiled only with OS_CONFIG_TEST_ENABLE. Nothing in the kernel reads it, and no behaviour
 * depends on it - it exists because "the clock ended up correct" and "the tick stopped firing" are
 * different claims, and only the second one distinguishes a suppressed window from a plain WFI
 * that happened to be woken by the tick it was supposed to skip. Write to it freely; the suite
 * samples it around a window of known length and expects roughly one entry rather than one per
 * tick.
 */
extern __IO uint32_t os_test_tick_isr_entries;
#endif /* OS_CONFIG_TEST_ENABLE */


#ifdef __cplusplus
}

#endif

#endif /* AHURA_H */
