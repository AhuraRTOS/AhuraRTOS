/**
 * @file soc_cb.c
 * @brief Template for the SoC-owned kernel callbacks (_cb functions), for targets with no SoC
 *        package under kernel/soc/.
 *
 * COPY THIS ONLY IF YOUR TARGET HAS NO SoC PACKAGE. Every callback here is a fact about the
 * silicon rather than a product decision, so a package under kernel/soc/<vendor>/<family>/ already
 * implements the whole group for the parts it covers - see doc/soc.md, which also lists what is
 * packaged today. Selecting one with AHURA_SOC is all that is needed; this file is then not part
 * of the build and must not be copied, because two definitions of one callback is exactly the
 * failure the split exists to prevent.
 *
 * So this file is the escape hatch that keeps every other MCU supported: an unpackaged part, a
 * custom ASIC, an FPGA soft core. Copy it into the application source tree as soc_cb.c, add
 * that copy to the application build, and fill in the bodies against the target's own registers.
 * When it works, a package under kernel/soc/ is the natural next step - it is this file plus a
 * soc.cmake, and it makes the port reusable.
 *
 * Some of these are MANDATORY when their feature is enabled - the kernel declares them and
 * defines nothing, so a missing one is a link error rather than a silently empty hook. Each block
 * below says which it is. The #if guards match the exact condition under which the kernel calls
 * the group, so the file compiles cleanly under any configuration.
 *
 * Every definition here is WEAK, the same way a vendor startup file marks its interrupt handlers,
 * so a strong definition of any one of them elsewhere in the application wins at link time with
 * no duplicate-symbol error and nothing to delete here. That is the supported way to override one
 * callback of a SoC package while keeping the rest: put the strong definition in the application,
 * where it beats the package's weak one.
 *
 * The application's own callbacks - os_log_output_cb, os_assert_failed_cb, os_stack_overflow_cb -
 * are NOT here. They live in template/os_cb.c, which every project copies.
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
OS_WEAK void os_arch_tick_init_cb(void)
{
}
#endif /* OS_CONFIG_TICK_SOURCE_EXTERNAL */

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
OS_WEAK void os_arch_tz_context_save_cb(uint32_t task_id)
{
    (void)task_id;
}

/******************************************************************************************************/
/**
 * @brief Restore the secure-side context of the task being switched in.
 */
OS_WEAK void os_arch_tz_context_restore_cb(uint32_t task_id)
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
OS_WEAK uint32_t os_arch_core_id_get_cb(void)
{
    return 0U;
}

/******************************************************************************************************/
/**
 * @brief Interrupt another core so it re-evaluates scheduling. SoC-specific: e.g. the RP2040
 *        inter-core FIFO/doorbell. Without an implementation the target core reacts at its
 *        next tick instead.
 */
OS_WEAK void os_arch_core_ipi_request_cb(uint32_t core_id)
{
    (void)core_id;
}

/******************************************************************************************************/
/**
 * @brief Boot a secondary core so it reaches os_core_start(). Called by os_start(), once per core
 *        from 1 to OS_CONFIG_CORE_COUNT-1, with the kernel complete and already running.
 *
 * REQUIRED when OS_CONFIG_CORE_COUNT is above 1: the kernel ships no default, so leaving this out
 * is a link error rather than a second core that silently never starts - which otherwise presents
 * as an application whose tasks simply never get scheduled there, with nothing pointing at the
 * cause.
 *
 * Two things the implementation owes the kernel, in this order:
 *
 *   1. Point the core at a vector table whose context-switch entry is the kernel's handler. On
 *      Cortex-M that is PendSV, and using core 0's own table is the usual answer.
 *   2. Have the core call os_core_start(), which configures its banked per-core state and enters
 *      the scheduler. It does not return.
 *
 * Do NOT start the core any earlier than this callback fires. os_core_start() begins dispatching
 * immediately, so a core released during os_init() would pick from ready lists core 0 is still
 * building.
 *
 * Example - the RP2040/RP2350, whose SDK boots core 1 onto core 0's vector table for you:
 *
 *     static void core1_entry(void)
 *     {
 *         my_core_local_irq_setup();   // anything banked per core
 *         os_core_start();             // does not return
 *     }
 *
 *     void os_arch_core_launch_cb(uint32_t core_id)
 *     {
 *         (void)core_id;               // only one secondary core on this part
 *         multicore_launch_core1(core1_entry);
 *     }
 */
OS_WEAK void os_arch_core_launch_cb(uint32_t core_id)
{
    (void)core_id;
}

/******************************************************************************************************/
/**
 * @brief Top of the given core's handler (MSP) stack. The first context switch on each core
 *        resets its MSP to this value while abandoning the boot context.
 *
 * REQUIRED when OS_CONFIG_CORE_COUNT is above 1: the kernel ships no default, so leaving this out
 * is a link error rather than a secondary core silently sharing core 0's handler stack - the
 * vector table only ever names core 0's initial stack pointer, and reading it on another core is
 * exactly that failure, with both cores' exception frames overwriting each other.
 *
 * Return the address a full stack pointer STARTS at (the top of the region, one past its highest
 * byte), from the symbols of the per-core stacks only the linker script knows:
 *
 *     uint32_t os_arch_handler_stack_top_cb(uint32_t core_id)
 *     {
 *         return (core_id == 1U)
 *              ? (uint32_t)&__StackOneTop    // one symbol pair per core
 *              : (uint32_t)&__StackTop;
 *     }
 */
OS_WEAK uint32_t os_arch_handler_stack_top_cb(uint32_t core_id)
{
    (void)core_id;
    return 0U;
}

/* os_arch_handler_stack_limit_cb() is deliberately NOT defined here. The ARMv8-M port ships a
 * weak default for it (the linker's single-stack symbols, correct for core 0, declined for the
 * rest), so a second weak definition in this file would race it at link time with no way to say
 * which won. A multi-core ARMv8-M target OVERRIDES it with a strong definition returning each
 * core's stack bottom, exactly like the top callback above; other ports never call it. */

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
OS_WEAK void os_arch_spinlock_acquire_cb(os_arch_spinlock_t *lock)
{
    (void)lock;
}

/******************************************************************************************************/
/**
 * @brief Release the kernel spinlock taken by os_arch_spinlock_acquire_cb. MANDATORY on the same
 *        terms.
 */
OS_WEAK void os_arch_spinlock_release_cb(os_arch_spinlock_t *lock)
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
OS_WEAK void os_tickless_pre_sleep_cb(void)
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
OS_WEAK void os_tickless_post_sleep_cb(void)
{
}
#endif /* OS_CONFIG_TICKLESS_ENABLE */
