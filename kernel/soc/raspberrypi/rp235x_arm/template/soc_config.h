/**
 * @file soc_config.h
 * @brief Template for the application's soc_config.h - every option of the raspberrypi/rp235x_arm SoC
 *        package at its default value.
 *
 * NOT included by the package as it sits here: copy it into the application beside os_config.h,
 * as soc_config.h, and change what needs changing. The package finds it on the same include path
 * OS_CONFIG_DIR already puts os_config.h on, so there is nothing further to set.
 *
 * REQUIRED, and so is every option in it - the same terms as os_config.h. The file holds ONLY the
 * values a user may need to decide; there are deliberately no built-in defaults, so a missing
 * option is rejected at compile time rather than silently read as 0. Whatever this package can
 * decide for itself is not in the file at all.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef SOC_CONFIG_H
#define SOC_CONFIG_H

/**
 * Which lock id the kernel takes for its critical sections.
 *
 * The SDK reserves two ids for "an operating system (or other system level software) co-existing
 * with the SDK", which is exactly this. Taking OS1 keeps the kernel clear of the ids the SDK
 * hands out at runtime. Change it only if something else already claimed OS1; OS2 is the other
 * one set aside.
 */
#define SOC_CONFIG_SPINLOCK_ID              PICO_SPINLOCK_ID_OS1

/**
 * Whether the package defines isr_systick to call os_tick_handler (1 = yes, the default).
 *
 * The kernel owns only PendSV and expects the application to route the tick. The SDK generates no
 * SysTick handler at all, so without this every project would write the same three-line function.
 *
 * Set it to 0 to write your own - to add work to the tick, or to count it somewhere. Then define
 * isr_systick in the application and call os_tick_handler() from it; leaving it out entirely
 * gives a board that hits crt0.S's breakpoint on the first tick.
 *
 * Ignored under OS_CONFIG_TICK_SOURCE_EXTERNAL, where the application owns the timer outright.
 */
#define SOC_CONFIG_SYSTICK_VECTOR           1U

/**
 * Whether os_arch_soc_init_cb() reads the live clock into SystemCoreClock (1 = yes, the default).
 *
 * The kernel takes the CPU frequency from SystemCoreClock, which the Pico SDK only provides in
 * its optional CMSIS stub - so the package defines and maintains it. Reading clock_get_hz(clk_sys)
 * at start-up is what makes set_sys_clock_khz() in main() come out right in the tick period and in
 * every os_delay_us() busy-wait.
 *
 * Set it to 0 only when the application maintains SystemCoreClock itself, and keep it current: a
 * stale value mis-programs the tick with no diagnostic anywhere.
 */
#define SOC_CONFIG_CLOCK_AUTO_UPDATE        1U

/**
 * Which hardware carries the "re-evaluate scheduling" nudge between cores
 * (1 = a claimed doorbell, the default; 0 = the inter-core FIFO).
 *
 * Doorbells exist for exactly this and leave the FIFO free for the SDK and for the application,
 * which is why they are the default. The FIFO is the fallback for a project that has already
 * claimed every doorbell.
 *
 * Note that the SDK's own multicore_lockout_* helpers use the FIFO. Choosing 0 here alongside
 * them means two users on one queue.
 *
 * Only on this package: the RP2040 has no doorbells, so raspberrypi/rp2040 always uses the FIFO
 * and has no such option.
 */
#define SOC_CONFIG_IPI_DOORBELL             1U

/**
 * Bring-up diagnostics: the package's own HardFault vector, and a line from each secondary
 * core as it starts (1 = on, 0 = off).
 *
 * The SDK's default fault vector is a breakpoint. With no debugger attached that escalates into a
 * second fault and the core simply stops - no message, no address, and on a multi-core build no
 * indication of WHICH core died. A core that faults looks exactly like a core that was never
 * started, which is a very expensive thing to confuse during bring-up.
 *
 * With this at 1 the package installs a handler that prints the faulting core, the instruction
 * address and the fault status registers, then parks. Printing from fault context is best-effort
 * rather than guaranteed - the transport may already be wedged - but it costs nothing to try and
 * it usually works over USB CDC.
 *
 * Set it to 0 when the application installs its own fault handling, or when a debugger is
 * attached and the breakpoint is what you actually want.
 */
#define SOC_CONFIG_FAULT_REPORT             1U

#endif /* SOC_CONFIG_H */
