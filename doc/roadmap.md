# Roadmap

[← Documentation index](README.md)

| Phase | Focus |
|---|---|
| **1. Cortex-M first** *(in progress)* | Core kernel, architecture ports, examples, and a minimal portable HAL. |
| **2. Expand versatility** *(planned)* | Ports for further architectures (RISC-V, Xtensa/ESP32), modular driver interfaces, consistent cross-platform APIs. |
| **3. Ecosystem and tools** *(planned)* | Configuration and build tooling, optional modules such as a filesystem and additional IPC, community-driven extensions. |

## Known gaps

Tracked deliberately, and stated here rather than discovered later:

- **Tickless idle is not wired into the idle task.** It is implemented for the
  ARMv8-M mainline port, but the ARMv6-M and ARMv7-M ports still need the same
  change, and the idle task still runs a plain `WFI` loop. An application-owned
  tick (`OS_CONFIG_TICK_SOURCE_EXTERNAL`) currently degrades to a plain WFI too,
  because the callback pair cannot yet express suppressing a timer the kernel
  does not own.
- **Multi-core (SMP) on the RP2040 has not run on real silicon.** The ARMv6-M
  SMP glue compiles and is exercised in CI, but treat it as experimental. The
  same kernel paths ARE verified on the RP2350's dual Cortex-M33, including a
  dedicated cross-core stress section in the self-test.
- **Mutex priority inheritance is single-level.** It does not propagate through
  a chain of nested mutexes held by different tasks.
- **IAR EWARM is not supported.** The port layer needs GCC-style inline
  assembly; the portable `core/` tree would build anywhere, so this is a
  contained piece of work confined to four files.

## Status

Early and under active development. The kernel is functional and self-testing
across the Cortex-M range, but APIs may still change, and no other architecture
is ported yet. **Not yet recommended for production use.**

Contributions are welcome - kernel work, new ports, testing, and documentation
all help. Open an issue or submit a pull request.
