/**
 * @file os_cb_template.c
 * @brief Template for the application-side kernel callbacks (_cb functions).
 *
 * NOT part of the kernel build, and it must never be added to it: copy this file into the
 * application source tree as os_cb.c, add that copy to the application build, and adapt the bodies
 * to the platform.
 *
 * Some of these are MANDATORY when their feature is enabled - the kernel declares them and defines
 * nothing, so a missing one is a link error rather than a silently empty hook. Each block below
 * says which it is. The #if guards match the exact condition under which the kernel calls the
 * group, in the same order as PART 2 of ahura.h and os_config.h, so the file compiles cleanly
 * under any configuration.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "ahura.h"

/*
 * ***********************************************************************************************************
 * Platform clock
 * ***********************************************************************************************************
*/

/* Nothing to implement here. The kernel reads the CPU frequency straight from the CMSIS
 * SystemCoreClock variable, which the device's SystemInit() sets and SystemCoreClockUpdate()
 * refreshes after every clock-tree change, so a board that boots on an internal oscillator and
 * later switches to a PLL is handled with no kernel involvement.
 *
 * Only devices whose startup code does not provide that symbol need to act, and then only by
 * defining it (anywhere in the application):
 *
 *     uint32_t SystemCoreClock = 120000000U;
 *
 * Keep it updated if the clock tree changes at runtime; the kernel re-reads it on every use.
 */

/*
 * ***********************************************************************************************************
 * Kernel tick source (OS_CONFIG_TICK_SOURCE_EXTERNAL only)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TICK_SOURCE == OS_CONFIG_TICK_SOURCE_EXTERNAL)
/******************************************************************************************************/
/**
 * @brief Start the timer that drives the kernel tick, and arrange for its ISR to call
 *        os_tick_handler() OS_CONFIG_TICK_HZ times per second.
 *
 * REQUIRED while OS_CONFIG_TICK_SOURCE is OS_CONFIG_TICK_SOURCE_EXTERNAL: the kernel ships no
 * default, so leaving this out is a link error rather than a kernel whose clock never advances -
 * which presents as every delay, timeout and timer hanging forever, with nothing pointing at the
 * tick as the cause. Delete this block (and set the option back to SYSTICK) to let the port
 * program SysTick itself.
 *
 * Called once from os_init(), after the application has configured its clock tree.
 *
 * Two rules for the interrupt this starts:
 *
 *   1. Give it the LOWEST priority the device offers, which is what the port does for SysTick.
 *      Anything higher lets a tick preempt application interrupts.
 *   2. It must be reachable by the kernel's interrupt mask. With a nonzero
 *      OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY, a tick ISR above that threshold is trapped by
 *      os_arch_isr_priority_check the moment it calls into the kernel.
 *
 * Example - an RTC/LPTIM-style peripheral, which is the usual reason to be here at all (Nordic
 * nRF5x, and any design whose SysTick stops in the sleep mode it ships with):
 *
 *     void os_arch_tick_init_cb(void)
 *     {
 *         my_lptim_start_periodic(OS_CONFIG_TICK_HZ);
 *         my_lptim_irq_priority_set(MY_LOWEST_IRQ_PRIORITY);
 *         my_lptim_irq_enable();
 *     }
 *
 *     void LPTIM_IRQHandler(void)   // the device's own vector name
 *     {
 *         my_lptim_flag_clear();
 *         os_tick_handler();
 *     }
 */
void os_arch_tick_init_cb(void)
{
}
#endif /* OS_CONFIG_TICK_SOURCE_EXTERNAL */

/*
 * ***********************************************************************************************************
 * Debug hooks
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_ASSERT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Called when an OS_ASSERT fails, just before the kernel halts.
 *
 * REQUIRED when OS_CONFIG_ASSERT_ENABLE is 1: the kernel ships no default, so leaving this out
 * is a link error. That is deliberate - a stub that did nothing would turn every assertion into
 * an unexplained halt with no clue where it came from.
 *
 * The kernel parks the core right after this returns, so there is no way to continue. Record
 * enough to find the cause: print it, store it in a retained/backup register or a noinit
 * section that survives reset, or just break into the debugger as below.
 *
 * Do not log from here through OS_LOG_*: the log task cannot run once the core is parked, so
 * the line would sit unsent in the buffer. Write directly to the transport instead.
 */
