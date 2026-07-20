# AhuraRTOS

A small preemptive real-time operating system for ARM Cortex-M, built around a clean core, a single public header, and an explicit application/kernel boundary — no editable kernel files, no hidden config.

The kernel itself lives in [`ahura_kernel`](https://github.com/AhuraRTOS/ahura_kernel), included here as the `kernel` submodule. This repository is the project umbrella: overview, roadmap, and licensing. For API details, configuration, and integration steps, see [`kernel/README.md`](kernel/README.md) — it's the authoritative reference.

> **Status:** early and under active development. The kernel is functional and self-testing across the Cortex-M range, but APIs may still change. Not yet recommended for production use.

This phase focuses on **ARM Cortex-M for the STM32 series** — that's where the kernel, ports, and testing effort are concentrated right now. Support for other popular MCU families (ESP32, RISC-V, NXP, TI, etc.) is planned for later phases; see the roadmap below.

## Highlights

- **Preemptive, priority-based scheduler** — O(1) list-based scheduling with one FIFO ready list per priority, a ready bitmap for O(1) next-task lookup, and round-robin among equal priorities.
- **Broad Cortex-M coverage** — ports for ARMv6-M through ARMv8.1-M: M0, M0+, M3, M4, M7, M23, M33, M35P, M52, M55, M85.
- **Standard IPC/sync primitives** — mutexes, semaphores, queues, and event groups, all with `timeout_ms` waits (try-once, timed, or forever).
- **Software timers and a deferrable work queue**, Zephyr-style, running on dedicated kernel service tasks.
- **Kernel heap and fixed-block memory pools** — a coalescing first-fit allocator plus O(1) pools for hot fixed-size objects.
- **TrustZone support (ARMv8-M)** — secure, non-secure, and disabled modes, with weak callbacks for secure-context banking.
- **Multi-core scheduling (experimental)** — per-task core affinity across shared ready lists; not yet run on real multi-core silicon.
- **Built-in self-test suite** — a standalone module that exercises every enabled feature and reports PASS/FAIL over `printf`, so a board bring-up can validate the port with no application code at all.
- **Single public header** (`ahura.h`) and a single application-owned config file (`os_config.h`, copied from a template) — the kernel ships no configuration of its own.

## Getting started

1. Add the kernel as a submodule (already the case in this repo — see `.gitmodules`):

   ```bash
   git submodule update --init --recursive
   ```

2. Copy `kernel/os_config_template.h` into your project as `os_config.h` and point the kernel build at it:

   ```cmake
   set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)
   add_subdirectory(kernel)
   ```

3. Copy `kernel/os_cb_template.c` (platform callbacks) and `kernel/os_main_template.c` (default task body) into your application source tree as `os_cb.c` and `os_main.c`, and add both to your **application** build.

4. Route `SysTick_Handler` to `os_tick_handler()`, call `os_init()` after clocks are configured, then `os_start()`.

Full configuration options, the integration checklist, task-priority rules, and every module's API are documented in [`kernel/README.md`](kernel/README.md).

## Repository layout

```
AhuraRTOS/
├── kernel/     ← ahura_kernel submodule (core, arch ports, self-test suite)
├── LICENSE
└── README.md   ← this file
```

## Roadmap

**Phase 1 — STM32 first**
Cortex-M core kernel, examples, and a minimal portable HAL.

**Phase 2 — Expand versatility**
Ports for more MCU families (ESP32, RISC-V, NXP, TI, etc.), modular driver interfaces, consistent cross-platform APIs.

**Phase 3 — Ecosystem & tools**
Configuration and build tooling, optional modules (filesystem, additional IPC), community-driven extensions.

Known gaps tracked for later: mutex priority inheritance, tickless idle (implemented but not yet wired into the idle task), and multi-core validation on real hardware.

## Contributing

Contributions are welcome — kernel work, new ports, testing, and documentation all help. Open an issue or submit a pull request.

## License

MIT — see [LICENSE](LICENSE).
