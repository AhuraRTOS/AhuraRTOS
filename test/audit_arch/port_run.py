"""Execute real Cortex-M port timing code with deterministic MMIO, and verify external ownership.
The MMIO model records writes; it is not a cycle-accurate SysTick simulation.
SPDX-License-Identifier: GPL-3.0-or-later
"""
from pathlib import Path
import argparse
import json
import re
import subprocess
import sys

SELF_TESTS = ["test_port_early_phase", "test_port_pending_open", "test_port_zero_initial", "test_port_full_window",
              "test_port_light_partial_windows", "test_port_light_boundaries"]

def execute(binary, symbols, entry, systick):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_WRITE, UC_HOOK_INTR
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    cpu.mem_map(0x10000, 0x200000)
    cpu.mem_map(0x300000, 0x1000)
    cpu.mem_write(0x10000, binary)
    cpu.reg_write(UC_ARM_REG_SP, 0x20FFF8)
    cpu.reg_write(UC_ARM_REG_LR, 0x300001)
    reloads = []
    def write(uc, access, address, size, value, unused):
        if address == 0xE000E010 and value & 1:
            reloads.append(int.from_bytes(uc.mem_read(0xE000E014, 4), "little"))
    def interrupt(uc, number, unused):
        raise AssertionError(f"{entry}: CPU exception {number}, assertion line "
                             + str(int.from_bytes(uc.mem_read(symbols["test_failure"], 4), "little")))
    cpu.hook_add(UC_HOOK_INTR, interrupt)
    if systick:
        cpu.mem_map(0xE000E000, 0x1000)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write)
    cpu.emu_start(symbols[entry] | 1, 0x300000, count=1000000)
    assert cpu.reg_read(UC_ARM_REG_PC) == 0x300000, entry
    failure = int.from_bytes(cpu.mem_read(symbols["test_failure"], 4), "little")
    assert failure == 0, (entry, failure)
    if systick:
        expected = int.from_bytes(cpu.mem_read(symbols["expected_reload"], 4), "little")
        assert reloads[-1] == expected, (entry, reloads, expected)
    return {"test": entry, "result": "PASS", "reloads_at_enable": reloads if len(reloads) < 10 else [reloads[0], reloads[-1]],
            "enable_count": len(reloads)}

def tool(directory, name):
    path = directory / name
    return path if path.is_file() else path.with_suffix(".exe")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--python-deps", type=Path)
    args = parser.parse_args()
    if args.python_deps:
        sys.path.insert(0, str(args.python_deps.resolve()))
    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    base = (root / "template/os_config.h").read_text(encoding="utf-8")
    results = []
    for mode in ("self", "external", "smp"):
        directory = build / mode
        directory.mkdir(exist_ok=True)
        config = base
        changes = {"OS_CONFIG_TICKLESS_ENABLE": "1U", "OS_CONFIG_CORE_COUNT": "2U" if mode == "smp" else "1U"}
        if mode == "external":
            changes["OS_CONFIG_TICK_SOURCE"] = "OS_CONFIG_TICK_SOURCE_EXTERNAL"
        for key, value in changes.items():
            config = re.sub(r"^(#define\s+" + key + r"\s+)\S+", lambda m: m[1] + value, config, flags=re.M)
        (directory / "os_config.h").write_text(config)
        linker = directory / "harness.ld"
        linker.write_text("SECTIONS { . = 0x10000; .text : { *(.text*) *(.rodata*) } "
                          ".data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } "
                          "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }\n")
        tests = SELF_TESTS if mode == "self" else (["test_port_shared_counter"] if mode == "smp" else ["test_external_ownership"])
        sources = [root / "test/audit_arch/port_regression.c", root / "test/audit_arch/reference_callbacks.c"] if mode != "external" else [
            root / "test/audit_arch/external_regression.c", root / "arch/arm/cortex_m33/os_arch_port.c"]
        elf = directory / "regression.elf"
        command = [str(tool(args.toolchain, "arm-none-eabi-gcc")), "-mcpu=cortex-m33", "-mthumb",
                   "-mfloat-abi=soft", "-DOS_ARCH_TICKLESS_REFERENCE_CLOCK=1", "-std=c11", "-O1", "-Wall", "-Wextra", "-Wundef", "-Werror",
                   "-ffunction-sections", "-fdata-sections", "-I" + str(directory), "-I" + str(root),
                   "-I" + str(root / "arch/arm/cortex_m33"), *map(str, sources), "-nostartfiles", "-nostdlib",
                   "-Wl,--gc-sections", "-Wl,-T," + str(linker), "-Wl,-e," + tests[0],
                   *["-Wl,--undefined=" + test for test in tests], "-lgcc", "-o", str(elf)]
        subprocess.run(command, check=True)
        binary = directory / "regression.bin"
        subprocess.run([str(tool(args.toolchain, "arm-none-eabi-objcopy")), "-O", "binary", str(elf), str(binary)], check=True)
        nm = subprocess.check_output([str(tool(args.toolchain, "arm-none-eabi-nm")), str(elf)], text=True)
        symbols = {f[2]: int(f[0], 16) for line in nm.splitlines() if len(f := line.split()) == 3}
        results += [execute(binary.read_bytes(), symbols, test, mode == "self") for test in tests]
    (build / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))

if __name__ == "__main__":
    main()
