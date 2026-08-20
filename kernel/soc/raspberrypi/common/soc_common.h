/**
 * @file soc_common.h
 * @brief Internal interface between the shared Raspberry Pi SoC code and the per-chip packages.
 *
 * NOT a public header: an application never includes this. It exists so soc_common.c and a
 * chip package's soc_cb.c agree on the options, the chip facts and the one function that differs
 * between them.
 *
 * The split is by what the silicon actually decides. Nearly everything the kernel needs from an
 * RP2 chip is identical across the family - the core id, the spinlock, the CPU clock, the SysTick
 * vector, booting core 1 - because it is all SIO or SDK, and SIO is the same block on every one.
 * What differs is precisely one thing: how a core interrupts the other one. The RP2040 has an
 * inter-core FIFO; the RP2350 adds purpose-built doorbells.
 *
 * So the shared file owns everything else and calls soc_ipi_arm(), which each chip package
 * implements against its own hardware, alongside its own os_arch_core_ipi_request_cb().
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef SOC_COMMON_H
#define SOC_COMMON_H

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "ahura.h"

#include "hardware/sync.h"
#include "pico/platform.h"

/*
 * The application's SoC configuration: copy this package's template/soc_config.h into the project
 * beside os_config.h. It is found on the include path OS_CONFIG_DIR already provides.
 *
 * REQUIRED, and so is every option in it - the same terms as os_config.h. The file holds ONLY the
 * values a user may need to decide; there are deliberately no built-in defaults, so a missing
 * option is rejected at compile time rather than silently read as 0, because 0 is a real setting
 * for most of these and a silently wrong one. Whatever this package can decide for itself is not
 * in the file at all.
 */
#if defined(__has_include)
#if !__has_include("soc_config.h")
#error "No soc_config.h found: copy this SoC package's template/soc_config.h into your project beside os_config.h, and make sure OS_CONFIG_DIR points at that directory."
#endif
#endif

#include "soc_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Reject an incomplete configuration rather than quietly filling the gap. A missing option would
 * read as 0 in an #if, which here would turn the SysTick vector off - a board that never ticks -
 * or select spinlock id 0, which the SDK uses for something else entirely. Neither reports
 * itself. Start from template/soc_config.h, which lists every option this package needs.
 *
 * SOC_CONFIG_IPI_DOORBELL is not checked here: doorbells are RP2350-only hardware, so that option
 * belongs to the chip package that has them and is checked there. */
#if !defined(SOC_CONFIG_SPINLOCK_ID) || !defined(SOC_CONFIG_SYSTICK_VECTOR) ||                        \
    !defined(SOC_CONFIG_CLOCK_AUTO_UPDATE) || !defined(SOC_CONFIG_FAULT_REPORT)
#error "soc_config.h is incomplete: it must define every option listed in this package's template/soc_config.h."
#endif

/* How many cores these chips HAVE. Not a configuration value and deliberately not in
 * soc_config.h: it is a property of the silicon, and every RP2 chip is dual-core.
 *
 * OS_CONFIG_CORE_COUNT is the separate question of how many the kernel should SCHEDULE on, which
 * is an application decision - running single-core on a dual-core chip is entirely reasonable,
 * and is the right default while SMP is experimental. What the package can do is stop the choice
 * being impossible, which is otherwise a puzzle at link time or, worse, at run time. */
#define SOC_CORE_COUNT                  2U

#if (OS_CONFIG_CORE_COUNT > SOC_CORE_COUNT)
#error "OS_CONFIG_CORE_COUNT is higher than this chip has cores: every RP2040/RP2350/RP2354 is dual-core, so the maximum is 2."
#endif

/*
 * ***********************************************************************************************************
 * Public function prototypes
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)

/******************************************************************************************************/
/**
 * @brief Enable the inter-core interrupt on the calling core. Implemented by the CHIP package,
 *        called by the shared code - once from os_arch_soc_init_cb() on core 0, and once from
 *        core 1's entry point.
 *
 * The one function that genuinely differs between RP2 chips: the RP2040 arms its SIO FIFO IRQ,
 * the RP2350 arms a claimed doorbell. Both end up pending PendSV from their handler, which is why
 * nothing above this line has to know which happened.
 */
void soc_ipi_arm(void);

#endif /* OS_CONFIG_CORE_COUNT > 1U */

#ifdef __cplusplus
}
#endif

#endif /* SOC_COMMON_H */
