# AhuraRTOS

A small preemptive real-time operating system for ARM Cortex-M, built around a
clean core, a single public header, and an explicit application/kernel boundary
— no editable kernel files, no hidden config.

> **Status:** early and under active development. The kernel is functional and
> self-testing across the Cortex-M range, but APIs may still change. Not yet
> recommended for production use.

This repository is the project **umbrella**: overview, roadmap, and licensing.
The kernel itself lives in [`ahura_kernel`](https://github.com/AhuraRTOS/ahura_kernel),
included here as the `kernel` submodule.

📖 **For API details, configuration, and integration steps, see
[`kernel/README.md`](kernel/README.md) — it is the authoritative reference.**

---

## Contents

[Highlights](#highlights) ·
[Getting started](#getting-started) ·
[Repository layout](#repository-layout) ·
[Platform focus](#platform-focus) ·
[Roadmap](#roadmap) ·
[Contributing](#contributing) ·
[License](#license)

---

## Highlights

### Scheduling

- **Preemptive, priority-based scheduler** — O(1) list-based scheduling with one
  FIFO ready list per priority, a ready bitmap for O(1) next-task lookup, and
  round-robin among equal priorities.
- **31 priority levels**, with the idle task and the kernel service tasks at
  reserved ends of the range.

### Synchronization & IPC

- **Mutexes with priority inheritance** — always on, the way FreeRTOS and Zephyr
  do it; correct even when one task holds several contended mutexes at once.
- **Counting semaphores, queues, and event groups**, all with `timeout_ms` waits
  (try-once, timed, or forever).
- **Task notifications** — a lightweight single-value mailbox built into each
  task's own control block, so one task or an ISR can signal a specific task
  without allocating a separate object.

### Time & deferred work

- **Software timers** (one-shot and periodic) and a **deferrable work queue**,
  Zephyr-style, each running on its own dedicated kernel service task.
- Millisecond, second, and cycle-accurate microsecond delays.

### Memory & diagnostics

- **Optional kernel heap** — a coalescing first-fit allocator (comparable to
  FreeRTOS `heap_4`) over a static array, compiled out entirely when unused.
- **Stack watermarking and CPU-load sampling**, both opt-in and near-zero
  overhead.

### Portability

- **Broad Cortex-M coverage** — ARMv6-M through ARMv8.1-M (M0, M0+, M3, M4, M7,
  M23, M33, M35P, M52, M55, M85) across just three shared port implementations.
- **TrustZone support (ARMv8-M)** — secure, non-secure, and disabled modes, with
  weak callbacks for secure-context banking.
- **Multi-core scheduling (experimental)** — per-task core affinity across shared
  ready lists; not yet run on real multi-core silicon.
- **Zero mandatory HAL/CMSIS dependencies.**

### Build & verification

- **Single public header** (`ahura.h`) and a single application-owned config file
  (`os_config.h`, copied from a template) — the kernel ships no configuration of
  its own.
- **Every feature is a compile-time switch** — `OS_CONFIG_<FEATURE>_ENABLE`
  removes unused code, RAM, and API surface entirely, not just at runtime.
- **Built-in self-test suite** — a standalone module that exercises every enabled
  feature and reports PASS/FAIL over `printf`, so a board bring-up can validate
  the port with no application code at all. It finishes with a cycle-accurate
  **benchmark table** for every hot kernel path.

## Getting started

1. Add the kernel as a submodule (already the case in this repo — see `.gitmodules`):

   ```bash
   git submodule update --init --recursive
   ```

2. Copy `kernel/os_config_template.h` into your project as `os_config.h` and
   point the kernel build at it:

   ```cmake
   set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)
   add_subdirectory(kernel)
   ```

3. Copy `kernel/os_cb_template.c` (platform callbacks) and
   `kernel/os_main_template.c` (default task body) into your application source
   tree as `os_cb.c` and `os_main.c`, and add both to your **application** build.

4. Route `SysTick_Handler` to `os_tick_handler()`, call `os_init()` after clocks
   are configured, then `os_start()`:

   ```c
   os_init();
   os_start();   /* never returns */
   ```

   `os_init()` has already created and started a default application task, so
   there is nothing else to create just to get moving — write your code in
   `os_main()`.

Full configuration options, the integration checklist, task-priority rules, and
every module's API are documented in [`kernel/README.md`](kernel/README.md).

## Repository layout

```text
AhuraRTOS/
├── kernel/     ← ahura_kernel submodule (core, arch ports, self-test suite)
├── LICENSE
└── README.md   ← this file
```

## Platform focus

This phase focuses on **ARM Cortex-M**, with the STM32 series as the primary
bring-up and testing target — that is where the kernel, ports, and testing
effort are concentrated right now. Nothing in the kernel is STM32-specific: it
has no mandatory HAL or CMSIS dependency, and the platform touchpoints (CPU
clock, sleep hooks, multi-core glue) are all weak callbacks the application
overrides.

Support for other MCU families (ESP32, RISC-V, NXP, TI, …) is planned for later
phases — see the roadmap.

## Roadmap

| Phase | Focus |
|---|---|
| **1 — Cortex-M first** *(in progress)* | Core kernel, architecture ports, examples, and a minimal portable HAL. |
| **2 — Expand versatility** *(planned)* | Ports for more MCU families (ESP32, RISC-V, NXP, TI, …), modular driver interfaces, consistent cross-platform APIs. |
| **3 — Ecosystem & tools** *(planned)* | Configuration and build tooling, optional modules (filesystem, additional IPC), community-driven extensions. |

**Known gaps** tracked for later:

- Tickless idle is implemented for the ARMv8-M mainline port but is not yet
  wired into the idle task; the ARMv6-M and ARMv7-M ports still need the same
  change.
- Multi-core (SMP) scheduling compiles and is exercised in CI, but has not run
  on real multi-core silicon.
- Mutex priority inheritance is single-level: it does not propagate through a
  chain of nested mutexes held by different tasks.

## Contributing

Contributions are welcome — kernel work, new ports, testing, and documentation
all help. Open an issue or submit a pull request.

## License

MIT — see [LICENSE](LICENSE).
