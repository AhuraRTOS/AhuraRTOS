# AhuraRTOS

A small preemptive real-time operating system built around a clean,
architecture-independent core, a single public header, and an explicit boundary
between the application and the kernel. There are no editable kernel files and
no hidden configuration.

**The kernel is not tied to any one architecture.** Everything a CPU changes —
the context switch, the tick, critical sections, atomics, low-power entry — is
confined to a small port layer, and the rest of the kernel is ordinary portable
C. ARM Cortex-M is the first architecture ported and the one that works today;
others follow on the same interface, without touching the core.

> **Status:** early and under active development. The kernel is functional and
> self-testing across the Cortex-M range, but APIs may still change, and no
> other architecture is ported yet. Not yet recommended for production use.

This repository is the project umbrella, covering the overview, roadmap, and
licensing. The kernel itself lives in
[`ahura_kernel`](https://github.com/AhuraRTOS/ahura_kernel) and is included here
as the `kernel` submodule.

📖 **For API details, configuration, and integration steps, see
[`kernel/README.md`](kernel/README.md). That is the authoritative reference.**

---

## Contents

[Highlights](#highlights) ·
[Getting started](#getting-started) ·
[Repository layout](#repository-layout) ·
[Platforms](#platforms) ·
[Roadmap](#roadmap) ·
[Contributing](#contributing) ·
[License](#license)

---

## Highlights

### Scheduling

- **Preemptive, priority-based scheduler.** O(1) list-based scheduling with one
  FIFO ready list per priority, a ready bitmap for O(1) next-task lookup, and
  round-robin among equal priorities.
- **31 priority levels**, with the idle task and the kernel service tasks at
  reserved ends of the range.

### Synchronization and IPC

- **Mutexes with priority inheritance**, always on, the way FreeRTOS and Zephyr
  do it. It stays correct even when one task holds several contended mutexes at
  once.
- **Counting semaphores, queues, and event groups**, all with `timeout_ms`
  waits: try once, wait a while, or wait forever.
- **Task notifications.** A lightweight single-value mailbox built into each
  task's own control block, so one task or an ISR can signal a specific task
  without allocating a separate object.

### Time and deferred work

- **Software timers** (one-shot and periodic) and a **deferrable work queue** in
  the style of Zephyr, each running on its own dedicated kernel service task.
- Millisecond, second, and cycle-accurate microsecond delays.

### Memory and diagnostics

- **Optional kernel heap.** A coalescing first-fit allocator over a static
  array, comparable to FreeRTOS `heap_4`, compiled out entirely when unused.
- **Stack watermarking and CPU-load sampling**, both opt-in and close to free at
  runtime.

### Portability

- **Architecture-independent core.** Scheduler, IPC, timers, work queue and heap
  are plain portable C. A port supplies only what the CPU decides: the context
  switch, the tick, the critical-section mask, the atomic operations, and the
  low-power hooks. Nothing above that layer knows which CPU it is running on.
- **Broad Cortex-M coverage today.** ARMv6-M through ARMv8.1-M (M0, M0+, M3, M4,
  M7, M23, M33, M35P, M52, M55, M85) across just three shared port
  implementations — evidence that the port interface is small enough for one
  file to serve a whole architecture family.
- **Where the instruction set differs, the port decides.** Cores with exclusive
  load/store get lock-free atomics; cores without them fall back to a critical
  section. Both are inside the port, so the API and its behaviour are identical
  either way.
- **Other architectures planned**, RISC-V and Xtensa (ESP32) first. See the
  roadmap below.
- **TrustZone support on ARMv8-M**, in secure, non-secure, or disabled mode,
  with weak callbacks for banking secure contexts.
- **Multi-core scheduling (experimental).** Per-task core affinity across shared
  ready lists, though it has not yet run on real multi-core silicon.
- **No mandatory HAL or CMSIS dependency.**

### Build and verification

- **Single public header** (`ahura.h`) and a single application-owned config
  file (`os_config.h`, copied from a template). The kernel ships no
  configuration of its own.
- **Every feature is a compile-time switch.** `OS_CONFIG_<FEATURE>_ENABLE`
  removes unused code, RAM, and API surface entirely, not just at runtime.
- **Built-in self-test suite.** A standalone module that exercises every enabled
  feature and reports PASS/FAIL over `printf`, so a board bring-up can validate
  the port with no application code at all. It finishes with a cycle-accurate
  benchmark table covering every hot kernel path.

## Getting started

1. Add the kernel as a submodule (already the case in this repo, see
   `.gitmodules`):

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
   tree as `os_cb.c` and `os_main.c`, then add both to your **application**
   build.

4. Route `SysTick_Handler` to `os_tick_handler()`, call `os_init()` after clocks
   are configured, then `os_start()`:

   ```c
   os_init();
   os_start();   /* never returns */
   ```

   `os_init()` has already created and started a default application task, so
   there is nothing else to create just to get moving. Write your code in
   `os_main()`.

Full configuration options, the integration checklist, task-priority rules, and
every module's API are documented in [`kernel/README.md`](kernel/README.md).

## Repository layout

```text
AhuraRTOS/
├── kernel/     <- ahura_kernel submodule (core, arch ports, self-test suite)
├── LICENSE
└── README.md   <- this file
```

## Platforms

AhuraRTOS is written to be multi-platform. The kernel core is architecture-
independent C, and each architecture is added as a port behind a fixed
interface — so supporting a new CPU means writing that port, not changing the
kernel.

### Available today

**ARM Cortex-M** — ARMv6-M through ARMv8.1-M: M0, M0+, M3, M4, M7, M23, M33,
M35P, M52, M55 and M85, covered by three shared port implementations, with
TrustZone support on ARMv8-M.

STM32 is the primary bring-up and testing target, because that is the hardware
on hand — not because anything in the kernel is STM32-specific. There is no
mandatory HAL or CMSIS dependency, and the platform touchpoints (CPU clock,
sleep hooks, multi-core glue) are weak callbacks the application overrides.

### Planned

RISC-V and Xtensa (ESP32) are the next architectures, since a portable kernel
has to prove itself against a different instruction set. Note that vendor
families built on Cortex-M — NXP, TI, Nordic, Renesas and the rest — are already
covered by the ARM ports today: what a new port adds is a new *instruction set*,
not a new vendor. See the roadmap below.

## Roadmap

| Phase | Focus |
|---|---|
| **1. Cortex-M first** *(in progress)* | Core kernel, architecture ports, examples, and a minimal portable HAL. |
| **2. Expand versatility** *(planned)* | Ports for further architectures (RISC-V, Xtensa/ESP32), modular driver interfaces, consistent cross-platform APIs. |
| **3. Ecosystem and tools** *(planned)* | Configuration and build tooling, optional modules such as a filesystem and additional IPC, community-driven extensions. |

Known gaps tracked for later:

- Tickless idle is implemented for the ARMv8-M mainline port but is not yet
  wired into the idle task. The ARMv6-M and ARMv7-M ports still need the same
  change.
- Multi-core (SMP) scheduling compiles and is exercised in CI, but has not run
  on real multi-core silicon.
- Mutex priority inheritance is single-level. It does not propagate through a
  chain of nested mutexes held by different tasks.

## Contributing

Contributions are welcome. Kernel work, new ports, testing, and documentation
all help. Open an issue or submit a pull request.

## License

MIT. See [LICENSE](LICENSE).
