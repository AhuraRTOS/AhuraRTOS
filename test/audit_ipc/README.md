# IPC audit regressions

`run.py` compiles the actual message, logging, and list implementations with Arm
GNU GCC and executes them as Thumb code in Unicorn. It injects deterministic
wait/wake schedules through scheduler substitutions. This tests the IPC decisions
and ring operations; it does not validate scheduler context switching, real
multicore interleavings, target hardware, or logging's libc formatter.

Install Python `unicorn`, then run:

```text
python test/audit_ipc/run.py --toolchain <Arm-GNU-bin> --build <temporary-directory>
```

Use `--python-deps <directory>` for a local `pip install --target` installation.
The runner selects executable names for Windows or POSIX. It writes the linked
image, generated configuration and `results.json` into the requested build folder.

Coverage: two blocked small producers completing after a single large receive;
an eligible small producer behind an oversized producer; competing producer retry;
undersized receiver wake relay; remaining-message receiver wake relay; cumulative
log drops across drain and a new producer burst during notice output; C++17
umbrella-header compilation with message support both enabled and disabled.
The drop notice is also checked with the producer ring full at emission time.
The maximum payload records all 65,537 required bytes, including its header.
