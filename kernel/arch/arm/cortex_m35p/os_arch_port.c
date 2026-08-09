/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M35P: uses the shared ARMv8-M mainline
 *        implementation (FPU context, per-task PSPLIM and the MSPLIM
 *        handler-stack guard; TrustZone selected with OS_CONFIG_TRUSTZONE).
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#include "../common/os_arch_port_v8m.c"
