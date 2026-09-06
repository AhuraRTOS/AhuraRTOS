/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M33: uses the shared ARMv8-M mainline
 *        implementation (FPU context handling compiles in with a hard/softfp
 *        float ABI; per-task PSPLIM and the MSPLIM handler-stack guard are
 *        always active; TrustZone selected with OS_CONFIG_TRUSTZONE).
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

#include "../common/os_arch_port_v8m.c"
