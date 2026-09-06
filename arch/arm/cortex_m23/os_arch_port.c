/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M23: uses the shared ARMv6-M-compatible
 *        implementation, not the v8m one (ARMv8-M baseline executes the Thumb-1
 *        subset, not the mainline Thumb-2 ISA; non-secure baseline has no
 *        PSPLIM, so no stack-limit support is lost). TrustZone is selected
 *        with OS_CONFIG_TRUSTZONE.
 *
 * THIN WRAPPER - DO NOT EDIT HERE. The implementation is the file it includes; an edit here
 * would change this core alone and silently diverge from the others. See doc/design.md.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

/* This file IS the translation unit; the shared implementation below is a textual
 * include and refuses to compile without this. */
#define OS_ARCH_PORT_TRANSLATION_UNIT

#include "../common/os_arch_port_v6m.c"
