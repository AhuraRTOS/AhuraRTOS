/**
 * @file regression.c
 * @brief Kernel timing, critical-section and delay-source tests against a deterministic port.
 *
 * Compiles the real os_tick.c, os_critical.c and os_delay.c, so a failure names kernel code
 * rather than a model of it.
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

#include "../../kernel/os_tick.c"
#include "../../kernel/os_critical.c"
#include "../../kernel/os_delay.c"
#include "../../arch/arm/common/os_arch_tick_math.h"

volatile uint32_t test_failure;
static uint32_t test_core;
static uint32_t test_masks[2];
static uint32_t test_ipis;
static uint32_t test_updated;
static uint32_t test_clock;
static uint32_t test_hz = 1000000U;
static uint32_t test_migrate;
static uint32_t test_lock_checks;
static bool test_locked;
static bool test_reconcile;
static bool test_idle;
static bool test_fault_expected;
static bool test_prepare_declines;
static bool test_prepared;
static uint32_t test_prepare_calls;
static uint32_t test_finish_calls;
static uint32_t test_sleep_calls;
static uint32_t test_next_deadline = 100U;
static uint32_t test_prepare_deadline;
static uint32_t test_sleep_phase;
__IO bool os_kernel_running;
__IO uint32_t os_kernel_lock_count[OS_CONFIG_CORE_COUNT];

#define CHECK(condition)                                    \
    do                                                      \
    {                                                       \
        if (!(condition))                                   \
        {                                                   \
            test_failure = __LINE__;                        \
            __asm volatile("bkpt #0");                      \
        }                                                   \
    } while (0)

/*
 * ***********************************************************************************************************
 * Function implementations
 * ***********************************************************************************************************
*/

