"""Compile real timing sources and execute deterministic Cortex-M regressions.
Requires Arm GCC and Unicorn. No peripheral or dual-CPU timing claim is made.
SPDX-License-Identifier: GPL-3.0-or-later
"""
from pathlib import Path
import argparse
import json
import subprocess
import sys

TESTS = ["test_remote_time_origin", "test_raw_lock_time_origin",
         "test_tickless_owner_migration", "test_tickless_owner_window",
         "test_sleep_prepare_declines", "test_sleep_prepare_short_deadline", "test_sleep_prepare_replans_deadline",
         "test_usage_writer_lock", "test_masked_busy_wait",
         "test_missing_busy_wait_counter", "test_tick_phase_arithmetic"]

def tool(directory, name):
    path = directory / name
    return path if path.is_file() else path.with_suffix(".exe")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--python-deps", type=Path)
    args = parser.parse_args()
    if args.python_deps:
        sys.path.insert(0, str(args.python_deps.resolve()))
    root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(root / "test/audit_ipc"))
    from run import execute_arm
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    linker = build / "harness.ld"
    linker.write_text("SECTIONS { . = 0x10000; .text : { *(.text*) *(.rodata*) } "
                      ".data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } "
                      "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }\n")
    elf = build / "regression.elf"
    gcc = tool(args.toolchain, "arm-none-eabi-gcc")
    command = [str(gcc), "-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft", "-std=c11", "-O1",
               "-Wall", "-Wextra", "-Wundef", "-Werror", "-fno-builtin", "-ffunction-sections",
               "-fdata-sections", "-I" + str(root / "test/audit_arch"), "-I" + str(root),
               str(root / "test/audit_arch/regression.c"), "-nostartfiles", "-nostdlib",
               "-Wl,--gc-sections", "-Wl,-T," + str(linker), "-Wl,-e," + TESTS[0],
               *["-Wl,--undefined=" + test for test in TESTS], "-lgcc", "-o", str(elf)]
    subprocess.run(command, check=True)
    binary = build / "regression.bin"
    subprocess.run([str(tool(args.toolchain, "arm-none-eabi-objcopy")), "-O", "binary", str(elf), str(binary)], check=True)
    nm = subprocess.check_output([str(tool(args.toolchain, "arm-none-eabi-nm")), str(elf)], text=True)
    symbols = {fields[2]: int(fields[0], 16) for line in nm.splitlines() if len(fields := line.split()) == 3}
    data = binary.read_bytes()
    results = [{"test": test, "failure_line": execute_arm(data, symbols, test, instruction_limit=30000000)} for test in TESTS]
    (build / "results.json").write_text(json.dumps({"command": command, "tests": results}, indent=2) + "\n")
    print(json.dumps(results, indent=2))
    assert all(item["failure_line"] == 0 for item in results)

if __name__ == "__main__":
    main()
