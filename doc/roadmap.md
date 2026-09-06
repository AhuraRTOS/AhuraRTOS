# Roadmap

[← Documentation index](README.md)

| Phase | Focus |
|---|---|
| **1. Cortex-M first** *(in progress)* | Core kernel, architecture ports, examples, and a minimal portable HAL. |
| **2. Expand versatility** *(in progress)* | Ports for further architectures - RV32 ships, Xtensa/ESP32 is next - modular driver interfaces, consistent cross-platform APIs. |
| **3. Ecosystem and tools** *(planned)* | Configuration and build tooling, optional modules such as a filesystem and additional IPC, community-driven extensions. |

## Known gaps

Tracked deliberately, and stated here rather than discovered later:

- **An application-owned tick suppresses nothing.** Tickless idle itself is
  finished on all four ports and all four SoC packages - see [Tickless
  idle](tickless.md) - but with `OS_CONFIG_TICK_SOURCE_EXTERNAL` the timer
  belongs to the application, and the callback pair cannot yet express
  suppressing a timer the kernel does not own. Such a build still runs the whole
  tickless pass and still honours every deadline; the sleep is a plain `WFI`.
- **Priority inheritance does not reposition a queued task.** The boost itself is
  transitive and immediate, but a task already queued on some *other* object -
  a semaphore, a queue - keeps the place it had until it is woken.
- **IAR EWARM is not supported.** The port layer needs GCC-style inline
  assembly; the portable `kernel/` tree would build anywhere, so this is a
  contained piece of work confined to four files.

## Status

Early and under active development. The kernel is functional and self-testing
across the Cortex-M range and on RV32, with dual-core SMP verified on three
targets, but APIs may still change. **Not yet recommended for production
use.**

Contributions are welcome - kernel work, new ports, testing, and documentation
all help. Open an issue or submit a pull request.
