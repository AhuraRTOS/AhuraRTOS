# SoC packages

[← Back to the documentation index](README.md) · [Porting](porting.md) · [Platforms](platforms.md)

A SoC package supplies the part of the kernel's callback contract that is a
**fact about the silicon** rather than a decision about the product. It is
optional: without one the kernel builds exactly as it always has, and the
application writes that group itself.

```cmake
| `raspberrypi/rp2040` | RP2040 (Pico, Pico W) |
| `raspberrypi/rp235x_arm` | RP2350, RP2354 - Arm cores (Pico 2) |
add_subdirectory(AhuraRTOS/kernel)
```

That is the whole integration. The application still links `ahura_kernel` and
nothing else.

---

## Why the layer exists

Nothing in the kernel or the ports names a vendor, and that stays true. What
changed is that the callbacks a multi-core or non-CMSIS part needs used to be
hand-written into every project's `os_cb.c`, from the datasheet, every time. A
package is that work done once, in the tree, where it can be reviewed and
reused.

The seam itself is not new. The kernel already routed every SoC-specific
behaviour through `_cb` callbacks; a package fills a hole that was already
there rather than opening one.

## Where does it belong: arch, soc, or os_config?

Three homes, and one question each. Ask them in order and the answer is
usually forced:

| Home | The question | The test |
|---|---|---|
| `kernel/arch/` | **What instruction set?** | It changes when you swap a Cortex-M33 for a Hazard3, on the same chip |
| `kernel/soc/` | **What chip?** | It changes when you swap an RP2350 for an STM32H5, with the same core |
| `os_config.h` | **What does this product need?** | It changes between two products built on the same chip |

Worked examples, because the boundaries are not always where they first look:

- **The context switch** is `arch`. It is the instruction set's exception
  frame, and every Cortex-M shares it whoever made the silicon.
- **The core id** is `soc`. Cortex-M has no architectural core-id register at
  all, which is exactly why the kernel has to ask.
- **The PendSV vector *name*** is `soc`, though the vector itself is `arch`.
  The exception is architectural; what the startup file calls it is a property
  of the SDK that shipped with the chip.
- **The number of cores** splits across two homes, and the split is the point.
  How many the chip *has* is a `soc` fact (`SOC_CORE_COUNT`). How many the
  kernel should *schedule on* is an application decision
  (`OS_CONFIG_CORE_COUNT`) - running single-core on a dual-core chip is
  entirely reasonable. The package validates the choice against the fact.
- **Starting a secondary core** is `soc`. There is no architectural way to do
  it: it is a reset release, a mailbox handshake, a boot-address register,
  different on every chip. The kernel can say *when* and never *how*, which is
  why `os_start()` calls `os_arch_core_launch_cb()` rather than doing it.
- **TrustZone mode** is `os_config.h`. Whether the core *has* the Security
  Extension is `arch` (`OS_ARCH_HAS_TRUSTZONE`), but which state the kernel
  runs in is a product decision, and the kernel `#error`s when the two
  disagree.
- **The tick source** is `os_config.h`, defaulted by `soc`. Whether SysTick
  survives the sleep modes a chip ships with is a silicon fact, so a package
  may set `OS_CONFIG_TICK_SOURCE` through its `soc.cmake`; whether the product
  uses those modes is still the application's call, and it can override.

The rule that catches most mistakes: **if two boards using the same chip would
answer differently, it is `os_config.h`. If two chips with the same core would
answer differently, it is `soc`. Otherwise it is `arch`.**

## Who owns what

The callback contract splits in two, and the split is the whole design:

| Application owns (`template/os_cb.c`) | SoC owns (a package, or `template/soc_cb.c`) |
|---|---|
| `os_log_output_cb` - which transport? | `os_arch_core_id_get_cb` |
| `os_assert_failed_cb` - report how? | `os_arch_core_ipi_request_cb` |
| `os_stack_overflow_cb` | `os_arch_spinlock_acquire_cb` / `_release_cb` |
| | `os_arch_handler_stack_top_cb` / `_limit_cb` (multi-core) |
| | `os_arch_tick_init_cb` |
| | `os_arch_tz_context_save_cb` / `_restore_cb` |
| | `os_tickless_pre_sleep_cb` / `_post_sleep_cb` |
| | the `SystemCoreClock` symbol, where the SDK omits it |

Two configuration values move with the second group, and are therefore **not in
`os_config.h`**:

- `OS_CONFIG_ARCH_PENDSV_HANDLER` - the vector symbol name.
- `OS_CONFIG_SPINLOCK_SOC_BACKEND` - whether the spinlock uses `LDREX`/`STREX`
  or the callbacks.

### Why those two had to move

