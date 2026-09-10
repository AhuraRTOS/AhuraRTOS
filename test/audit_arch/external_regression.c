/**
 * @file external_regression.c
 * @brief External-tick regression: the real port path with SysTick MMIO left unmapped.
 *
 * Proves the external-tick selection never touches SysTick. Any access to the suppressed
 * register block faults the runner rather than passing quietly.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */
/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "ahura.h"
volatile uint32_t test_failure;
static uint32_t prepared;

/*
 * ***********************************************************************************************************
 * Function implementations
 * ***********************************************************************************************************
*/

uint32_t os_arch_tick_suppress_max_cb(void)
{
    return 100U;
}
void os_arch_tick_suppress_cb(uint32_t ticks)
{
    prepared = ticks;
}
uint32_t os_arch_tick_resume_cb(void)
{
    return 7U;
}
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
