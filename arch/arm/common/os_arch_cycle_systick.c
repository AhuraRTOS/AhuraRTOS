/**
 * @file os_arch_cycle_systick.c
 * @brief Cycle counter synthesized from SysTick, for cores and devices where DWT CYCCNT is not
 *        available.
 *
 * Textually included by the shared port implementations (os_arch_port_v6m.c, _v7m.c, _v8m.c), the
 * same way each variant's os_arch_port.c includes those - it is not a separate compilation unit and
 * must never be added to a build as one.
 *
 * Two ports need it for different reasons:
 *
 *   v6m   ARMv6-M has no DWT at all, so this is the only cycle counter that exists there.
 *   v7m   DWT is OPTIONAL on Cortex-M3/M4/M7 and on ARMv8-M, and even where the unit is present
 *   v8m   CYCCNT itself may not be (DWT_CTRL.NOCYCCNT), may be gated behind debug power, or may
 *         be locked. Those ports use CYCCNT when it really counts and fall back to here when it
 *         does not - see os_arch_cycle_count_get() in each.
 *
 * What it provides is a counter that advances monotonically in CPU cycles, which is exactly what
 * its callers need: os_delay_us() and os_delay_ms()'s busy-wait path measure a SHORT interval as a
 * difference between two reads. It is not absolute time. Wraps of the SysTick down-counter are only
 * observed while this function is being polled, so an interval longer than one SysTick period that
 * is not polled throughout will be undercounted; busy-waits poll continuously and are unaffected.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Cycles credited per call while SysTick is not running - before os_tick_init(), or for the whole
 * run with OS_CONFIG_TICK_SOURCE_EXTERNAL, where SysTick may never be started at all. Deliberately
 * well below the real cost of one call so a busy-wait driven by it only ever comes out LONGER than
 * asked for. It exists so those waits terminate rather than being accurate. */
#define OS_ARCH_CYCLE_FALLBACK_STEP          8U

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/* Accumulated whole SysTick periods, plus the software-only count used while SysTick is stopped. */
static uint32_t os_arch_cycle_accum    = 0U;
static uint32_t os_arch_cycle_fallback = 0U;

/*
 * ***********************************************************************************************************
 * Function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Read the SysTick-derived cycle count. See the file header for what it does and does not
 *        guarantee.
 *
 * @return uint32_t  Current cycle count.
 */
static uint32_t os_arch_cycle_systick_get(void)
{
    uint32_t primask = os_arch_primask_get();
    uint32_t reload;
    uint32_t csr;
    uint32_t value;

    /* The accumulator is read-modify-written against a down-counter that keeps running, so the
     * whole sample has to be indivisible - a tick landing in the middle would double-count or
     * skip a wrap. PRIMASK rather than the kernel mask: this must also be correct when called
     * from an interrupt above OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY. */
    OS_ARCH_IRQ_DISABLE();

    reload = OS_ARCH_REG_SYST_RVR & OS_ARCH_SYST_RVR_RELOAD_MSK;

    /* Single CSR read: it clears COUNTFLAG, so it must be sampled once. */
    csr = OS_ARCH_REG_SYST_CSR;

    if (((csr & OS_ARCH_SYST_CSR_ENABLE_MSK) == 0U) || (reload == 0U))
    {
        os_arch_cycle_fallback += OS_ARCH_CYCLE_FALLBACK_STEP;
        value = os_arch_cycle_accum + os_arch_cycle_fallback;
    }
    else
    {
        if ((csr & OS_ARCH_SYST_CSR_COUNTFLAG_MSK) != 0U)
        {
            os_arch_cycle_accum += (reload + 1U);
        }

        value = os_arch_cycle_accum + (reload - (OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK));
    }

    if (primask == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }

    return value;
}

/******************************************************************************************************/
/**
 * @brief Reset the synthesized counter's state. Called from os_arch_init().
 *
 * @return None.
 */
static void os_arch_cycle_systick_reset(void)
{
    os_arch_cycle_accum    = 0U;
    os_arch_cycle_fallback = 0U;
}
