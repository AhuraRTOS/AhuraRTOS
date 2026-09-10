"""Compile RP SoC guards against a configured project's real Pico SDK headers.

Pass its CMake compile_commands.json and an output directory. The project and
its configuration are read only; temporary configurations cover tickless on/off
and one/two cores. Run once for each RP2040/RP235x Arm/RISC-V project.
SPDX-License-Identifier: GPL-3.0-or-later
"""
from pathlib import Path
import argparse
import ctypes
import json
import os
import re
import shlex
import subprocess


def command_arguments(entry):
    if "arguments" in entry:
        return entry["arguments"]
    if os.name != "nt":
        return shlex.split(entry["command"])
    # CMake writes Windows command-line quoting, which differs from shlex.
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    shell32.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell32.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    kernel32.LocalFree.restype = ctypes.c_void_p
    count = ctypes.c_int()
    pointer = shell32.CommandLineToArgvW(entry["command"], ctypes.byref(count))
    if not pointer:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return [pointer[index] for index in range(count.value)]
    finally:
        kernel32.LocalFree(pointer)


def setting(text, name, value):
    result, count = re.subn(r"^(#define\s+" + name + r"\s+)\S+",
                            lambda match: match[1] + value, text, flags=re.M)
    if count != 1:
        raise ValueError("Expected exactly one definition: " + name)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-commands", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    entries = json.loads(args.compile_commands.read_text(encoding="utf-8"))
    entries = [entry for entry in entries
               if "/soc/raspberrypi/" in entry["file"].replace("\\", "/")]
    chip_entry = next(entry for entry in entries if Path(entry["file"]).name == "soc_cb.c")
    chip_source = Path(chip_entry["file"])
    chip = chip_source.parent.name
    previous_root = chip_source.parents[3]
    config = (root / "template/os_config.h").read_text(encoding="utf-8")
    soc_config = (root / "soc/raspberrypi" / chip / "template/soc_config.h").read_text(encoding="utf-8")
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    results = []

    def compile_source(entry, directory, label, expected_error=None):
        arguments = command_arguments(entry)
        compiler = Path(arguments[0])
        source = root / Path(entry["file"]).relative_to(previous_root)
        output = directory / (source.stem + ".o")
        command = [str(compiler), "-I" + str(directory)]
        skip = False
        for argument in arguments[1:]:
            if skip:
                skip = False
            elif argument in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
            elif argument in ("-MD", "-MMD") or argument.replace("\\", "/") == entry["file"].replace("\\", "/"):
                continue
            else:
                command.append(argument.replace(str(previous_root), str(root))
                               .replace(previous_root.as_posix(), root.as_posix()))
        command += [str(source), "-o", str(output)]
        process = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
        diagnostic = process.stdout + process.stderr
        (directory / (source.stem + ".log")).write_text(diagnostic, encoding="utf-8")
        if expected_error:
            if process.returncode == 0 or expected_error not in diagnostic:
                raise RuntimeError(label + ": expected diagnostic missing\n" + diagnostic)
            return set()
        if process.returncode:
            raise RuntimeError(label + ": compile failed\n" + diagnostic)
        nm = compiler.with_name(compiler.name.replace("gcc", "nm"))
        symbols = subprocess.run([str(nm), "--defined-only", str(output)],
                                 check=True, capture_output=True, text=True).stdout
        return {line.split()[-1] for line in symbols.splitlines() if line.split()}

    for cores in (1, 2):
        for tickless in (0, 1):
            label = f"{chip}-cores{cores}-tickless{tickless}"
            directory = build / label
            directory.mkdir(exist_ok=True)
            variant = setting(config, "OS_CONFIG_CORE_COUNT", f"{cores}U")
            variant = setting(variant, "OS_CONFIG_TICKLESS_ENABLE", f"{tickless}U")
            (directory / "os_config.h").write_text(variant, encoding="utf-8")
            # Off builds must also work with the unused sleep option absent.
            mode = soc_config if tickless else re.sub(
                r"^#define\s+SOC_CONFIG_SLEEP_MODE\s+.*$", "", soc_config, flags=re.M)
            (directory / "soc_config.h").write_text(mode, encoding="utf-8")
            for entry in entries:
                symbols = compile_source(entry, directory, label)
                if Path(entry["file"]).name == "soc_cb.c":
                    required = {"os_arch_core_ipi_request_cb"}
                    if chip != "rp235x_riscv":
                        required.add("soc_ipi_arm")
                    if cores == 2 and not required <= symbols:
                        raise RuntimeError(label + ": missing multicore callbacks: " + str(required - symbols))
                    if cores == 1 and required & symbols:
                        raise RuntimeError(label + ": multicore callbacks emitted in single-core build")
                elif Path(entry["file"]).name == "soc_common.c":
                    required = {"os_arch_reference_clock_hz_cb", "os_arch_reference_clock_get_cb"}
                    if not required <= symbols:
                        raise RuntimeError(label + ": delay reference counter missing")
                    if ("os_arch_tick_reference_clock_hz_cb" in symbols) != bool(tickless):
                        raise RuntimeError(label + ": incorrect tickless reference callback guard")
            results.append({"test": label, "result": "PASS"})

    directory = build / (chip + "-missing-sleep-mode")
    directory.mkdir(exist_ok=True)
    (directory / "os_config.h").write_text(
        setting(config, "OS_CONFIG_TICKLESS_ENABLE", "1U"), encoding="utf-8")
    (directory / "soc_config.h").write_text(re.sub(
        r"^#define\s+SOC_CONFIG_SLEEP_MODE\s+.*$", "", soc_config, flags=re.M), encoding="utf-8")
    compile_source(chip_entry, directory, "missing sleep mode",
                   "SOC_CONFIG_SLEEP_MODE is required when OS_CONFIG_TICKLESS_ENABLE is 1")
    results.append({"test": chip + " missing enabled sleep mode rejected", "result": "PASS"})
    (build / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
