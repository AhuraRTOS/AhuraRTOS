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


#if (OS_CONFIG_TICKLESS_ENABLE == 1U) && \
    (OS_CONFIG_TICK_SOURCE == OS_CONFIG_TICK_SOURCE_SYSTICK)

#include "os_arch_tick_math.h"

#ifndef OS_ARCH_TICKLESS_REFERENCE_CLOCK
#define OS_ARCH_TICKLESS_REFERENCE_CLOCK 0
#endif

static uint32_t os_arch_tickless_saved_csr;
#if (OS_ARCH_TICKLESS_REFERENCE_CLOCK == 1)
static uint32_t os_arch_tickless_start_cvr;
static uint32_t os_arch_tickless_start_pending;
static uint64_t os_arch_tickless_start_reference;
static uint32_t os_arch_tickless_reference_hz;

/* A coherent phase/reference sample. Pending must not change between reads:
 * a tick latched while TICKINT is enabled is supplied by the ordinary ISR,
 * while a boundary crossed with TICKINT disabled is supplied by announce. */
static uint64_t os_arch_tickless_sample(uint32_t *cvr, uint32_t *pending)
{
    uint32_t before;
    uint32_t before_pending;
    uint64_t reference;
    do
    {
        before_pending = OS_ARCH_REG_ICSR & OS_ARCH_ICSR_PENDSTSET_MSK;
        before = OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK;
        reference = os_arch_reference_clock_get_cb();
        *cvr = OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK;
        *pending = OS_ARCH_REG_ICSR & OS_ARCH_ICSR_PENDSTSET_MSK;
    } while ((*cvr > before) || (*pending != before_pending));
    return reference;
}

#endif /* OS_ARCH_TICKLESS_REFERENCE_CLOCK */

static void os_arch_tickless_tick_silence(void)
{
    os_arch_tickless_saved_csr = OS_ARCH_REG_SYST_CSR;
#if (OS_ARCH_TICKLESS_REFERENCE_CLOCK == 1)
    os_arch_tickless_reference_hz = os_arch_tick_reference_clock_hz_cb();
    if (os_arch_tickless_reference_hz != 0U)
    {
        /* Capture before masking the source, so even a boundary during the
         * control-register write belongs to either the ISR or this window. */
        os_arch_tickless_start_reference =
            os_arch_tickless_sample(&os_arch_tickless_start_cvr,
                                    &os_arch_tickless_start_pending);
    }
#endif
    OS_ARCH_REG_SYST_CSR = os_arch_tickless_saved_csr & ~OS_ARCH_SYST_CSR_TICKINT_MSK;
}

static void os_arch_tickless_tick_restore(void)
{
    OS_ARCH_REG_SYST_CSR = os_arch_tickless_saved_csr;
}

#if (OS_ARCH_TICKLESS_REFERENCE_CLOCK == 1)
static uint32_t os_arch_tickless_elapsed_adjust(uint32_t elapsed)
{
    if (os_arch_tickless_reference_hz != 0U)
    {
        uint32_t cvr;
        uint32_t pending;
        uint64_t reference = os_arch_tickless_sample(&cvr, &pending);
        uint64_t cycles = ((reference - os_arch_tickless_start_reference) *
                           (uint64_t)os_arch_clock_hz_get()) / os_arch_tickless_reference_hz;
        uint32_t period = (OS_ARCH_REG_SYST_RVR & OS_ARCH_SYST_RVR_RELOAD_MSK) + 1U;
        elapsed = os_arch_tick_wraps(cycles, os_arch_tickless_start_cvr, cvr, period);
        /* One boundary may have latched during the entry/exit CSR writes.
         * It is already pending and will be counted by the ordinary ISR. */
        if ((os_arch_tickless_start_pending == 0U) && (pending != 0U) && (elapsed != 0U))
        {
            elapsed--;
        }
    }
    return elapsed;
}

#define OS_ARCH_TICKLESS_ELAPSED_ADJUST(n)    os_arch_tickless_elapsed_adjust(n)
#endif /* OS_ARCH_TICKLESS_REFERENCE_CLOCK */

#define OS_ARCH_TICKLESS_TICK_SILENCE()       os_arch_tickless_tick_silence()
#define OS_ARCH_TICKLESS_TICK_RESTORE()       os_arch_tickless_tick_restore()
#define OS_ARCH_TICKLESS_CYCLE_FROM_TICK      1

#endif /* Owned SysTick only: external-tick callbacks own all suppression. */

#include "../../common/os_arch_tickless.c"
