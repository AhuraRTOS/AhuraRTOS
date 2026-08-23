/**
 * @file os_arch_port.h
 * @brief Architecture port interface for the Hazard3 RISC-V core (RV32IMAC, RP2350).
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_ARCH_PORT_H
#define OS_ARCH_PORT_H

/*
 * Hazard3 implements Xh3irq, its custom interrupt-controller extension, and that is a property of
 * THIS CORE rather than of the chip around it - so it is stated here, in the core folder, and the
 * shared RV32 file branches on it. A future RISC-V core folder that lacks Xh3irq simply does not
 * define this and gets the generic behaviour.
 *
 * It is not taken from the compiler: -march does not advertise it (the ISA string has no room for
 * vendor extensions), and the Pico SDK's own __hazard3_extension_xh3irq comes from
 * hardware/hazard3/features.h - an SDK header, which the kernel does not depend on by design.
 * Selecting this folder IS the assertion that the core has it.
 */
#define OS_ARCH_HAS_XH3IRQ    1

#include "../common/os_arch_port_common.h"

#endif /* OS_ARCH_PORT_H */
