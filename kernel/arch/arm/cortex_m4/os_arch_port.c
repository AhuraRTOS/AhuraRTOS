/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M4: uses the shared ARMv7-M implementation
 *        (FPU context handling compiles in when built with a hard/softfp float ABI).
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#include "../common/os_arch_port_v7m.c"
