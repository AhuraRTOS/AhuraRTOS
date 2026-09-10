/** Real port external-tick path; SysTick MMIO is intentionally unmapped.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ahura.h"
volatile uint32_t test_failure;
static uint32_t prepared;
uint32_t os_arch_tick_suppress_max_cb(void) { return 100U; }
void os_arch_tick_suppress_cb(uint32_t ticks) { prepared = ticks; }
uint32_t os_arch_tick_resume_cb(void) { return 7U; }
uint32_t test_external_ownership(void)
{
    os_arch_sleep_prepare(20U);
    if ((prepared != 20U) || (os_arch_elapsed_ticks_get() != 7U))
    {
        test_failure = __LINE__;
    }
    os_arch_sleep_finish();
    return test_failure;
}
