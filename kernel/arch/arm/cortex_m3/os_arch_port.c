/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M3: uses the shared ARMv7-M implementation
 *        (no FPU on this core, so the FP paths compile out).
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#include "../common/os_arch_port_v7m.c"
