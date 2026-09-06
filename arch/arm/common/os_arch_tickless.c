/**
 * @file os_arch_tickless.c
 * @brief Cortex-M adapter for the shared tickless-idle contract.
 *
 * Textually included by all three Arm ports (os_arch_port_v6m/v7m/v8m.c). Not a translation unit
 * - no include guard, statics the includer uses - so it opens with a #error unless the wrapper
 * that includes it has claimed OS_ARCH_PORT_TRANSLATION_UNIT.
 *
 * The contract is in arch/common/os_arch_tickless.c; this is its Cortex-M half. Two things are
 * Arm-specific: the tick is silenced by clearing SysTick's TICKINT (the COUNTER keeps running, or
 * the tick grid loses its phase), and the cycle counter may be SYNTHESIZED from that interrupt, so
 * the window has to be credited back or os_delay_us() runs short by its whole length.
 *
 * os_arch_port_v8m.c additionally sets OS_ARCH_TICKLESS_SELF_SUPPRESS and supplies the
 * os_arch_tickless_self_* functions; v6m and v7m include this file as-is.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_ARCH_PORT_TRANSLATION_UNIT
#error "os_arch_tickless.c is a textual include, not a translation unit. Compile arch/<family>/<core>/os_arch_port.c instead - it defines OS_ARCH_PORT_TRANSLATION_UNIT and includes this. See doc/installation.md."
#endif


#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/** SysTick CSR as it stood before a window masked the tick interrupt. */
static uint32_t os_arch_tickless_saved_csr = 0U;

/******************************************************************************************************/
/**
 * @brief Silence the tick interrupt for a window, leaving the counter running.
 *
 * Reading CSR clears COUNTFLAG; nothing here depends on it (see os_arch_cycle_systick.c).
 *
 * @return None.
 */
static void os_arch_tickless_tick_silence(void)
{
    os_arch_tickless_saved_csr = OS_ARCH_REG_SYST_CSR;

    OS_ARCH_REG_SYST_CSR = os_arch_tickless_saved_csr & ~OS_ARCH_SYST_CSR_TICKINT_MSK;
}

/******************************************************************************************************/
/**
 * @brief Put SysTick's control register back exactly as the window found it.
 *
 * @return None.
 */
static void os_arch_tickless_tick_restore(void)
{
    OS_ARCH_REG_SYST_CSR = os_arch_tickless_saved_csr;
}

#define OS_ARCH_TICKLESS_TICK_SILENCE()   os_arch_tickless_tick_silence()
#define OS_ARCH_TICKLESS_TICK_RESTORE()   os_arch_tickless_tick_restore()

/** Cortex-M's cycle counter may be the one synthesized from SysTick, so a window has to be
 *  bracketed. See the file header. */
#define OS_ARCH_TICKLESS_CYCLE_FROM_TICK  1

#endif /* OS_CONFIG_TICKLESS_ENABLE */

#include "../../common/os_arch_tickless.c"