void os_assert_failed_cb(const char *file, uint32_t line)
{
    (void)file;
    (void)line;

    /* Example: __asm volatile("bkpt 0"); or a direct blocking UART write. */
}
#endif /* OS_CONFIG_ASSERT_ENABLE */

#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Called when a task is found to have overrun its stack, as it is switched out.
 *
 * REQUIRED when OS_CONFIG_STACK_CHECK_ENABLE is 1, exactly like os_assert_failed_cb above: the
 * kernel ships no default, so leaving this out is a link error. How a blown stack gets reported
 * belongs to the product, and a stub that did nothing would turn the detector into an unexplained
 * halt - the core parks right after this returns either way, so this is the only chance to say
 * which task it was.
 *
 * Runs inside PendSV with the kernel's interrupts masked. Do NOT call kernel APIs from here, and
 * do not use OS_LOG_*: the log task cannot run once the core is parked, so the line would never
 * leave the buffer. Write to the transport directly, or latch the pointer somewhere the debugger
 * can read after the halt.
 *
 * The usual fix is a bigger stack for that task (OS_TASK_DEFINE's second argument), or less on
 * it - large locals and printf-family calls are the common culprits.
 */
void os_stack_overflow_cb(const char *task_name)
{
    (void)task_name;

    /* Example: __asm volatile("bkpt 0"); or a direct blocking UART write of task_name. */
}
#endif /* OS_CONFIG_STACK_CHECK_ENABLE */

/* The self-test build owns this one. The suite has to see what the kernel actually emitted in
 * order to test the log at all, so ahura_kernel/test/os_test.c defines os_log_output_cb itself
 * and captures into a buffer it can search. Two definitions would be a duplicate-symbol link
 * error, so the application's steps aside for that build - which is also why this guard tests
 * OS_CONFIG_TEST_ENABLE while every other block here tests only its own feature switch.
 *
 * The suite's PASS/FAIL report does not come through here; it goes to printf. */
#if (OS_CONFIG_LOG_ENABLE == 1U) && (OS_CONFIG_TEST_ENABLE == 0U)
/******************************************************************************************************/
/**
 * @brief Transmit finished log bytes.
 *
 * Called from the kernel log task, never from an ISR or a critical section, so it may block or
 * start a DMA transfer. The buffer is only valid for the duration of the call: copy it if the
 * transport completes asynchronously, or block here until it has been consumed.
 *
 * Keep this reasonably prompt. It runs at OS_CONFIG_LOG_TASK_PRIORITY, so a slow transport
 * delays only the log, but the ring keeps filling while it runs and lines are dropped once it
 * is full.
 */
void os_log_output_cb(const uint8_t *data, size_t length)
{
    (void)data;
    (void)length;

    /* Example: HAL_UART_Transmit(&huart, (uint8_t *)data, length, HAL_MAX_DELAY); */
}
#endif /* OS_CONFIG_LOG_ENABLE && !OS_CONFIG_TEST_ENABLE */

/*
 * ***********************************************************************************************************
 * TrustZone secure-context management (OS_CONFIG_TRUSTZONE_NON_SECURE only)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
/******************************************************************************************************/
/**
 * @brief Bank the secure-side context (secure stack / PSP_S) of the task being switched out.
 *        task_id 0 is the idle task (never owns a secure context). Typically calls a secure
 *        gateway (cmse_nonsecure_entry) provided by the secure firmware.
 *
 * REQUIRED while OS_CONFIG_TRUSTZONE is OS_CONFIG_TRUSTZONE_NON_SECURE: the kernel ships no
 * default, so leaving this out is a link error rather than tasks switching with their secure
 * state left behind.
 */
void os_arch_tz_context_save_cb(uint32_t task_id)
{
    (void)task_id;
}

/******************************************************************************************************/
/**
 * @brief Restore the secure-side context of the task being switched in.
 */
void os_arch_tz_context_restore_cb(uint32_t task_id)
{
    (void)task_id;
}
#endif /* OS_CONFIG_TRUSTZONE_NON_SECURE */

/*
 * ***********************************************************************************************************
 * Multi-core SoC glue (OS_CONFIG_CORE_COUNT > 1 only)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Return the index of the calling core (0-based). SoC-specific: e.g. SIO CPUID on the RP2040.
 */
uint32_t os_arch_core_id_get_cb(void)
{
    return 0U;
}

/******************************************************************************************************/
/**
 * @brief Interrupt another core so it re-evaluates scheduling. SoC-specific: e.g. the RP2040
 *        inter-core FIFO/doorbell. Without an implementation the target core reacts at its
 *        next tick instead.
 */
void os_arch_core_ipi_request_cb(uint32_t core_id)
{
    (void)core_id;
}

/* Exactly the condition under which the kernel routes its spinlock through
 * these callbacks (os_arch_port_common.h), so the two can never disagree:
 * cores without LDREX/STREX, plus any core where
 * OS_CONFIG_SPINLOCK_SOC_BACKEND opts out of the built-in backend.
 * Testing OS_ARCH_HAS_EXCLUSIVES alone would miss that second case and leave
 * the opt-out failing at link time. */
#if (OS_ARCH_SPINLOCK_USE_CB)
/******************************************************************************************************/
/**
 * @brief Take the kernel spinlock, busy-waiting until it is free. Route it to the SoC's hardware
 *        spinlocks (e.g. RP2040 SIO). Called with interrupts already masked.
 *
 * MANDATORY when the built-in LDREX/STREX backend is unavailable or opted out of: the kernel
 * ships no default, so leaving it out fails at link time.
 */
void os_arch_spinlock_acquire_cb(os_arch_spinlock_t *lock)
{
    (void)lock;
}

/******************************************************************************************************/
/**
 * @brief Release the kernel spinlock taken by os_arch_spinlock_acquire_cb. MANDATORY on the same
 *        terms.
 */
void os_arch_spinlock_release_cb(os_arch_spinlock_t *lock)
{
    (void)lock;
}
#endif /* OS_ARCH_SPINLOCK_USE_CB */
#endif /* OS_CONFIG_CORE_COUNT > 1U */

/*
 * ***********************************************************************************************************
 * Tickless idle hooks (OS_CONFIG_TICKLESS_ENABLE only)
 *
 * Both MANDATORY when tickless idle is on: the kernel declares them and defines neither, so a
 * missing one is a link error. Delete the pair (and this block) when it is off.
 *
 * Worth checking before leaving pre-sleep empty: a vendor HAL driving its own periodic tick from a
 * separate timer wakes the WFI at that timer's period, cutting every suppressed sleep short.
 * Suspending it here and resuming it post-sleep is what makes tickless idle save power.
 *
 * BOTH run with the kernel's interrupts masked, which is what stops an ISR from moving a deadline
 * between the sleep length being decided and the WFI. So polling a hardware flag is fine, but
 * waiting on anything an interrupt must deliver (a DMA callback, HAL_GetTick()) will hang. Keep
 * both short - their duration adds directly to interrupt latency.
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Called right before the idle sleep: select the sleep mode (e.g. SLEEPDEEP), gate clocks.
 *
 * Empty body = plain SLEEP (SLEEPDEEP left clear): the CPU clock stops but every peripheral clock -
 * UARTs, timers, DMA - keeps running, so nothing needs saving here and os_tickless_post_sleep_cb()
 * has nothing to restore. If a peripheral's completion must be guaranteed before the CPU naps (e.g.
 * flush a debug UART so the last line is fully transmitted), block on its busy/TX-complete flag
 * here. Selecting a deeper mode (STOP/SLEEPDEEP) instead means gated peripheral and system clocks -
 * restore them (and re-run the clock configuration if PLL/HSE were affected) in
 * os_tickless_post_sleep_cb() before anything relies on them again.
 */
void os_tickless_pre_sleep_cb(void)
{
}

/******************************************************************************************************/
/**
 * @brief Called right after wakeup: clear SLEEPDEEP, restore clocks.
 *
 * Runs with the kernel's interrupts still masked and before the sleep has been announced, so the
 * kernel clock is still short by the whole sleep duration while this executes. Restore hardware
 * here; do not call kernel APIs that block, delay, or read the tick expecting it to be current.
 * Keep it short for the same reason: everything in here is added to interrupt latency.
 */
void os_tickless_post_sleep_cb(void)
{
}
#endif /* OS_CONFIG_TICKLESS_ENABLE */
