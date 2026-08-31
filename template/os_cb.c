/**
 * @file os_cb.c
 * @brief Template for the application-owned kernel callbacks (_cb functions).
 *
 * NOT part of the kernel build, and it must never be added to it: copy this file into the
 * application source tree as os_cb.c, add that copy to the application build, and adapt the
 * bodies to the product.
 *
 * This file holds the callbacks the APPLICATION owns, the ones whose answer is a product
 * decision rather than a fact about the chip: where a log line goes, what a failed assertion
 * does, how a blown stack is reported. Nothing here depends on the target.
 *
 * The other group is different in kind. Core id, the inter-core IPI, the kernel spinlock, the
 * tick timer, TrustZone banking and the tickless sleep hooks are all facts about the SoC, so a
 * SoC package under soc/ supplies them (see doc/soc.md). A target with no package copies
 * template/soc_cb.c into the application instead and fills it in by hand.
 *
 * Do NOT define that second group here as well. The kernel ships no default for it, so the
 * symbols are undefined until something supplies them - which is what makes the linker pull them
 * out of the SoC package. An empty stub in this file, compiled straight into the application,
 * defines the symbol before the linker ever looks in the package, so the package's real
 * implementation is never extracted and the stub wins in silence. On SMP that reads as every core
 * reporting id 0, which corrupts shared state rather than failing to build.
 *
 * Some of these are MANDATORY when their feature is enabled - the kernel declares them and
 * defines nothing, so a missing one is a link error rather than a silently empty hook. Each block
 * below says which it is. The #if guards match the exact condition under which the kernel calls
 * the group, in the same order as PART 2 of ahura.h and os_config.h, so the file compiles cleanly
 * under any configuration.
 *
 * Every definition here is WEAK, the same way a vendor startup file marks its interrupt handlers.
 * So there are two ways to work: edit a body in place, or leave this file exactly as it came and
 * put a normal (strong) definition of that one function anywhere else in the application - the
 * strong one wins at link time, with no duplicate-symbol error and nothing to delete here. Useful
 * when the natural home for a callback is the driver it belongs to, such as os_log_output_cb next
 * to the UART code.
 *
 * It costs none of the safety above. Weak or not, this file is still the ONLY definition of these
 * symbols, so forgetting to copy it into the build is still a link error naming exactly what is
 * missing.
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
OS_WEAK void os_assert_failed_cb(const char *file, uint32_t line)
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
 *
 * task_name is NULL in a build with OS_CONFIG_TASK_NAME_ENABLE at 0, which drops the names
 * altogether; the overflow is still detected and reported, it just cannot say whose it was.
 *
 * @param[in] task_name  Name of the task that overran, or NULL when the build carries no names.
 */
OS_WEAK void os_stack_overflow_cb(const char *task_name)
{
    (void)task_name;

    /* Example: __asm volatile("bkpt 0"); or a direct blocking UART write of task_name. */
}
#endif /* OS_CONFIG_STACK_CHECK_ENABLE */

/* The self-test build owns this one. The suite has to see what the kernel actually emitted in
 * order to test the log at all, so AhuraRTOS/test/os_test.c defines os_log_output_cb itself
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
OS_WEAK void os_log_output_cb(const uint8_t *data, size_t length)
{
    (void)data;
    (void)length;

    /* Example: HAL_UART_Transmit(&huart, (uint8_t *)data, length, HAL_MAX_DELAY); */
}
#endif /* OS_CONFIG_LOG_ENABLE && !OS_CONFIG_TEST_ENABLE */

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Called with interrupts masked, immediately before a suppressed idle window.
 *
 * Where a board selects its sleep mode and quiesces whatever must not run through it: set
 * SLEEPDEEP, gate clocks, flush a UART that would otherwise lose the bytes still in its FIFO.
 *
 * Nothing is needed on this part. The tick here comes from the SoC's mtimecmp, which keeps
 * counting through a WFI, and no HAL runs a second tick source that would cut the window short.
 * A part that stops its tick timer in the sleep mode it selects has to arm an always-on timer
 * here instead, and report the real elapsed time from the post-sleep hook.
 *
 * @return None.
 */
void os_tickless_pre_sleep_cb(void)
{
}

/******************************************************************************************************/
/**
 * @brief Called with interrupts still masked, after the sleep and BEFORE it has been announced.
 *
 * os_tick_get() is still short by the whole sleep while this runs, so nothing here may block,
 * delay, or wait on a tick-driven timeout. Its whole duration is added to interrupt latency.
 *
 * @return None.
 */
void os_tickless_post_sleep_cb(void)
{
}
#endif /* OS_CONFIG_TICKLESS_ENABLE */
