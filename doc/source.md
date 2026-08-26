# Source layout

Where everything is, and why it is there. The tree separates the three
questions a port asks, so each one is answered in exactly one place:
`kernel/` never changes, `arch/` changes with the instruction set, `soc/`
changes with the chip.

```text
AhuraRTOS/
├── CMakeLists.txt   <- the whole build: SoC dispatch, arch detection, ahura_kernel
├── ahura.h          <- the single public header; applications include only this
├── kernel/          <- the portable core, mapped below
├── arch/            <- the port layer, <family>/<core>: arm/ and riscv/
├── soc/             <- optional per-silicon packages, <vendor>/<family>
├── template/        <- os_config.h, os_cb.c, os_main.c, soc_cb.c - copied into a project
├── test/            <- the self-test suite, built as its own os_test library
├── examples/        <- one runnable os_main.c per feature
├── doc/             <- every documentation page, this one included
└── tools/           <- the one-command installers
```

`ahura.h` and `CMakeLists.txt` sit at the top because they are what the outside
world touches: an application adds one directory to its build and includes one
header. Everything below them is the kernel's own business.

## `kernel/` - the portable core

The architecture-independent half of the kernel: plain C11 with no CPU
knowledge in it at all. Everything a CPU changes - the context switch, the
tick, critical sections, atomics, low-power entry - lives one directory over,
in [`../arch/`](../arch/).

```text
kernel/
├── os_kernel.c      <- lifecycle: os_init, os_start, the scheduler lock
├── os_task.c        <- the TCB pool, the ready lists and the O(1) scheduler
├── os_tick.c        <- the tick counter and tick handler
├── os_delay.c       <- blocking ms/s delays and the us busy-wait
├── os_critical.c    <- the nesting critical section
├── os_mutex.c       <- mutexes, with priority inheritance
├── os_semaphore.c   <- counting semaphores
├── os_queue.c       <- queues, static or heap-backed
├── os_msg.c         <- message buffers: whole messages of varying length
├── os_event.c       <- event groups
├── os_notify.c      <- direct-to-task notifications
├── os_timer.c       <- software timers and deferred calls
├── os_mem.c         <- the kernel heap: first-fit with coalescing
├── os_log.c         <- the buffered log and its drain task
├── os_atomic.c      <- the validating wrapper over the port's atomics
├── os_list.c        <- the intrusive list the scheduler itself runs on
└── os_internal.h    <- the internal cross-module contract, not for applications
```

`os_internal.h` is deliberately not on any include path. The files here reach
it because a quoted include searches the including file's own directory first;
an application linking `ahura_kernel` gets [`../ahura.h`](../ahura.h) and
nothing else.

## Where the rest is documented

| Directory | Its page |
|---|---|
| `arch/` | [How the kernel works → the port layer](design.md) |
| `soc/` | [SoC packages](soc.md), then one page per part |
| `template/` | [What the kernel needs from a platform](integration.md) |
| `test/` | [Testing and examples](testing.md), [Self-test suite](self-test.md) |
| `examples/` | [Examples](examples.md) |
| `tools/` | [Installation](installation.md) |

**📖 The full kernel reference - every API, every `os_config.h` option, how the
scheduler, context switch and priority inheritance actually work - is
[`doc/kernel.md`](kernel.md).**

To get this into your own project, start at
[Installation](installation.md).

## License

GNU GPL v3 (or later) - every source file carries `SPDX-License-Identifier: GPL-3.0-or-later`
in its header.
See the [project LICENSE](../LICENSE).
