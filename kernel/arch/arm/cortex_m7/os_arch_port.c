/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M7: uses the shared ARMv7-M implementation
 *        (FPU context handling compiles in; the DWT LAR unlock in os_arch_init is
 *        required on this core and already part of the shared code).
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#include "../common/os_arch_port_v7m.c"
