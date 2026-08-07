# AhuraRTOS

A small preemptive real-time operating system built around a clean,
architecture-independent core, a single public header, and an explicit boundary
between the application and the kernel. There are no editable kernel files and
no hidden configuration.

**The kernel is not tied to any one architecture.** Everything a CPU changes -
the context switch, the tick, critical sections, atomics, low-power entry - is
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

📖 **For API details, configuration, and integration steps, see the
[kernel README](https://github.com/AhuraRTOS/ahura_kernel/blob/main/README.md).
That is the authoritative reference.** (In a local clone it is `kernel/README.md`,
once `git submodule update --init` has fetched it.)

---

## Contents

[Highlights](#highlights) ·
[Installation](#installation) ·
[Running the self-test suite](#running-the-self-test-suite) ·
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
  reserved ends of the range. The kernel's own service tasks refuse `os_task_pause`
  and `os_task_delete`, so an application cannot stop the timer, work or log
  service out from under the APIs built on it.
- **Configurable time slice.** `OS_CONFIG_TIME_SLICE_TICKS` sets how long a task
  holds the CPU before an equal-priority peer takes over - every tick by default,
  or 0 to turn rotation off entirely. Longer slices mean proportionally fewer
  context switches, and a tick that would only have rotated costs a bitmap check
  instead of a full context-switch round trip.
- **Scheduler lock.** `os_kernel_lock()` defers preemption *without masking a
  single interrupt*, which is what a critical section cannot do: the tick and
  every driver keep running, and only the scheduler is held back until the
  outermost unlock. The right barrier for task-to-task data; interrupt-shared
  data still wants a critical section.

### Synchronization and IPC

- **Mutexes with priority inheritance**, always on rather than opt-in. A lower-
  priority owner is boosted to the priority of the task waiting on it, and the
  accounting stays correct even when one task holds several contended mutexes at
  once.
- **Counting semaphores, queues, and events**, all with `timeout_ms`
  waits: try once, wait a while, or wait forever.
- **Task notifications.** A lightweight single-value mailbox built into each
  task's own control block, so one task or an ISR can signal a specific task
  without allocating a separate object.

### Time and deferred work

- **Software timers** (one-shot and periodic) and a **deferrable work queue**,
  each running on its own dedicated kernel service task, so callbacks and
  handlers run in task context rather than in the tick interrupt.
- Millisecond, second, and cycle-accurate microsecond delays.

### Memory and diagnostics

- **Optional kernel heap.** A first-fit allocator over a static array, with an
  address-ordered free list and coalescing of adjacent blocks, compiled out
  entirely when unused. Nothing is taken from the linker heap.
- **Stack watermarking and CPU-load sampling**, both opt-in and close to free at
  runtime.

### Portability

- **Architecture-independent core.** Scheduler, IPC, timers, work queue and heap
  are plain portable C. A port supplies only what the CPU decides: the context
  switch, the tick, the critical-section mask, the atomic operations, and the
  low-power hooks. Nothing above that layer knows which CPU it is running on.
- **A one-vector footprint.** The kernel takes over PendSV and nothing else.
  `SVC` is left entirely to the application - the first task starts through
  PendSV instead - which keeps the kernel compatible with everything that
  legitimately wants `SVC` for itself: Nordic's SoftDevice, TF-M and other
  secure firmware, vendor bootloaders and ROM APIs. The tick is a single
  application call, and its timer is configurable, so parts whose SysTick stops
  in low-power modes are first-class rather than special cases.
- **Misintegration fails loudly.** The kernel checks at boot that the vector
  table really routes PendSV to it, and traps at the cause if not. The
  alternative - the usual one - is a board that reaches `os_start()` and stops
  dead with no fault and nothing to attach a debugger to.
- **Broad Cortex-M coverage today.** ARMv6-M through ARMv8.1-M (M0, M0+, M3, M4,
  M7, M23, M33, M35P, M52, M55, M85) across just three shared port
  implementations - evidence that the port interface is small enough for one
  file to serve a whole architecture family.
- **Where the instruction set differs, the port decides.** Cores with exclusive
  load/store get lock-free atomics; cores without them fall back to a critical
  section. Both are inside the port, so the API and its behaviour are identical
  either way.
- **Other architectures planned**, RISC-V and Xtensa (ESP32) first. See the
  roadmap below.
- **TrustZone support on ARMv8-M**, in secure, non-secure, or disabled mode,
  with application callbacks for banking secure contexts.
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

## Installation

**The kernel takes over exactly one exception - PendSV - and asks the
application for exactly one thing: a periodic call to `os_tick_handler()`.** It
claims no `SVC_Handler`, no `SysTick_Handler`, no HAL, and no vendor headers.
That is the whole integration contract, and it is why the same kernel drops onto
an STM32, an nRF52 and an LPC without changing anything but a config file.

### Step 1 - get the source

As a submodule of your own project (recommended, keeps updates a `git pull`
away):

```bash
git submodule add https://github.com/AhuraRTOS/ahura_kernel.git AhuraRTOS/kernel
git submodule update --init --recursive
```

Or clone this umbrella repo, which already wires up the kernel and the examples
(see `.gitmodules`):

```bash
git clone https://github.com/AhuraRTOS/AhuraRTOS.git
cd AhuraRTOS
git submodule update --init --recursive
```

A plain copy of the `kernel/` directory works too - there is nothing
git-specific about the build.

### Step 2 - copy three files into your project

The kernel deliberately compiles none of these. Two are your code, one is your
configuration. Their locations do not matter, only that the build can see them.

| Copy this template | into your project as | and add it to |
|---|---|---|
| `kernel/os_config_template.h` | `os_config.h` | nothing - it is a header |
| `kernel/os_cb_template.c` | `os_cb.c` | your **application** build |
| `kernel/os_main_template.c` | `os_main.c` | your **application** build |

`os_config.h` is every build-time option at its default value - edit it in
place. `os_cb.c` holds the platform callbacks (assert reporting, log transport).
`os_main.c` is where your application code goes.

### Step 3 - add the kernel to the build

`OS_CONFIG_DIR` must be set **before** `add_subdirectory`, so the kernel library
and your application compile against the same configuration. If only the
application saw `os_config.h`, their structure sizes would silently disagree.

```cmake
set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)   # wherever your copy lives
add_subdirectory(AhuraRTOS/kernel)

target_sources(my_firmware PRIVATE
    Core/Src/os_cb.c
    Core/Src/os_main.c
)
target_link_libraries(my_firmware ahura_kernel)
```

**Not using CMake?** Nothing here requires it. The kernel is plain C11 with
GCC-style inline assembly, no generated sources and no build-time code
generation, so Keil, IAR-style project files, MPLAB X, SEGGER Embedded Studio or
a hand-written Makefile all work. Compile every `core/*.c` plus **one**
`arch/arm/<core>/os_arch_port.c` matching your device, and add three include
paths. The kernel README's "Adding the kernel to a build" lists them exactly.

### Step 4 - give the kernel its tick

On a stock CMSIS device that is one line, anywhere in your interrupt file:

```c
void SysTick_Handler(void) { os_tick_handler(); }
```

Where SysTick is unavailable or already taken - Nordic nRF5x, whose SysTick
stops in sleep, is the classic case - set `OS_CONFIG_TICK_SOURCE_EXTERNAL` in
`os_config.h` and drive `os_tick_handler()` from any timer you like instead.

If a vendor HAL also wants SysTick, move one of them rather than sharing the
handler. On STM32 that is CubeMX → SYS → Timebase Source → any spare timer.

### Step 5 - make sure nothing else defines `PendSV_Handler`

Usually nothing does, and there is nothing to do. The one common exception is a
vendor IDE that generates an empty stub - **STM32CubeMX does**, and it is a
checkbox: NVIC → Code generation → clear "Generate IRQ handler" for *Pendable
request for system service*.

The kernel verifies the live vector table at boot and traps immediately if it
was not wired up, instead of hanging silently.

### Step 6 - boot it

From `main()`, after the clock tree is configured:

```c
int main(void)
{
    SystemClock_Config();      /* whatever your device needs */

    os_init();
    os_start();                /* never returns */
}
```

`os_init()` has already created and started a default application task, so there
is nothing else to create just to get moving. Write your code in `os_main()`:

```c
void os_main(void)
{
    while (1)
    {
        my_led_toggle();
        os_delay_ms(500U);
    }
}
```

### If it does not build

| Message | Cause |
|---|---|
| `No os_config.h found` | Step 2 missed, or `OS_CONFIG_DIR` does not point at it |
| `os_config.h is incomplete` | An option was deleted from the copy - start again from the template |
| `multiple definition of 'PendSV_Handler'` | Step 5: something else defines it, usually a vendor IDE stub |
| `undefined reference to 'os_main'` | `os_main.c` is not in the application build (step 2) |
| `undefined reference to 'os_assert_failed_cb'` (or `os_log_output_cb`) | `os_cb.c` is not in the application build, or that callback was deleted from it |
| Builds and runs, but nothing happens | Step 4: the tick is not reaching `os_tick_handler()` |

## Running the self-test suite

The kernel ships a suite that exercises every enabled feature and reports
PASS/FAIL over `printf`, finishing with a cycle-accurate benchmark table. It
needs no application code and no board support beyond a working `printf`, which
makes it the fastest way to prove a new target is correctly integrated **before**
writing anything on top of it.

Three things have to line up:

1. **Turn it on** in `os_config.h`:

   ```c
   #define OS_CONFIG_TEST_ENABLE  1U
   ```

2. **Link the suite.** `os_test()` is declared in `ahura.h` and defined only in
   this library - the kernel ships no stub, not even a weak one, so forgetting
   this fails at link time rather than booting a test build that silently tests
   nothing:

   ```cmake
   add_subdirectory(AhuraRTOS/kernel/test)
   target_link_libraries(my_firmware os_test)
   ```

3. **Let the suite own `os_log_output_cb`** (only matters when
   `OS_CONFIG_LOG_ENABLE` is 1). The suite defines that callback itself, because
   testing the log means inspecting what the kernel actually emitted, so your
   `os_cb.c` copy must step aside. The current `os_cb_template.c` already does
   this for you with:

   ```c
   #if (OS_CONFIG_LOG_ENABLE == 1U) && (OS_CONFIG_TEST_ENABLE == 0U)
   ```

   An `os_cb.c` copied before that guard existed needs the same condition added.

With the switch on, `os_init()` creates the test task **instead of** the default
application task, so the suite runs alone and `os_main()` is never called.

| Message | Cause |
|---|---|
| `undefined reference to 'os_test'` | Point 2: the test library is not linked |
| `multiple definition of 'os_log_output_cb'` | Point 3: your `os_cb.c` still defines it |

To avoid keeping points 1 and 2 in sync by hand, let CMake read the switch out
of the config so there is only one place to change:

```cmake
file(READ ${OS_CONFIG_DIR}/os_config.h _os_cfg)
if(_os_cfg MATCHES "#define[ \t]+OS_CONFIG_TEST_ENABLE[ \t]+1")
    add_subdirectory(AhuraRTOS/kernel/test)
    target_link_libraries(my_firmware os_test)
endif()
```

Re-run CMake after changing that define - a header is not a configure-time
dependency, so the build will not notice on its own.

---

Full configuration options, the integration contract, per-vendor notes,
task-priority rules, and every module's API are documented in the
[kernel README](https://github.com/AhuraRTOS/ahura_kernel/blob/main/README.md).

## Repository layout

```text
AhuraRTOS/
├── kernel/     <- ahura_kernel submodule (core, arch ports, self-test suite)
├── examples/   <- ahura_examples submodule (one runnable main per feature)
├── LICENSE
└── README.md   <- this file
```

## Platforms

AhuraRTOS is written to be multi-platform. The kernel core is architecture-
independent C, and each architecture is added as a port behind a fixed
interface - so supporting a new CPU means writing that port, not changing the
kernel.

### Available today

**ARM Cortex-M** - ARMv6-M through ARMv8.1-M: M0, M0+, M3, M4, M7, M23, M33,
M35P, M52, M55 and M85, covered by three shared port implementations, with
TrustZone support on ARMv8-M.

**Every one of those cores is supported on every silicon vendor.** Nothing in
the kernel or the ports names a vendor, a family, or a HAL. The only
device-specific symbol anywhere is CMSIS `SystemCoreClock`, and that has a
documented one-line fallback for devices whose startup code omits it. What
differs between vendors is only their *tooling* - specifically whether the code
generator emits a competing `PendSV_Handler` - and the kernel README has a short
note per vendor covering it.

Toolchains: GCC, Clang and Arm Compiler 6 (`armclang`). IAR and the end-of-life
`armcc` are not supported, because the port layer uses GCC-style inline
assembly; the portable `core/` tree would build anywhere, so an IAR port is a
contained piece of work confined to four files.

STM32 is the primary bring-up and testing target, because that is the hardware
on hand - not because anything in the kernel is STM32-specific.

### Planned

RISC-V and Xtensa (ESP32) are the next architectures, since a portable kernel
has to prove itself against a different instruction set. Note that vendor
families built on Cortex-M - NXP, TI, Nordic, Renesas and the rest - are already
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
  change, and an application-owned tick (`OS_CONFIG_TICK_SOURCE_EXTERNAL`)
  currently degrades to a plain WFI because the callback pair cannot yet express
  suppressing a timer the kernel does not own.
- Multi-core (SMP) scheduling compiles and is exercised in CI, but has not run
  on real multi-core silicon.
- Mutex priority inheritance is single-level. It does not propagate through a
  chain of nested mutexes held by different tasks.
- IAR EWARM is not supported; the port layer needs GCC-style inline assembly.

## Contributing

Contributions are welcome. Kernel work, new ports, testing, and documentation
all help. Open an issue or submit a pull request.

## License

MIT. See [LICENSE](LICENSE).
