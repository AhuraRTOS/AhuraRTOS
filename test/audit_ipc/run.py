"""Compile actual IPC sources for Arm and execute the deterministic C regressions.

Requires Arm GNU Toolchain and Python unicorn (pip install unicorn).
No board, libc syscalls, scheduler context switching, or concurrent CPUs involved.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def execute_arm(binary, symbols, entry, instruction_limit=5_000_000):
    """Run a linked Thumb/M-class function; return its C assertion line (0=pass)."""
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_INTR
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    cpu.mem_map(0x10000, 0x200000)
    cpu.mem_map(0x300000, 0x1000)
    cpu.mem_write(0x10000, binary)
    cpu.reg_write(UC_ARM_REG_SP, 0x20FFF8)
    cpu.reg_write(UC_ARM_REG_LR, 0x300001)
    stopped = []

    def interrupt(uc, number, unused):
        if number != 7:  # Unicorn's Arm BKPT exception.
            raise AssertionError(f"unexpected CPU exception {number}")
        stopped.append(number)
        uc.emu_stop()

    cpu.hook_add(UC_HOOK_INTR, interrupt)
    cpu.emu_start(symbols[entry] | 1, 0x300000, count=instruction_limit)
    pc = cpu.reg_read(UC_ARM_REG_PC)
    assert stopped or pc == 0x300000, f"{entry}: instruction limit reached at {pc:#x}"
    failure = int.from_bytes(cpu.mem_read(symbols["test_failure"], 4), "little")
    if not stopped:
        assert cpu.reg_read(UC_ARM_REG_R0) == failure
    return failure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--toolchain", type=Path, required=True, help="Arm GCC bin directory")
    parser.add_argument("--build", type=Path, required=True, help="Disposable output directory")
    parser.add_argument("--python-deps", type=Path, help="Optional local pip --target directory")
    args = parser.parse_args()
    if args.python_deps:
        sys.path.insert(0, str(args.python_deps.resolve()))
    repo = args.repo.resolve()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    config = (repo / "template/os_config.h").read_text(encoding="utf-8")
    changes = {"OS_CONFIG_ALLOC_ENABLE": "0U", "OS_CONFIG_ASSERT_ENABLE": "0U",
               "OS_CONFIG_MSG_ENABLE": "1U", "OS_CONFIG_LOG_ENABLE": "1U",
               "OS_CONFIG_LOG_BUFFER_SIZE": "64U", "OS_CONFIG_CORE_COUNT": "1U",
               "OS_CONFIG_TICK_HZ": "1000U", "OS_CONFIG_TICKLESS_ENABLE": "0U"}
    for key, value in changes.items():
        config, count = re.subn(r"^(#define\s+" + key + r"\s+)\S+", lambda m: m[1] + value,
                                config, flags=re.M)
        assert count == 1, key
    (build / "os_config.h").write_text(config, encoding="utf-8")
    linker = build / "harness.ld"
    linker.write_text("SECTIONS { . = 0x10000; .text : { *(.text*) *(.rodata*) } "
                      ".data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } "
                      "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }\n")
    tests = ["test_two_senders", "test_eligible_sender", "test_competing_senders", "test_maximum_sender_metadata",
             "test_receivers", "test_remaining_messages", "test_log_drain", "test_log_notice_full_ring"]
    suffix = ".exe" if os.name == "nt" else ""
    gcc = args.toolchain / ("arm-none-eabi-gcc" + suffix)
    elf = build / "regression.elf"
    flags = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-std=c11", "-O1",
             "-Wall", "-Wextra", "-Wundef", "-Werror", "-fno-builtin", "-ffunction-sections",
             "-fdata-sections", "-I" + str(build), "-I" + str(repo),
             "-I" + str(repo / "arch/arm/cortex_m4")]
    command = [str(gcc), *flags, str(Path(__file__).resolve().parent / "regression.c"), "-nostartfiles",
               "-nostdlib", "-Wl,--gc-sections", "-Wl,-T," + str(linker), "-Wl,-e,test_two_senders",
               *["-Wl,--undefined=" + test for test in tests], "-lc", "-lgcc", "-o", str(elf)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if compiled.returncode:
        raise RuntimeError(compiled.stdout + compiled.stderr)
    binary_file = build / "regression.bin"
    subprocess.run([str(args.toolchain / ("arm-none-eabi-objcopy" + suffix)), "-O", "binary", str(elf),
                    str(binary_file)], check=True, capture_output=True, text=True)
    nm = subprocess.run([str(args.toolchain / ("arm-none-eabi-nm" + suffix)), str(elf)], check=True,
                        capture_output=True, text=True).stdout
    symbols = {fields[2]: int(fields[0], 16) for line in nm.splitlines()
               if len(fields := line.split()) == 3}
    binary = binary_file.read_bytes()
    results = [{"test": test, "failure_line": execute_arm(binary, symbols, test)} for test in tests]

    # Compile the full real umbrella header in both C++ configurations.
    gxx = args.toolchain / ("arm-none-eabi-g++" + suffix)
    cpp = build / "public_header.cpp"
    for enabled in (0, 1):
        cpp.write_text('#include "os_arch_port.h"\n#undef OS_CONFIG_MSG_ENABLE\n'
                       f'#define OS_CONFIG_MSG_ENABLE {enabled}U\n#include "ahura.h"\n')
        result = subprocess.run([str(gxx), "-mcpu=cortex-m4", "-mthumb", "-std=c++17",
                                 "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                                 "-I" + str(build), "-I" + str(repo),
                                 "-I" + str(repo / "arch/arm/cortex_m4"), str(cpp)],
                                capture_output=True, text=True, timeout=30)
        results.append({"test": f"cpp_messages_{enabled}", "failure_line": result.returncode,
                        "diagnostics": result.stdout + result.stderr})
    report = {"compiler_command": command, "tests": results}
    (build / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(results, indent=2))
    assert all(result["failure_line"] == 0 for result in results), "IPC regression failed"


if __name__ == "__main__":
    main()
