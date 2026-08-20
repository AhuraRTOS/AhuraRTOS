/**
 * @file soc_config.h
 * @brief Template for the application's soc_config.h - every option of the st/stm32 SoC package
 *        at its default value.
 *
 * NOT included by the package as it sits here: copy it into the application beside os_config.h,
 * as soc_config.h, and change what needs changing. The package finds it on the same include path
 * OS_CONFIG_DIR already puts os_config.h on, so there is nothing further to set. On a CubeMX
 * project that means Core/Inc.
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

/*
 * ***********************************************************************************************************
 * Vendor headers
 * ***********************************************************************************************************
*/

/**
 * The header that brings in this device's CMSIS and HAL declarations.
 *
 * There is no one name to hardcode: it is stm32h5xx_hal.h on an H5, stm32f4xx_hal.h on an F4, and
 * so on for every family. "main.h" is the default because CubeMX generates it for every project
 * and it includes the right family header itself, so one value is correct across the whole STM32
 * range. It also sits in Core/Inc, which is already on the kernel's include path - that is where
 * os_config.h lives.
 *
 * Point it at the family header directly (for instance "stm32h5xx_hal.h") on a project that has
 * no main.h, or one whose main.h drags in more than the SoC layer should see.
 *
 * A bare-CMSIS project with no HAL at all sets SOC_CONFIG_HAL_ENABLE to 0 instead: what turns off
 * is the clock refresh and the tickless timebase handling below, both of which are HAL calls.
 */
#define SOC_CONFIG_HAL_HEADER               "main.h"

/**
 * Whether this project has the ST HAL at all (1 = yes, and the header above is included; 0 = no).
 *
 * A bare-CMSIS project with no HAL is a perfectly good STM32 project, and everything this package
 * contributes happens to be a HAL call - the SystemCoreClock refresh and the tickless timebase
 * handling. With this at 0 both compile away and the kernel behaves exactly as it does with no
 * SoC package at all.
 *
 * Stated here rather than discovered by testing whether the header is reachable: whether the HAL
 * is part of the design is the project's decision, not something the build should infer from an
 * include path that might just be missing.
 */
#define SOC_CONFIG_HAL_ENABLE               1U

/*
 * ***********************************************************************************************************
 * CPU clock
 * ***********************************************************************************************************
*/

/**
 * Whether os_arch_soc_init_cb() calls SystemCoreClockUpdate() before the kernel starts
 * (1 = yes, the default).
 *
 * The kernel takes the CPU frequency from SystemCoreClock and programs the tick from it, so a
 * stale value gives a tick at the wrong rate and every os_delay_us() busy-wait wrong with it, in
 * a way nothing reports. CubeMX's generated SystemClock_Config() normally leaves the variable
 * correct; this is one call at start-up that makes it true whatever the project did, including a
 * hand-written clock setup that forgot.
 *
 * Set it to 0 only if the application maintains SystemCoreClock itself.
 */
#define SOC_CONFIG_CLOCK_AUTO_UPDATE        1U

/*
 * ***********************************************************************************************************
 * Tickless idle (OS_CONFIG_TICKLESS_ENABLE only)
 * ***********************************************************************************************************
*/

/**
 * Whether the tickless sleep hooks suspend and resume the HAL timebase (1 = yes, the default).
 *
 * This is the option that decides whether tickless idle saves any power on an STM32, so it is
 * worth reading rather than skipping.
 *
 * The kernel takes SysTick, which means CubeMX has been told to run the HAL from a spare timer
 * (SYS -> Timebase Source). That timer keeps interrupting at its own period - 1 kHz by default -
 * and WFI wakes on any pending interrupt whether or not it is masked. So a sleep the kernel
 * planned to run for 500 ms ends after 1 ms, every time, and tickless idle appears to do nothing.
 *
 * HAL_SuspendTick() and HAL_ResumeTick() are ST's own hook for this, and bracketing the sleep with
 * them is what lets it run its full length. HAL_GetTick() does not advance while suspended, which
 * is correct - no time is lost, the counter simply does not tick while the CPU is asleep - but it
 * does mean a HAL driver's own timeout cannot be relied on across the sleep window.
 *
 * Set it to 0 when the HAL timebase must keep running, or when the application brackets the sleep
 * itself by defining the two callbacks (a strong definition in the application replaces the
 * package's weak one).
 */
#define SOC_CONFIG_TICKLESS_HAL_TICK        1U

#endif /* SOC_CONFIG_H */
