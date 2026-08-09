/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M23: uses the shared ARMv6-M-compatible
 *        implementation, not the v8m one (ARMv8-M baseline executes the Thumb-1
 *        subset, not the mainline Thumb-2 ISA; non-secure baseline has no
 *        PSPLIM, so no stack-limit support is lost). TrustZone is selected
 *        with OS_CONFIG_TRUSTZONE.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#include "../common/os_arch_port_v6m.c"
