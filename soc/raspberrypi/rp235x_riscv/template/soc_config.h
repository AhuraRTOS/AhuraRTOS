/**
 * @file soc_config.h
 * @brief Template for the application's soc_config.h - every option of the raspberrypi/rp235x_riscv
 *        SoC package at its default value.
 *
 * NOT included by the package as it sits here: copy it into the application beside os_config.h, as
 * soc_config.h, and change what needs changing. The package finds it on the same include path
 * OS_CONFIG_DIR already puts os_config.h on, so there is nothing further to set.
 *
 * REQUIRED, and so is every option in it - the same terms as os_config.h. The file holds ONLY the
 * values a user may need to decide; there are deliberately no built-in defaults, so a missing
 * option is rejected at compile time rather than silently read as 0.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef SOC_CONFIG_H
#define SOC_CONFIG_H

/**
 * Which hardware lock id the kernel takes for its critical sections.
 *
 * The SDK reserves two ids for "an operating system (or other system level software) co-existing
 * with the SDK", which is exactly this. Taking OS1 keeps the kernel clear of the ids the SDK hands
 * out at runtime. Change it only if something else already claimed OS1; OS2 is the other one set
 * aside.
 */
#define SOC_CONFIG_SPINLOCK_ID              PICO_SPINLOCK_ID_OS1

/**
 * Bytes of trap stack reserved for each core the kernel starts itself.
 *
 * RISC-V has one stack pointer, so a trap runs on whatever stack was current - normally the
 * interrupted task's. A secondary core still needs somewhere to land before it has a task, and
 * nothing in the SDK reserves a stack for a core the kernel launched, so the package provides one.
 *
 * Core 0 is not counted here: it keeps the stack the linker script gave it.
 *
 * Ignored entirely on a single-core build (OS_CONFIG_CORE_COUNT == 1), where no such core exists.
 */
#define SOC_CONFIG_HANDLER_STACK_SIZE       1024U

/*
 * ***********************************************************************************************************
 * Tickless wake source (OS_CONFIG_TICKLESS_ENABLE only)
 * ***********************************************************************************************************
 *
 * There is nothing to pick: what ends a suppressed window follows from how deep the core sleeps,
 * which is SOC_CONFIG_SLEEP_MODE below.
 *
 *   LIGHT   the clocks keep running, so the window is ended by
 *           the RISC-V machine timer, mtime/mtimecmp - an absolute 64-bit deadline, which is
 *           why suppression on this port is a single write.
 *   DEEP    the clocks are gated and everything derived from them stops, so it would take
 *           the always-on POWMAN alarm - which this package does not
 *           implement yet, and says so rather than sleeping shallowly and reporting nothing.
 *
 * It used to be five flags plus the mode, with an arithmetic rule saying exactly one flag had to
 * be 1, another naming the sources this part does not physically have, and a third refusing deep
 * sleep against a source that stops with the clocks. None of those states can be expressed any
 * more, so none of those rules exists.
*/

/* How deep the core sleeps inside a window.
 *   OS_CONFIG_SLEEP_MODE_LIGHT   core stops, clocks keep running. Works with every source.
 *   OS_CONFIG_SLEEP_MODE_DEEP    clocks gated. Needs an always-on alarm to end the window.
 * Values: one of the two above. */
#define SOC_CONFIG_SLEEP_MODE               OS_CONFIG_SLEEP_MODE_LIGHT

#endif /* SOC_CONFIG_H */