uint32_t os_arch_kernel_mask_save(void)
{
    if (test_migrate && !test_masks[test_core])
    {
        test_core = 1U;
        test_migrate = 0U;
    }
    uint32_t old = test_masks[test_core];
    test_masks[test_core] = 1U;
    return old;
}
void os_arch_kernel_mask_restore(uint32_t mask)
{
    test_masks[test_core] = mask;
}
uint32_t os_arch_core_id_get(void)
{
    return test_core;
}
bool os_arch_in_isr(void)
{
    return true;
}
void os_arch_isr_priority_check(void)
{
     
}
void os_arch_tick_init(void)
{
     
}
void os_arch_cycle_tick(void)
{
     
}
bool os_kernel_is_running(void)
{
    return false;
}
bool os_task_reschedule_possible(void)
{
    return false;
}
void os_task_tick_update(uint32_t elapsed)
{
    test_updated += elapsed;
    if (test_prepared)
    {
        CHECK(test_sleep_phase == 5U);
        test_sleep_phase = 6U;
    }
}
void os_task_slice_tick(uint32_t elapsed)
{
    (void)elapsed;
}
void os_task_sleep_ticks(uint32_t ticks)
{
    (void)ticks; CHECK(false);
}
bool os_task_current_is_idle(void)
{
    /* The previous writer did not take the lock: this assertion detects A17. */
    CHECK(test_locked);
    test_lock_checks++;
    return test_idle;
}
uint32_t os_task_next_delay_ticks_get(void)
{
    return test_next_deadline;
}
uint32_t os_arch_max_suppressed_ticks_get(void)
{
    return 100U;
}
uint32_t os_arch_min_suppressed_ticks_get(void)
{
    return 2U;
}
uint32_t os_arch_elapsed_ticks_get(void)
{
    CHECK(test_sleep_phase == 3U);
    test_sleep_phase = 4U;
    return 50U;
}
void os_arch_sleep_finish(void)
{
    CHECK(test_sleep_phase == 6U);
    test_sleep_phase = 7U;
}
void os_tickless_pre_sleep_cb(void)
{
    CHECK(test_sleep_phase == 1U);
    test_sleep_phase = 2U;
}
void os_tickless_post_sleep_cb(void)
{
    CHECK(test_sleep_phase == 4U);
    test_sleep_phase = 5U;
}
bool os_arch_soc_sleep_prepare_cb(void)
{
    CHECK(!test_locked && !os_tickless_window_open && test_masks[test_core] != 0U);
    test_prepare_calls++;
    if (test_prepare_declines)
    {
        return false;
    }
    test_prepared = true;
    test_sleep_phase = 1U;
    if (test_prepare_deadline != 0U)
    {
        test_next_deadline = test_prepare_deadline;
    }
    return true;
}
void os_arch_soc_sleep_finish_cb(void)
{
    CHECK(test_prepared && !test_locked && !os_tickless_window_open);
    CHECK(test_masks[test_core] != 0U);
    CHECK(test_sleep_phase == ((test_sleep_calls != 0U) ? 7U : 1U));
    test_finish_calls++;
    test_prepared = false;
}
void audit_sleep(uint32_t ticks)
{
    CHECK(test_core == 0U && !test_locked && ticks == test_next_deadline);
    CHECK(test_prepared && os_tickless_window_open && test_sleep_phase == 2U);
    test_sleep_phase = 3U;
    test_sleep_calls++;
}
void os_arch_core_ipi_request_cb(uint32_t core)
{
    CHECK(test_locked && test_core == 1U && core == 0U);
    test_ipis++;
}
void os_arch_spinlock_acquire(os_arch_spinlock_t *lock)
{
    (void)lock;
    CHECK(!test_locked);
    test_locked = true;
}
void os_arch_spinlock_release(os_arch_spinlock_t *lock)
{
    (void)lock;
    CHECK(test_locked);
    test_locked = false;
    if (test_reconcile && test_core == 1U && test_ipis != 0U)
    {
        /* Controlled peer execution after the remote entrant drops the lock.
         * This is the IPI wakeup interleaving, not two emulated CPUs. */
        test_reconcile = false;
        test_core = 0U;
        test_masks[0] = 1U;
        os_tick_announce(50U);
        os_critical_enter();
        os_tickless_window_open = false;
        os_critical_exit();
        test_core = 1U;
    }
}
uint32_t os_arch_delay_counter_hz_get(void)
{
    return test_hz;
}
uint32_t os_arch_delay_counter_get(void)
{
    return test_clock++;
}
void os_arch_config_fault_trap(void)
{
    CHECK(test_fault_expected);
    __asm volatile("bkpt #0");
}

uint32_t test_remote_time_origin(void)
{
    test_core = 1U;
    os_tick_count = 100U;
    os_tickless_window_open = true;
    test_reconcile = true;
    uint32_t start = os_tick_get();
    CHECK(start == 150U && test_updated == 50U && test_ipis == 1U);
    CHECK(!test_locked && !os_tickless_window_open);
    CHECK(os_internal_wait_remaining(start, 10U) == 10U);
    return test_failure;
}

uint32_t test_raw_lock_time_origin(void)
{
    test_core = 1U;
    os_tick_count = 100U;
    os_tickless_window_open = true;
    test_reconcile = true;
    os_critical_multicore_lock();
    CHECK(test_locked && os_tick_count == 150U && test_ipis == 1U);
    os_critical_multicore_unlock();
    return test_failure;
}

uint32_t test_tickless_owner_migration(void)
{
    test_core = 0U;
    test_migrate = 1U;
    os_tick_count = 10U;
    os_tickless_idle_process();
    CHECK(test_core == 1U && os_tick_count == 10U);
    CHECK(!os_tickless_window_open && test_masks[1] == 0U);
    return test_failure;
}

uint32_t test_tickless_owner_window(void)
{
    test_core = 0U;
    os_tick_count = 10U;
    os_tickless_idle_process();
    CHECK(os_tick_count == 60U && test_updated == 50U);
    CHECK(!os_tickless_window_open && test_masks[0] == 0U);
    CHECK(test_prepare_calls == 1U && test_finish_calls == 1U && test_sleep_calls == 1U);
    return test_failure;
}

