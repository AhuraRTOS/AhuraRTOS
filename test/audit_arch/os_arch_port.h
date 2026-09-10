/**
 * @file os_arch_port.h
 * @brief Deterministic test port; only used by the standalone audit regression runner.
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */
#ifndef OS_ARCH_PORT_H
#define OS_ARCH_PORT_H

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../template/os_config.h"

#undef OS_CONFIG_CORE_COUNT
#define OS_CONFIG_CORE_COUNT 2U
#undef OS_CONFIG_TICKLESS_ENABLE
#define OS_CONFIG_TICKLESS_ENABLE 1U
#undef OS_CONFIG_TIMER_ENABLE
#define OS_CONFIG_TIMER_ENABLE 0U
#undef OS_CONFIG_LOG_ENABLE
#define OS_CONFIG_LOG_ENABLE 0U
#undef OS_CONFIG_TEST_ENABLE
#define OS_CONFIG_TEST_ENABLE 0U
#undef OS_CONFIG_ASSERT_ENABLE
#define OS_CONFIG_ASSERT_ENABLE 0U
#undef OS_CONFIG_STACK_CHECK_ENABLE
#define OS_CONFIG_STACK_CHECK_ENABLE 0U
#undef OS_CONFIG_STACK_WATERMARK_ENABLE
#define OS_CONFIG_STACK_WATERMARK_ENABLE 0U
#undef OS_CONFIG_TRUSTZONE
#define OS_CONFIG_TRUSTZONE 0U
#define OS_CONFIG_TRUSTZONE_NON_SECURE 2U
#define OS_ARCH_STACK_ALIGNMENT_BYTES 8U
#define __IO volatile
#define OS_INLINE static inline
#define OS_FORCE_INLINE static inline __attribute__((always_inline))
#define OS_WEAK __attribute__((weak))
#define OS_ARCH_IDLE() ((void)0)
#define OS_ARCH_CONTEXT_SWITCH_REQUEST() ((void)0)

uint32_t os_arch_kernel_mask_save(void);
void os_arch_kernel_mask_restore(uint32_t mask);
uint32_t os_arch_core_id_get(void);
bool os_arch_in_isr(void);
uint32_t os_arch_highest_bit_get(uint32_t bits);
uint32_t os_arch_lowest_bit_get(uint32_t bits);
uint32_t *os_arch_task_stack_initialize(uint8_t *stack, size_t bytes,
                                      void (*entry)(void *), void *context);
void os_arch_init(void);
void os_arch_tick_init(void);
void os_arch_start_first_task(void);
void os_arch_config_fault_trap(void);
void os_arch_core_ipi_request_cb(uint32_t core);
void os_arch_core_launch_cb(uint32_t core);
void os_arch_soc_idle_cb(void);

#define OS_ARCH_SPINLOCK_INIT { 0U }
typedef struct { uint32_t locked; } os_arch_spinlock_t;
void os_arch_spinlock_acquire(os_arch_spinlock_t *lock);
void os_arch_spinlock_release(os_arch_spinlock_t *lock);
void os_arch_isr_priority_check(void);
void os_arch_cycle_tick(void);
uint32_t os_arch_max_suppressed_ticks_get(void);
uint32_t os_arch_min_suppressed_ticks_get(void);
uint32_t os_arch_elapsed_ticks_get(void);
uint32_t os_arch_elapsed_ticks_peek(void);
void os_arch_sleep_finish(void);
void audit_sleep(uint32_t ticks);
#define OS_ARCH_SLEEP(ticks) audit_sleep(ticks)
uint32_t os_arch_delay_counter_hz_get(void);
uint32_t os_arch_delay_counter_get(void);
#ifdef __cplusplus
}
#endif

#endif
