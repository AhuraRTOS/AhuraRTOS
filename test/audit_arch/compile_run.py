"""Compile architecture regressions with real GNU cross compilers.
SPDX-License-Identifier: GPL-3.0-or-later
"""
from pathlib import Path
import argparse
import json
import re
import subprocess

def tool(directory, name):
    path = directory / name
    return path if path.is_file() else path.with_suffix(".exe")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--riscv-toolchain", type=Path)
    parser.add_argument("--build", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    base = (root / "template/os_config.h").read_text(encoding="utf-8")
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    results = []
    for core in ("m52", "m55", "m85", "m33", "m0"):
        directory = build / core
        directory.mkdir(exist_ok=True)
        config = re.sub(r"(#define\s+OS_CONFIG_TICKLESS_ENABLE\s+)\S+", r"\g<1>1U", base)
        mve = core in ("m52", "m55", "m85")
        if not mve:
            config = re.sub(r"(#define\s+OS_CONFIG_TICK_SOURCE\s+)\S+", r"\g<1>OS_CONFIG_TICK_SOURCE_EXTERNAL", config)
        (directory / "os_config.h").write_text(config)
        assembly = directory / "port.s"
        gcc = tool(args.toolchain, "arm-none-eabi-gcc")
        cpu = "cortex-" + core + ("+nofp" if mve else "")
        flags = ["-mcpu=" + cpu, "-mthumb", "-mfloat-abi=softfp", "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Wundef", "-Werror", "-I" + str(directory), "-I" + str(root),
                 "-I" + str(root / "arch/arm" / ("cortex_" + core))]
        source = root / "arch/arm" / ("cortex_" + core) / "os_arch_port.c"
        subprocess.run([str(gcc), *flags, "-c", str(source), "-o", str(directory / "port.o")], check=True)
        subprocess.run([str(gcc), *flags, "-S", str(source), "-o", str(assembly)], check=True)
        if mve:
            code = assembly.read_text()
            assert re.search(r"vstm(?:db|ia).*s16.*s31", code), core
            assert re.search(r"vldm(?:db|ia).*s16.*s31", code), core
        results.append({"test": core + (" integer MVE context" if mve else " external tick"), "result": "PASS"})
        if core == "m55":
            small = re.sub(r"(#define\s+OS_CONFIG_MIN_STACK_SIZE\s+)\S+", r"\g<1>128U", config)
            (directory / "os_config.h").write_text(small)
            rejected = subprocess.run([str(gcc), *flags, "-c", str(source), "-o", str(directory / "invalid.o")], capture_output=True, text=True)
            assert rejected.returncode and "OS_CONFIG_MIN_STACK_SIZE must be at least 256" in rejected.stderr
            (directory / "os_config.h").write_text(config)
            results.append({"test": "MVE insufficient stack rejected", "result": "PASS"})
    if args.riscv_toolchain:
        directory = build / "rv32"
        directory.mkdir(exist_ok=True)
        config = re.sub(r"(#define\s+OS_CONFIG_CORE_COUNT\s+)\S+", r"\g<1>2U", base)
        (directory / "os_config.h").write_text(config)
        gcc = tool(args.riscv_toolchain, "riscv32-unknown-elf-gcc")
        source = root / "arch/riscv/hazard3/os_arch_port.c"
        flags = ["-march=rv32imac_zicsr_zifencei", "-mabi=ilp32", "-DOS_CONFIG_SPINLOCK_SOC_BACKEND=0", "-std=c11", "-O2", "-Wall", "-Wextra", "-Wundef", "-Werror",
                 "-I" + str(directory), "-I" + str(root), "-I" + str(root / "arch/riscv/hazard3")]
        assembly = directory / "port.s"
        subprocess.run([str(gcc), *flags, "-c", str(source), "-o", str(directory / "port.o")], check=True)
        subprocess.run([str(gcc), *flags, "-S", str(source), "-o", str(assembly)], check=True)
        code = assembly.read_text()
        begin = code.index("call    os_arch_scheduler_stack_get")
        switch = code.index("mv      sp, a0", begin)
        publish = code.index("call    os_task_stack_save_current", begin)
        assert begin < switch < publish
        assert "call    os_arch_scheduler_stack_set" in code
        results.append({"test": "RV32 assembled scheduler stack before publication", "result": "PASS"})
    (build / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))

if __name__ == "__main__":
    main()