uint32_t test_sleep_prepare_declines(void)
{
    os_tick_count = 10U;
    test_prepare_declines = true;
    os_tickless_idle_process();
    CHECK(test_prepare_calls == 1U && test_finish_calls == 0U && test_sleep_calls == 0U);
    CHECK(os_tick_count == 10U && test_updated == 0U);
    CHECK(!os_tickless_window_open && !test_locked && test_masks[0] == 0U);
    return test_failure;
}

uint32_t test_sleep_prepare_short_deadline(void)
{
    os_tick_count = 10U;
    /* A peer posts an imminent timeout before it acknowledges parking. */
    test_prepare_deadline = 1U;
    os_tickless_idle_process();
    CHECK(test_prepare_calls == 1U && test_finish_calls == 1U && test_sleep_calls == 0U);
    CHECK(os_tick_count == 10U && test_updated == 0U && !test_prepared);
    CHECK(!os_tickless_window_open && !test_locked && test_masks[0] == 0U);
    return test_failure;
}

uint32_t test_sleep_prepare_replans_deadline(void)
{
    os_tick_count = 10U;
    test_prepare_deadline = 75U;
    os_tickless_idle_process();
    CHECK(test_prepare_calls == 1U && test_finish_calls == 1U && test_sleep_calls == 1U);
    CHECK(os_tick_count == 60U && test_updated == 50U && !test_prepared);
    CHECK(!os_tickless_window_open && !test_locked && test_masks[0] == 0U);
    return test_failure;
}

uint32_t test_usage_writer_lock(void)
{
    test_idle = false;
    os_tick_handler();
    os_tick_handler();
    test_idle = true;
    os_tick_handler();
    test_core = 1U;
    CHECK(os_cpu_usage_get() == 66U && test_lock_checks == 3U);
    CHECK(os_cpu_usage_get() == 0U);
    test_core = 0U;
    os_tick_announce(10U);
    test_core = 1U;
    CHECK(os_cpu_usage_get() == 0U);
    return test_failure;
}

uint32_t test_masked_busy_wait(void)
{
    os_critical_enter();
    os_critical_enter();
    os_delay_ms(3U);
    CHECK(test_clock >= 3000U && test_clock < 3010U && os_tick_count == 0U);
    CHECK(test_masks[0] == 1U && test_locked);
    os_critical_exit();
    os_critical_exit();
    CHECK(test_masks[0] == 0U && !test_locked);
    return test_failure;
}

uint32_t test_missing_busy_wait_counter(void)
{
    test_hz = 0U;
    test_fault_expected = true;
    os_delay_us(5U);
    CHECK(false);
    return test_failure;
}

uint32_t test_tick_phase_arithmetic(void)
{
    uint32_t period = 1000U;
    for (uint32_t phase = 0U; phase < period; phase += 7U)
    {
        uint32_t start = (phase == 0U) ? 0U : period - phase;
        for (uint32_t elapsed = 0U; elapsed < 4000U; elapsed += 13U)
        {
            uint32_t end_phase = (phase + elapsed) % period;
            uint32_t end = (end_phase == 0U) ? 0U : period - end_phase;
            uint32_t expected = (phase + elapsed) / period;
            CHECK(os_arch_tick_wraps(elapsed, start, end, period) == expected);
            CHECK(os_arch_tick_wraps(elapsed + 99U, start, end, period) == expected);
            CHECK(os_arch_tick_remaining(elapsed, period - phase, period) == period - end_phase);
        }
    }
    /* Repeated 0.1..0.9 period windows cross no boundary. All ordinary
     * boundaries remain ISR credits: no independent +80 ticks in 100 ms. */
    CHECK(os_arch_tick_wraps(800U, 900U, 100U, 1000U) == 0U);
    CHECK(os_arch_tick_remaining(400U, 900U, 1000U) == 500U);
    return test_failure;
}
