/** Execute the actual v8-M self-SysTick path against modeled MMIO registers.
 * The runner checks the first reload written at enable, and subsequent LOAD.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../arch/arm/cortex_m33/os_arch_port.c"

volatile uint32_t test_failure;
uint32_t SystemCoreClock = 1000000U;
uint32_t expected_reload;
#define CHECK(c) do { if (!(c)) { test_failure = __LINE__; __asm volatile("bkpt #0"); } } while (0)

static void setup(void)
{
    os_arch_tick_reload_cycles = 1000U;
    OS_ARCH_REG_ICSR = 0U;
    OS_ARCH_REG_SYST_RVR = 999U;
    OS_ARCH_REG_SYST_CSR = 7U;
    OS_ARCH_REG_SYST_CVR = 900U;
}
uint32_t test_port_early_phase(void)
{
    setup();
    CHECK(os_arch_tickless_self_open(10U) == 10U);
    OS_ARCH_REG_SYST_CVR = 9499U; /* 400 cycles elapsed from a 9900-cycle window */
    CHECK(os_arch_tickless_self_close(10U) == 0U);
    expected_reload = 499U; /* remaining 500 cycles, LOAD = period - 1 */
    CHECK(OS_ARCH_REG_SYST_RVR == 999U);
    os_arch_tickless_self_finish();
    return test_failure;
}
uint32_t test_port_pending_open(void)
{
    setup();
    OS_ARCH_REG_ICSR = OS_ARCH_ICSR_PENDSTSET_MSK;
    CHECK(os_arch_tickless_self_open(10U) == 0U);
    CHECK(OS_ARCH_REG_SYST_RVR == 999U && OS_ARCH_REG_SYST_CVR == 900U);
    CHECK(OS_ARCH_REG_SYST_CSR == 7U && !os_arch_sleep_mask_held);
    expected_reload = 999U;
    return test_failure;
}
uint32_t test_port_zero_initial(void)
{
    setup();
    CHECK(os_arch_tickless_self_open(10U) == 10U);
    OS_ARCH_REG_SYST_CVR = 0U; /* not loaded yet, no COUNTFLAG or pending event */
    CHECK(os_arch_tickless_self_close(10U) == 0U);
    expected_reload = 899U;
    os_arch_tickless_self_finish();
    return test_failure;
}
uint32_t test_port_full_window(void)
{
    setup();
    CHECK(os_arch_tickless_self_open(10U) == 10U);
    OS_ARCH_REG_SYST_CVR = 0U;
    OS_ARCH_REG_ICSR = OS_ARCH_ICSR_PENDSTSET_MSK;
    CHECK(os_arch_tickless_self_close(10U) == 9U);
    expected_reload = 999U;
    CHECK((OS_ARCH_REG_ICSR & OS_ARCH_ICSR_PENDSTSET_MSK) != 0U);
    os_arch_tickless_self_finish();
    return test_failure;
}

extern uint64_t test_reference;

uint32_t test_port_light_partial_windows(void)
{
    uint32_t total = 0U;
    setup();
    for (uint32_t window = 0U; window < 100U; window++)
    {
        test_reference = (uint64_t)window * 1000U + 100U;
        OS_ARCH_REG_SYST_CVR = 900U;
        os_arch_tickless_tick_silence();
        test_reference += 800U;
        OS_ARCH_REG_SYST_CVR = 100U;
        os_arch_tickless_tick_restore();
        total += os_arch_tickless_elapsed_adjust(1U); /* deliberately wrong old sleep credit */
    }
    CHECK(total == 0U); /* 100 ordinary boundaries account for all 100 ms */
    expected_reload = 999U;
    return test_failure;
}

uint32_t test_port_light_boundaries(void)
{
    setup();
    test_reference = 100U;
    os_arch_tickless_tick_silence();
    test_reference = 3100U;
    OS_ARCH_REG_SYST_CVR = 900U;
    os_arch_tickless_tick_restore();
    CHECK(os_arch_tickless_elapsed_adjust(0U) == 3U);

    test_reference = 4100U;
    OS_ARCH_REG_SYST_CVR = 900U;
    os_arch_tickless_tick_silence();
    test_reference = 5000U;
    OS_ARCH_REG_SYST_CVR = 0U;
    os_arch_tickless_tick_restore();
    CHECK(os_arch_tickless_elapsed_adjust(0U) == 1U);

    test_reference = 6100U;
    OS_ARCH_REG_SYST_CVR = 900U;
    os_arch_tickless_tick_silence();
    test_reference = 7000U;
    OS_ARCH_REG_SYST_CVR = 0U;
    os_arch_tickless_tick_restore();
    OS_ARCH_REG_ICSR = OS_ARCH_ICSR_PENDSTSET_MSK;
    CHECK(os_arch_tickless_elapsed_adjust(0U) == 0U); /* pending ISR owns this boundary */
    expected_reload = 999U;
    return test_failure;
}

uint32_t test_port_shared_counter(void)
{
    /* In SMP a present DWT must not override the common SoC epoch. Its MMIO
     * is deliberately unmapped by the runner for this case. */
    SystemCoreClock = 100000000U;
    os_arch_dwt_available = true;
    test_reference = 12345U;
    CHECK(os_arch_delay_counter_hz_get() == 1000000U);
    CHECK(os_arch_delay_counter_get() == 12345U);
    return test_failure;
}
