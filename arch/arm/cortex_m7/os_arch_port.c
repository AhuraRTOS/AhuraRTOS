/**
 * @file os_arch_port.c
 * @brief Architecture port for ARM Cortex-M7: uses the shared ARMv7-M implementation
 *        (FPU context handling compiles in; the DWT LAR unlock in os_arch_init is
 *        required on this core and already part of the shared code).
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

#include "../common/os_arch_port_v7m.c"