Not tidiness. `os_arch_port_common.h` includes `os_config.h`, so anything
defined in the application's configuration is seen **after** the build system's
`-D`. A stale `#define OS_CONFIG_ARCH_PENDSV_HANDLER PendSV_Handler` left in a
project's config therefore silently overrides a package that set `isr_pendsv`,
and the kernel traps at `os_start()` with the vector check pointing at a
handler that was never linked. The same shape of bug on the spinlock gives a
lock that excludes nothing.

Both keep `#ifndef` defaults in `os_arch_port_common.h`, so a project with no
package, or an older config that still defines them, builds and behaves exactly
as before.

## The one-definition rule

> Exactly one definition of each SoC-owned callback is reachable at link time.

This is the rule that makes the split safe, and the reason
`template/soc_cb.c` is a separate file rather than a section of
`template/os_cb.c`.

The linker pulls a member out of a static archive only when it defines a symbol
that is currently **undefined**. Every SoC-owned callback is called by the
kernel and defaulted nowhere, so the reference is unresolved and the definition
is always pulled in. But if an application also carries an empty weak stub -
compiled straight into the executable, not into an archive - then the symbol is
already defined by the time the linker looks, the package's real implementation
is never extracted, and the stub wins **in silence**. On SMP that reads as every
core reporting id 0, which corrupts shared state rather than failing to build.

Hence:

- A package defines the group. An application using a package does **not** copy
  `template/soc_cb.c`.
- A package's definitions are `OS_WEAK`, so an application may still override
  any single one with a normal (strong) definition in its own sources. A strong
  definition in a directly linked object beats a weak one in an archive,
  reliably.
- A package's sources are compiled into `ahura_kernel` rather than into an
  archive of their own, which is what keeps the guarantee above simple.

## Layout

```
kernel/soc/<vendor>/<family>/
├── soc.cmake        # required
├── soc.h            # only if the package exposes something to call; most do not
│
└── ../common/       # optional: code shared by sibling packages of one vendor,
                     # compiled into whichever one the build selected
├── soc_cb.c         # optional: the SoC-owned callbacks
└── README.md
```

Vendor names use the standard vendor prefixes - `raspberrypi`, `infineon`,
`nordic`, `silabs` - the same list Zephyr's `soc/` tree uses, so anyone arriving
from there already knows where to look. The family level is kept even when a
vendor has effectively one family, so `AHURA_SOC` values and the CMake dispatch
never need a special case.

Board-level detail - pin mux, LEDs, which UART is the console - is deliberately
**not** here. That is application territory, and it is where a tree like this
starts to sprawl.

## `soc.cmake`

Read by `kernel/CMakeLists.txt` **before** the `ahura_kernel` target is
finished, which is why the dispatch lives inside the kernel rather than beside
it: the PendSV name has to be known while the port's `.c` files compile, since
they paste it into naked assembly. The application never has to get an
`add_subdirectory()` order right.

Every variable is optional except the name. Unset means "the kernel's default".

| Variable | Meaning |
|---|---|
| `AHURA_SOC_NAME` | Human-readable, for the configure-time status line |
| `AHURA_SOC_PENDSV_HANDLER` | Vector symbol name, if not `PendSV_Handler` |
| `AHURA_SOC_SPINLOCK_BACKEND` | `1` to force the callback spinlock backend |
| `AHURA_SOC_ARCH` | Core folder under `arch/<family>/`, e.g. `cortex_m33` |
| `AHURA_SOC_SOURCES` | Absolute paths; use `${CMAKE_CURRENT_LIST_DIR}` |
| `AHURA_SOC_INCLUDE_DIRS` | Added `PUBLIC` to `ahura_kernel` |
| `AHURA_SOC_LINK_LIBRARIES` | Vendor SDK targets the sources need |
| `AHURA_SOC_COMPILE_DEFINITIONS` | Anything else the target requires |

## Writing one

The path is deliberately the same work twice rather than two jobs:

1. Copy `kernel/template/soc_cb.c` into an application as `soc_cb.c` and
   fill in the bodies against the target's registers. Get the board running.
2. Move the finished file to `kernel/soc/<vendor>/<family>/soc_cb.c`, add a
   `soc.cmake` stating the build facts, and write a README saying what it
   assumes and what it claims.

Step 1 alone is a complete, supported port - step 2 only makes it reusable.

Be honest in the README about what has run on silicon and what has only
compiled. The RP2 package's own README does this: on the RP2350 both
single-core and dual-core SMP have run the full self-test on silicon, while
the RP2040's ARMv6-M glue is written against the SDK's documented API and has
not run on hardware.

## Packaged today

| `AHURA_SOC` | Parts | Notes |
|---|---|---|
| `st/stm32` | Every STM32 | Contributes no code. CMSIS-Pack startup already satisfies the kernel |

An unpackaged part is not an unsupported part. Leave `AHURA_SOC` unset, copy
`template/soc_cb.c`, and the kernel behaves as it always has - which is what
keeps [every core on every vendor](platforms.md) true.
