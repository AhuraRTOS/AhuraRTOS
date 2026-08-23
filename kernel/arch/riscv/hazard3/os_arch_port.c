/**
 * @file os_arch_port.c
 * @brief Architecture port for the Hazard3 RISC-V core (RV32IMAC, RP2350).
 *
 * The whole implementation is the shared RV32 file, included textually here exactly as the ARM core
 * folders include their profile's port. Compiling ../common/os_arch_port_rv32.c separately would
 * produce duplicate symbols; the build adds this file only.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#if !defined(__riscv) || (__riscv_xlen != 32)
#error "kernel/arch/riscv/hazard3 is an RV32 port: build it with a 32-bit RISC-V toolchain (-march=rv32...)."
#endif

#include "../common/os_arch_port_rv32.c"
