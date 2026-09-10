# Architecture and timekeeping regressions

These standalone tests exercise production sources without changing the kernel
folder structure. They are separate from the six board self-test builds.

| Runner | Coverage | Result on 2026-09-09 |
|---|---|---|
| `run.py` | Real `os_tick.c`, `os_critical.c`, `os_delay.c` with controlled mask/core/IPI callbacks; late remote clock reads and raw lock entry, owner migration, sleep close, usage writer/reset synchronization, masked delays and unsupported counter, tick-phase arithmetic | 8 PASS |
| `port_run.py` | Real Cortex-M33 port and shared Arm adapter; early residual reload, pending open, initial zero CVR, full window, 100 partial LIGHT windows, missed boundaries/pending ISR credit, external-tick ownership, shared SMP counter with DWT present | 8 PASS |
| `compile_run.py` | Integer-only MVE on M52/M55/M85 including actual assembly, minimum stack rejection, M0/M33 external tick; optional RV32 assembled scheduler-stack handoff order | 7 PASS with RV32 option |
| `soc_compile_run.py` | Real Pico SDK SoC compilation with tickless on/off and one/two cores; object symbols prove multicore callbacks and ordinary delay counters remain available in the required profiles | 12 combinations + 3 missing-setting checks PASS across the three RP packages |

`run.py` reuses the instruction runner in `test/audit_ipc/run.py`. It executes
actual compiled C under Unicorn. Its peer execution occurs at explicit lock/IPI
hooks; it does not execute two CPU cores simultaneously. The delay test supplies
a real-counter contract with controlled values and never services a tick ISR.

`port_run.py` executes actual port C with deliberately controlled register
values. It records the first LOAD value at each SysTick enable and checks the
ordinary reload is restored. External-tick and SMP counter tests leave SysTick
or DWT MMIO unmapped, so an unintended access fails execution. The register model
is not a cycle-accurate SysTick peripheral simulation. Cortex-M0 and RV32 busy-wait
integration is additionally compiled/linked in the existing board builds.

The harness linkers deliberately place test code/data in one emulated segment;
GNU ld reports an RWX-segment warning for those harnesses. The six final board
build logs were warning-free. Harness warnings do not represent executable
permissions configured on a board.

## Run

Requirements: Python 3.10+, Unicorn (`python -m pip install unicorn`), Arm GNU
Toolchain, and optionally the RP RISC-V toolchain. Both `.exe` and unsuffixed
compiler executable names are supported. Run from the RTOS root; use disposable
output directories outside production source folders.

```powershell
python -B test/audit_arch/run.py --toolchain C:/path/to/arm/bin --build C:/temp/ahura-timing
python -B test/audit_arch/port_run.py --toolchain C:/path/to/arm/bin --build C:/temp/ahura-port
python -B test/audit_arch/compile_run.py --toolchain C:/path/to/arm/bin --riscv-toolchain C:/path/to/riscv/bin --build C:/temp/ahura-compile
```

The first two runners also accept `--python-deps` for a local `pip --target`
installation. Each runner writes `results.json` in its output directory. The
local validation used Arm GCC 15.2.1, RISC-V GCC 15.2.0 and Unicorn 2.1.4.

The SoC regression uses a configured Pico SDK project's compiler and include paths.
Run it once for each RP2040, RP235x Arm and RP235x RISC-V project:

```powershell
python -B test/audit_arch/soc_compile_run.py --compile-commands C:/path/to/project/build/compile_commands.json --build C:/temp/ahura-soc
```

It tests the RTOS checkout containing this script. It creates isolated configurations
and leaves the supplied project's files unchanged. Tickless-off cases omit the unused
sleep mode entirely; tickless-on cases also check that a missing mode is diagnosed.
These source/object checks supplement full firmware links. The follow-up recorded in
`AUDIT.md` includes all six complete projects with tickless enabled and disabled, plus
Pico 2 Arm single-core builds in both modes (14 complete firmware builds).

## Scope and target acceptance

No hardware was flashed or executed by these tests. On-target migration with
deep live stacks, MVE task register patterns, ISR/critical-section delays measured
against an independent timer, long-duration reference-clock drift, and SMP wake
latency remain hardware acceptance work. Runtime results must name the exact
board, build, clock and sleep configuration.

The partial-first-reload sequence was conceptually cross-checked against the
[FreeRTOS Cortex-M33 port](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/portable/GCC/ARM_CM33/non_secure/port.c)
(`vPortSuppressTicksAndSleep`, restart with the partial LOAD then restore the
ordinary LOAD). No FreeRTOS source code was copied into these changes. Register
programming and integer arithmetic tests do not prove a zero-drift hardware clock;
short counter-stop overhead and physical clock tolerance still need measurement.

The new port contracts are documented in `doc/porting.md`: RV32 boot-stack
lifetime, IRQ-independent shared counters for SMP, explicit unavailable-counter
failure, reference-timer capability selection, and bounded sleep/IPI callbacks
which cannot wait for peer kernel progress during an open window.

## RP235x Arm coordinated DEEP regression

Run against a configured Pico 2 Arm project's compilation database:

```powershell
python -B test/audit_arch/smp_deep_run.py --compile-commands <pico2-arm-build>/compile_commands.json --python-deps <unicorn-package-directory> --build <temporary-output>
```

The harness includes the production `soc_cb.c` and its private peripheral checks,
using the installed SDK headers and two Unicorn CPU contexts with shared RAM.
It exercises request cancellation, late acknowledgements, busy-peer LIGHT fallback,
masked peer IRQ relay, mask/SysTick/SCR restoration, PLL/clock restoration before
peer release, and peripheral/board vetoes. The timer, NVIC events and clock-ready
transitions are controlled; memory ordering and hardware timing/power still need
board tests. The kernel timing runner also checks prepare-before-window ordering,
deadline changes during preparation, declined and too-short passes, and release
only after time announcement and reopening remote kernel entry.
