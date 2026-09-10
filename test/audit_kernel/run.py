"""Compile real kernel sources and execute deterministic ARM tests in Unicorn.

Test port stubs scheduler/IRQ hardware only; no hardware timing claims are made.
SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


TESTS = (
    "audit_migration", "audit_inheritance", "audit_idle_partial",
    "audit_new_owner_inheritance", "audit_new_owner_departure",
    "audit_boot_success", "audit_boot_idle_failure", "audit_boot_secondary_failure",
    "audit_boot_main_failure", "audit_start_before_init",
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gcc", required=True, type=Path)
    parser.add_argument("--python-deps", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--tests", nargs="+", choices=TESTS, default=TESTS)
    args = parser.parse_args()
    if args.python_deps:
        sys.path.insert(0, str(args.python_deps.resolve()))
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_PC

    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="audit-kernel-", dir=args.build_dir) as temp:
        elf = Path(temp) / "regression.elf"
        command = [str(args.gcc), "-mcpu=cortex-m3", "-mthumb", "-std=c11", "-O1", "-g",
                   "-Wall", "-Wextra", "-Wundef", "-Werror", "-ffreestanding", "-fno-builtin",
                   "-ffunction-sections", "-fdata-sections", "-Itest/audit_kernel", "-I.",
                   "test/audit_kernel/regression.c", "-nostdlib", "-Wl,-e,audit_migration",
                   "-Wl,-Ttext=0x10000", "-Wl,-Tdata=0x20000000", "-Wl,--gc-sections",
                   *[f"-Wl,--undefined={test}" for test in TESTS], "-lgcc", "-o", str(elf)]
        subprocess.run(command, cwd=root, check=True)
        nm = args.gcc.with_name(args.gcc.name.replace("gcc", "nm"))
        output = subprocess.check_output([str(nm), "-n", str(elf)], text=True)
        symbols = {}
        for line in output.splitlines():
            fields = line.split()
            if len(fields) == 3:
                symbols[fields[2]] = int(fields[0], 16)
        data = elf.read_bytes()
        phoff = struct.unpack_from("<I", data, 28)[0]
        phsize, phcount = struct.unpack_from("<HH", data, 42)
        results = []
        for test in args.tests:
            uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
            uc.mem_map(0, 0x200000)
            uc.mem_map(0x20000000, 0x100000)
            for index in range(phcount):
                kind, offset, virtual, _, filesz, _, _, _ = struct.unpack_from(
                    "<IIIIIIII", data, phoff + index * phsize)
                if kind == 1 and filesz:
                    uc.mem_write(virtual, data[offset:offset + filesz])
            uc.reg_write(UC_ARM_REG_SP, 0x200F0000)
            uc.mem_write(symbols["audit_result"], struct.pack("<I", 0xFFFFFFFF))
            uc.emu_start(symbols[test] | 1, symbols["audit_stop"] & ~1, count=2000000)
            result = struct.unpack("<I", bytes(uc.mem_read(symbols["audit_result"], 4)))[0]
            pc = uc.reg_read(UC_ARM_REG_PC)
            if pc != (symbols["audit_stop"] & ~1) or result != 0:
                raise RuntimeError(f"{test}: failure at regression.c:{result}, PC=0x{pc:x}")
            results.append({"test": test, "result": "PASS"})
        print(json.dumps({"kind": "real kernel sources / deterministic ARM emulation",
                          "compiler": str(args.gcc), "tests": results}, indent=2))


if __name__ == "__main__":
    main()
