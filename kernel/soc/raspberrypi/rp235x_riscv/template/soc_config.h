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

#endif /* SOC_CONFIG_H */
