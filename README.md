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

This repository is the project umbrella: **what AhuraRTOS is, and how to install
it.** The kernel itself lives in
[`ahura_kernel`](https://github.com/AhuraRTOS/ahura_kernel) and is included here
as the `kernel` submodule.

📖 **How the kernel works - the scheduler, the context switch, every API, every
configuration option - is documented in the
[kernel README](https://github.com/AhuraRTOS/ahura_kernel/blob/main/README.md).
That is the authoritative reference.** (In a local clone it is
`kernel/README.md`, once `git submodule update --init` has fetched it.) This
page keeps the kernel description brief and spends its length on installation.

---

## Contents

[What you get](#what-you-get) ·
[Installation](#installation)
([general](#general---any-platform-any-toolchain) ·
[vendor notes](#vendor-notes) ·
[STM32Cube](#stm32cubemx--stm32cubeide---step-by-step)) ·
[Running the self-test suite](#running-the-self-test-suite) ·
[Repository layout](#repository-layout) ·
[Platforms](#platforms) ·
[Roadmap](#roadmap) ·
[Contributing](#contributing) ·
[License](#license)

---

## What you get

A summary. Every line here is covered in depth in the
[kernel README](https://github.com/AhuraRTOS/ahura_kernel/blob/main/README.md).

**Scheduling.** A preemptive, priority-based scheduler with 31 priority levels,
O(1) list-based ready queues (one FIFO list per priority plus a ready bitmap),
and round-robin among equal priorities with a configurable time slice. Plus a
scheduler lock that defers preemption *without masking a single interrupt* -
the barrier a critical section cannot be.

**Synchronization and IPC.** Mutexes with always-on priority inheritance,
counting semaphores, queues, events, and a lightweight per-task notification
mailbox - all with `timeout_ms` waits: try once, wait a while, or wait forever.

**Time and deferred work.** One-shot and periodic software timers and a
deferrable work queue, each on its own kernel service task, so callbacks run in
task context rather than in the tick interrupt. Millisecond, second and
cycle-accurate microsecond delays.

**Memory and diagnostics.** An optional first-fit kernel heap with coalescing
over a static array (nothing is taken from the linker heap), stack watermarking,
stack-overflow detection, and CPU-load sampling. No dynamic allocation anywhere
in the kernel itself - every object is a statically sized array.

**Portability.** ARMv6-M through ARMv8.1-M (M0, M0+, M3, M4, M7, M23, M33, M35P,
M52, M55, M85) across just three shared port implementations, with TrustZone
support on ARMv8-M and experimental multi-core scheduling. No mandatory HAL or
CMSIS dependency. Other architectures - RISC-V, Xtensa - are planned on the same
port interface.

**A one-vector footprint.** The kernel takes over PendSV and nothing else. `SVC`
is left entirely to the application, which keeps the kernel compatible with
everything that legitimately wants it: Nordic's SoftDevice, TF-M and other
secure firmware, vendor bootloaders and ROM APIs. The tick is a single
application call, and its timer is configurable, so parts whose SysTick stops in
low-power modes are first-class rather than special cases.

**Misintegration fails loudly.** The kernel checks at boot that the vector table
really routes PendSV to it, and traps at the cause if not. The alternative - the
usual one - is a board that reaches `os_start()` and stops dead with no fault
and nothing to attach a debugger to.

**Build and verification.** A single public header (`ahura.h`) and a single
application-owned config file (`os_config.h`, copied from a template); the
kernel ships no configuration of its own. Every feature is a compile-time switch
that removes code, RAM and API surface entirely. And a built-in self-test suite
validates a fresh port over `printf` with no application code at all, finishing
with a cycle-accurate benchmark table.

## Installation

**The kernel takes over exactly one exception - PendSV - and asks the
application for exactly one thing: a periodic call to `os_tick_handler()`.** It
claims no `SVC_Handler`, no `SysTick_Handler`, no HAL, and no vendor headers.
That is the whole integration contract, and it is why the same kernel drops onto
an STM32, an nRF52 and an LPC without changing anything but a config file.

This section is in three parts:

- **[General](#general---any-platform-any-toolchain)** - the six-step procedure
  itself, independent of vendor, IDE and build system. Read this one.
- **[Vendor notes](#vendor-notes)** - the one thing that differs per vendor, and
  what to do about it on STM32, Nordic, NXP and everything else.
- **[STM32CubeMX / STM32CubeIDE](#stm32cubemx--stm32cubeide---step-by-step)** -
  the same six steps carried out on a concrete board, with the exact CubeMX
  checkboxes, file paths, CMake lines and build commands. Read it if you use ST
  tooling; skim it as a worked example if you do not.

### General - any platform, any toolchain

Paths below assume the kernel sits at `AhuraRTOS/kernel/` in your project, which
is what step 1 produces. Nothing depends on that layout - only on the build
being able to see the files.

#### Step 1 - get the source

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

#### Step 2 - copy three files into your project

The kernel deliberately compiles none of these. Two are your code, one is your
configuration. Their locations do not matter, only that the build can see them.

| Copy this template | into your project as | and add it to |
|---|---|---|
| `AhuraRTOS/kernel/os_config_template.h` | `os_config.h` | nothing - it is a header |
| `AhuraRTOS/kernel/os_cb_template.c` | `os_cb.c` | your **application** build |
| `AhuraRTOS/kernel/os_main_template.c` | `os_main.c` | your **application** build |

`os_config.h` is every build-time option at its default value - edit it in
place, and do not delete options: a missing one would read as `0` in an `#if`
and silently disable a feature, so the kernel rejects an incomplete file
instead. `os_cb.c` holds the platform callbacks (assert reporting, stack-overflow
reporting, log transport); the kernel declares them and defines none of them, so
a missing one is a link error rather than a silently empty hook. `os_main.c` is
where your application code goes.

#### Step 3 - add the kernel to the build

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

Keep those five lines together as one block, appended after your existing
target definitions rather than merged into them: `target_sources()` and
`target_link_libraries()` both *append*, so the only ordering requirement is
that the target already exists. A generator-owned `CMakeLists.txt` (CubeMX,
MCUXpresso) then stays exactly as generated, and the integration is one block to
review or remove.

The kernel library picks its own architecture port from the `-mcpu` / `-march`
in your toolchain file and prints what it chose at configure time:

```text
-- Ahura kernel arch: cortex_m33
```

Override it with `-DOS_ARCH_VARIANT=cortex_m4` if that guess is ever wrong.

**Not using CMake?** Nothing here requires it. The kernel is plain C11 with
GCC-style inline assembly, no generated sources and no build-time code
generation, so Keil, MPLAB X, SEGGER Embedded Studio or a hand-written Makefile
all work. Compile:

```text
AhuraRTOS/kernel/core/*.c                        <- all 16 files
AhuraRTOS/kernel/arch/arm/<core>/os_arch_port.c  <- exactly ONE, matching the device
```

`<core>` is one of `cortex_m0`, `cortex_m0plus`, `cortex_m3`, `cortex_m4`,
`cortex_m7`, `cortex_m23`, `cortex_m33`, `cortex_m35p`, `cortex_m52`,
`cortex_m55`, `cortex_m85`. Each carries an `#error` guard, so a mismatch with
`-mcpu` fails at compile time rather than producing a subtly wrong context
switch. **Do not add `arch/arm/common/*.c` to the build** - those are textual
includes pulled in by the wrapper above, and compiling them separately produces
duplicate symbols.

Three include paths:

```text
AhuraRTOS/kernel/                      <- ahura.h
AhuraRTOS/kernel/arch/arm/<core>/      <- os_arch_port.h
<the directory holding your os_config.h>/
```

Then add `os_cb.c` and `os_main.c` to the application. No linker-script edits,
no `OS_CONFIG_` defines from the build system - `os_config.h` is the single
source of configuration.

#### Step 4 - give the kernel its tick

`os_tick_handler()` is declared in `ahura.h`, the kernel's single public header.
On a stock CMSIS device, routing it is one line in your interrupt file:

```c
#include "ahura.h"

void SysTick_Handler(void) { os_tick_handler(); }
```

Give that interrupt the **lowest** priority the device offers - it drives
preemption, so it must never preempt an application interrupt. The port already
does this for SysTick.

Where SysTick is unavailable or already taken - Nordic nRF5x, whose SysTick
stops in sleep, is the classic case - set `OS_CONFIG_TICK_SOURCE_EXTERNAL` in
`os_config.h` and drive `os_tick_handler()` from any timer you like instead,
starting it in `os_arch_tick_init_cb()` in your `os_cb.c`.

If a vendor HAL also wants SysTick, move one of them rather than sharing the
handler - two time bases on one interrupt drift against each other. On STM32
that is CubeMX → SYS → Timebase Source → any spare timer; see the STM32 section
below.

#### Step 5 - make sure nothing else defines `PendSV_Handler`

Usually nothing does, and there is nothing to do: the port defines
`PendSV_Handler`, which is the name every CMSIS startup file already has in the
vector table. The one common exception is a vendor IDE that generates an empty
stub - **STM32CubeMX does**, and it is a checkbox; see
[Vendor notes](#vendor-notes).

If your vector table calls entry 14 something else (a hand-written startup file,
a bootloader's own table), point the kernel at that name instead:

```c
#define OS_CONFIG_ARCH_PENDSV_HANDLER  my_pendsv_vector_name
```

The kernel verifies the live vector table at boot and traps immediately if it
was not wired up, instead of hanging silently.

#### Step 6 - boot it

From `main()`, after the clock tree is configured - `os_init()` programs the
tick from the live `SystemCoreClock`, so a still-default clock would give you
the wrong tick rate:

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

Tasks you need before the scheduler runs go between the two calls; everything
else is better created from `os_main()`.

**Learning a specific feature?** `examples/kernel/` holds one standalone
`os_main_<feature>.c` per feature - mutexes, queues, events, timers, the work
queue, notifications, atomics, the heap, logging, and so on. Each one *is* an
`os_main.c`: it includes only `ahura.h` and `<stdio.h>`, needs no board support
beyond `printf`, and drops straight into the build in place of the copy from
step 2. Swap one in, build, read the console, swap the next.

#### If it does not build

| Message | Cause |
|---|---|
| `No os_config.h found` | Step 2 missed, or `OS_CONFIG_DIR` does not point at it |
| `os_config.h is incomplete` | An option was deleted from the copy - start again from the template |
| `multiple definition of 'PendSV_Handler'` | Step 5: something else defines it, usually a vendor IDE stub |
| `undefined reference to 'os_main'` | `os_main.c` is not in the application build (step 2) |
| `undefined reference to 'os_assert_failed_cb'` (or `os_stack_overflow_cb`, `os_log_output_cb`) | `os_cb.c` is not in the application build, or that callback was deleted from it |
| Duplicate symbols from the port | `arch/arm/common/*.c` was added to the build (step 3) - remove it |
| Builds and runs, but nothing happens | Step 4: the tick is not reaching `os_tick_handler()` |

### Vendor notes

Nothing in the kernel is vendor-specific, but vendor *tooling* differs in
exactly one way that matters: whether it generates an interrupt file that also
defines `PendSV_Handler`.

#### STM32 (STM32CubeMX / CubeIDE)

CubeMX generates a non-weak `PendSV_Handler` into `Core/Src/stm32<family>_it.c`,
which collides with the kernel's. Deleting it by hand works until the next code
generation overwrites the file, so turn it off at the source instead:

> **CubeMX → System Core → NVIC → Code generation tab → clear "Generate IRQ
> handler" for *Pendable request for system service*.**

That setting is stored in the `.ioc`, so regeneration keeps honouring it. Leave
*System tick timer* generating, since that is where `os_tick_handler()` goes,
and leave *System service call via SWI instruction* (`SVC_Handler`) alone - the
kernel does not use it.

Then move the HAL off SysTick, or the HAL and the kernel will fight over it:

> **CubeMX → System Core → SYS → Timebase Source → any spare timer** (TIM6,
> TIM7 and TIM17 are common picks).

CubeMX adds `stm32<family>_hal_timebase_tim.c` to the project, and from then on
`HAL_Delay()` and `HAL_GetTick()` run off that timer while SysTick belongs to
the kernel. Do **not** call `HAL_IncTick()` from `SysTick_Handler` afterwards.

Note that `HAL_Delay()` still busy-waits - it does not yield. Use
`os_delay_ms()` in task code and keep `HAL_Delay()` for driver init paths that
run before `os_start()`.

The full walkthrough is [below](#stm32cubemx--stm32cubeide---step-by-step).

#### Nordic (nRF5x, nRF53, nRF91)

Nordic's MDK startup files use the standard CMSIS names, so `PendSV_Handler`
binds with no configuration and nothing needs disabling.

The thing to get right on these parts is the tick. SysTick does not run when the
CPU sleeps, so anything using the low-power modes these devices are chosen for
will lose time. Set `OS_CONFIG_TICK_SOURCE_EXTERNAL` and drive
`os_tick_handler()` from an RTC peripheral instead, as Nordic's own software
does - `os_cb_template.c` has the skeleton.

If a SoftDevice is present, note that it owns the top of the vector table and
forwards the lower SVC range to the application. That is not a problem here,
because the kernel does not use SVC at all.

#### NXP, TI, Silicon Labs, Renesas, Microchip, Infineon, GD32

All use CMSIS-Pack startup files with the standard names, and none of their
generators emits a competing `PendSV_Handler`. Copy the three files, route the
tick, build. If a vendor RTOS abstraction is enabled in the project (MCUXpresso
with FreeRTOS selected, for instance), disable it - two RTOSes cannot both own
PendSV.

#### Anything else

Open the startup file, find the vector table, and read the name at entry 14
(offset `0x38`). If it is `PendSV_Handler`, there is nothing to do. If it is
something else, set `OS_CONFIG_ARCH_PENDSV_HANDLER` to that name. The boot-time
vector check will confirm it either way.

### STM32CubeMX / STM32CubeIDE - step by step

The same six steps on real hardware, expanded to eight because two of them are
CubeMX settings rather than code. Verified end to end on a **NUCLEO-H503RB**
(Cortex-M33) generated by CubeMX as a **CMake** project and built with
`arm-none-eabi-gcc` 14.3.1 from the STM32Cube toolchain; the only board-specific
names below are the `stm32h5xx_*` file names and `TIM7`.

ST tooling differs from every other vendor's in exactly two places, and both are
CubeMX checkboxes: it generates its own `PendSV_Handler`, and its HAL takes
SysTick for `HAL_GetTick()`. Steps 2 and 3 deal with those.

#### 1. Add the kernel to the project

From the project root (the directory holding `CMakeLists.txt` and the `.ioc`):

```bash
git submodule add https://github.com/AhuraRTOS/ahura_kernel.git AhuraRTOS/kernel
git submodule update --init --recursive
```

#### 2. CubeMX: stop generating `PendSV_Handler`

> **System Core → NVIC → Code generation tab → clear "Generate IRQ handler" for
> *Pendable request for system service*.**

CubeMX otherwise writes a non-weak empty `PendSV_Handler` into
`Core/Src/stm32h5xx_it.c`, which collides with the kernel's at link time.
Deleting it by hand works until the next code generation puts it back; the
checkbox is stored in the `.ioc`, so regeneration keeps honouring it.

Leave *System tick timer* generating - that is where `os_tick_handler()` goes in
step 6 - and leave *System service call via SWI instruction* (`SVC_Handler`)
alone, the kernel does not use it.

#### 3. CubeMX: move the HAL time base off SysTick

> **System Core → SYS → Timebase Source → TIM7** (TIM6 and TIM17 are equally
> good on parts that have them).

`HAL_Init()` otherwise claims SysTick for `HAL_GetTick()`, and the kernel needs
it. CubeMX adds `Core/Src/stm32h5xx_hal_timebase_tim.c` to the project, and from
then on `HAL_Delay()` / `HAL_GetTick()` run off TIM7 while SysTick belongs to
the kernel. Do **not** call `HAL_IncTick()` from `SysTick_Handler` afterwards.

Regenerate the code once both checkboxes are set (**Project → Generate Code**),
then confirm in `Core/Src/stm32h5xx_it.c` that `PendSV_Handler` is gone and
`SysTick_Handler` is still there.

`HAL_Delay()` still busy-waits - it does not yield. Use `os_delay_ms()` in task
code and keep `HAL_Delay()` for driver init that runs before `os_start()`.

#### 4. Copy the three files

```bash
cp AhuraRTOS/kernel/os_config_template.h Core/Inc/os_config.h
cp AhuraRTOS/kernel/os_cb_template.c     Core/Src/os_cb.c
cp AhuraRTOS/kernel/os_main_template.c   Core/Src/os_main.c
```

`Core/Inc` and `Core/Src` are only a convention - CubeMX never overwrites files
it did not generate, so anything of yours in there is safe across
regenerations.

Then fill in `os_cb.c` for the board. On a Nucleo, `printf` already reaches the
ST-LINK virtual COM port through the BSP (`USE_COM_LOG` in
`stm32h5xx_nucleo_conf.h`), so the three callbacks are short:

```c
#include "main.h"
#include "ahura.h"

void os_assert_failed_cb(const char *file, uint32_t line)
{
    printf("\r\nOS_ASSERT failed at %s:%lu\r\n", (file != NULL) ? file : "?", line);
    if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0U) { __BKPT(0); }
}

void os_stack_overflow_cb(const char *task_name)
{
    printf("\r\nStack overflow in task '%s'\r\n", (task_name != NULL) ? task_name : "?");
    if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0U) { __BKPT(0); }
}

#if (OS_CONFIG_LOG_ENABLE == 1U) && (OS_CONFIG_TEST_ENABLE == 0U)
void os_log_output_cb(const uint8_t *data, size_t length)
{
    (void)HAL_UART_Transmit(&hcom_uart[COM1], (uint8_t *)data, (uint16_t)length, HAL_MAX_DELAY);
}
#endif
```

Delete the template blocks whose feature is off in `os_config.h` - the tick
callback (SysTick is the tick here), TrustZone, multi-core and the tickless
hooks. Each is guarded by the same `#if` the kernel uses, so leaving them in
compiles fine too.

`os_main.c` is the application. To blink the user LED:

```c
#include "main.h"
#include "ahura.h"

void os_main(void)
{
    while (1)
    {
        BSP_LED_Toggle(LED_GREEN);
        os_delay_ms(500U);
    }
}
```

#### 5. Edit the top-level `CMakeLists.txt`

CubeMX generates this file once and never regenerates it, so these edits are
permanent.

You could scatter them through the generated blocks - the kernel next to
`add_subdirectory(cmake/stm32cubemx)`, the two sources under `# Add user sources
here`, the library under `# Add user defined libraries`. **Append one block at
the end of the file instead.** `target_sources()` and `target_link_libraries()`
both *append*, so a later call adds to what the generated blocks already set;
the executable target is created near the top, which is the only ordering that
matters. Everything the kernel needs then sits in one place to read, copy to the
next project, or delete, and the generated blocks stay byte-for-byte as CubeMX
wrote them:

```cmake
# ***********************************************************************************************
# AhuraRTOS
# ***********************************************************************************************

# OS_CONFIG_DIR must be set BEFORE add_subdirectory: the kernel library and the
# application have to compile against the same os_config.h.
set(OS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc)
add_subdirectory(AhuraRTOS/kernel)

# Editing os_config.h re-runs CMake by itself (see "One switch instead of two").
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OS_CONFIG_DIR}/os_config.h)

# Link the self-test suite iff os_config.h asks for it.
file(READ ${OS_CONFIG_DIR}/os_config.h _os_config_contents)
if(_os_config_contents MATCHES "#define[ \t]+OS_CONFIG_TEST_ENABLE[ \t]+1")
    message(STATUS "Ahura self-test suite: ENABLED (os_main() is not run in this build)")
    add_subdirectory(AhuraRTOS/kernel/test)
    set(AHURA_TEST_LIB os_test)
else()
    set(AHURA_TEST_LIB "")
endif()
unset(_os_config_contents)

# Application-owned kernel files: the APPLICATION build, never the kernel library.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/os_cb.c
    Core/Src/os_main.c
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    ahura_kernel
    ${AHURA_TEST_LIB}   # empty unless OS_CONFIG_TEST_ENABLE is 1, see above
)
```

That is the whole build change. The middle two stanzas are explained in [One
switch instead of two](#one-switch-instead-of-two) below; drop them and the
block still works, at the cost of switching the test suite on in two places by
hand.

#### 6. Route the tick in `Core/Src/stm32h5xx_it.c`

Put both edits **inside the `USER CODE` markers**, so CubeMX preserves them the
next time it regenerates that file:

```c
/* USER CODE BEGIN Includes */
#include "ahura.h"
/* USER CODE END Includes */

void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */
  os_tick_handler();
  /* USER CODE END SysTick_IRQn 0 */
  ...
}
```

There is no `HAL_IncTick()` in this handler - step 3 moved the HAL to TIM7.

#### 7. Boot the kernel in `Core/Src/main.c`

Again inside the markers, after the peripherals the kernel's callbacks depend on
are initialized - on a Nucleo that means after `BSP_COM_Init()`, or the first
log line goes nowhere:

```c
/* USER CODE BEGIN Includes */
#include "ahura.h"
/* USER CODE END Includes */

  /* ... HAL_Init(), SystemClock_Config(), MX_*_Init(), BSP_COM_Init() ... */

  /* USER CODE BEGIN WHILE */
  os_init();
  os_start();   /* never returns */

  while (1)
  {
    /* USER CODE END WHILE */
```

The generated `while (1)` below is now unreachable; leaving it in place keeps
the `USER CODE` markers intact for CubeMX.

#### 8. Build and flash

With the STM32 VS Code extension, the usual **CMake: Build** is enough. From a
shell, with `arm-none-eabi-gcc`, `cmake` and `ninja` on `PATH` (STM32CubeCLT
provides all three):

```bash
cmake --preset Debug
cmake --build build/Debug
```

The configure log should end with the kernel naming its port:

```text
-- Ahura kernel arch: cortex_m33
Memory region         Used Size  Region Size  %age Used
             RAM:        7856 B        32 KB     23.97%
           FLASH:       35320 B       128 KB     26.95%
```

Flash it with CubeProgrammer, the CubeIDE debugger, or:

```bash
STM32_Programmer_CLI -c port=SWD -w build/Debug/ahura.elf -rst
```

The green LED blinks at 1 Hz, and COM1 (115200 8N1 on the ST-LINK VCP) carries
whatever `OS_LOG_*` emits.

#### What regeneration touches

| File | Owner | Survives CubeMX regeneration |
|---|---|---|
| `Core/Inc/os_config.h`, `Core/Src/os_cb.c`, `Core/Src/os_main.c` | you | yes - CubeMX does not know they exist |
| `CMakeLists.txt` | you, after the first generation | yes - generated once only |
| `Core/Src/stm32h5xx_it.c`, `Core/Src/main.c` | CubeMX | yes, **if** the edits are inside `USER CODE` markers |
| `PendSV_Handler` staying absent, TIM7 time base | the `.ioc` | yes - both are stored settings, not hand edits |

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
application task, so the suite runs alone and `os_main()` is never called
(`os_main.c` can stay in the build; it is simply unused).

| Message | Cause |
|---|---|
| `undefined reference to 'os_test'` | Point 2: the test library is not linked |
| `multiple definition of 'os_log_output_cb'` | Point 3: your `os_cb.c` still defines it |

### One switch instead of two

Rather than keep points 1 and 2 in sync by hand, let CMake read the switch out
of the config, and register `os_config.h` as a configure dependency so that
editing it re-runs CMake by itself - a header is not otherwise a configure-time
dependency, and the build would go on linking the previous choice:

```cmake
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OS_CONFIG_DIR}/os_config.h)

file(READ ${OS_CONFIG_DIR}/os_config.h _os_cfg)
if(_os_cfg MATCHES "#define[ \t]+OS_CONFIG_TEST_ENABLE[ \t]+1")
    message(STATUS "Ahura self-test suite: ENABLED (os_main() is not run in this build)")
    add_subdirectory(AhuraRTOS/kernel/test)
    set(AHURA_TEST_LIB os_test)
else()
    set(AHURA_TEST_LIB "")
endif()

target_link_libraries(my_firmware ahura_kernel ${AHURA_TEST_LIB})
```

Flipping the define is then the whole procedure:

```text
$ cmake --build build/Debug
[0/1] Re-running CMake...
-- Ahura self-test suite: ENABLED (os_main() is not run in this build)
```

### Reading the output

The suite prints to `printf`, which on a Nucleo means COM1 at 115200 8N1 over
the ST-LINK virtual COM port - open it before resetting the board, the header
appears within milliseconds of boot:

```text
========================================
 Ahura RTOS self-test suite starting...
========================================

--- Kernel / Tick ---
  [PASS] ...
--- Task Lifecycle ---
  [PASS] os_task_create() rejects priority 0 (idle-reserved)
  [PASS] ...
  (one section per subsystem: mutex, semaphore, queue, events, timers,
   work queue, notifications, atomics, heap, logging, stress, ...)

========================================
 RESULT: <n> passed, 0 failed (of <n> checks)
 ALL RTOS FEATURES VERIFIED OK
========================================

========================================
 BENCHMARKS
========================================
  core      : ARMv8-M mainline (Cortex-M33/M35P), FPU, DSP
  build     : -Os, optimized for size, 32-bit
  clocks    : tick 1000 Hz, CPU 250000000 Hz

  Operation (uncontended fast path)              cycles         ns
  ------------------------------------------------------------
  (one row per hot kernel path)
```

A failing check prints `[FAIL]` with the file and line it came from, and the
`RESULT` line counts it.

`[SKIP]` lines are not failures - they name a feature switched off in
`os_config.h`, or one the port does not implement yet. Only the `RESULT` line
decides.

### Nothing on the terminal? Check the libc before the kernel

A silent console is almost never the kernel - it is usually C library buffering,
and it is worth ruling out first because the symptom (a board that boots and
says nothing) looks identical to a tick that never arrives.

newlib decides how to buffer `stdout` on the *first* `printf`, from what your
`syscalls.c` reports about file descriptor 1. It wants **both** answers, and
falls back to full buffering if either is missing:

| `_fstat()` sets `st_mode` | `_isatty()` returns | Buffering | What you see |
|---|---|---|---|
| `S_IFCHR` | nonzero | line buffered | each `\n` flushes - output appears line by line |
| `S_IFCHR` | `0` | **fully buffered** | silence |
| anything else, or no `_fstat` | (not consulted) | **fully buffered** | silence |

Fully buffered means the output accumulates in a 1 KB buffer that a kernel which
never exits never flushes.

CubeMX's generated `syscalls.c` sets both (`st_mode = S_IFCHR`, `_isatty`
returning `1`), so STM32 projects usually get line buffering for free.
Hand-written or trimmed-down retarget layers often supply only one, or neither.

Take the decision away from the libc entirely - one line in `main()`, before
`os_init()`:

```c
setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: every byte goes out at once */
```

Three things this buys, beyond making a fully-buffered target work at all:

- **No `malloc`.** Both buffered modes allocate their buffer on the first
  `printf`; `_IONBF` with a `NULL` pointer allocates nothing. On a target whose
  heap is a few KB, that matters, and it removes a `malloc` call from whichever
  task happens to print first.
- **Output survives a park.** `os_assert_failed_cb()` and
  `os_stack_overflow_cb()` write directly to the transport and then the core
  stops. A partially filled buffer is lost at exactly the moment you needed to
  read it.
- **Nothing is reordered.** Buffered `printf` from a task and a direct UART
  write from a callback interleave unpredictably; unbuffered does not.

The cost is throughput - one character per `_write()` call - which is
irrelevant for a bring-up console and is why this is a debug-build habit rather
than a rule.

### Budget the flash for it

The suite is deliberately exhaustive, and most of its footprint is the
`.rodata` behind those PASS/FAIL strings: **roughly 100 KB of flash at `-Os`**,
against about 20 KB for a small application. Measured on the NUCLEO-H503RB
(128 KB flash, 32 KB RAM) with everything in `os_config.h` at its default:

| Build | Flash | RAM |
|---|---|---|
| Application, `Release` (`-Os`) | 22 KB (17%) | 7.8 KB (24%) |
| Application, `Debug` (`-O0 -g3`) | 35 KB (27%) | 7.8 KB (24%) |
| **Self-test, `Release` (`-Os`)** | **130 KB (99%)** | 17.7 KB (54%) |
| Self-test, `Debug` (`-O0 -g3`) | overflows by ~14 KB | - |

So on a 128 KB part **run the self-test from the `Release` preset**:

```bash
cmake --preset Release
cmake --build build/Release
```

`Debug` does not fit, and the linker says so plainly - `region 'FLASH'
overflowed by 14172 bytes` - rather than producing a broken image. The suite
already drops its
extended stress tests (~15 KB) in unoptimized builds for the same reason,
printing a `[SKIP]` that names the cause; `-Os` runs the full set. Parts with
256 KB or more take either preset comfortably. Once the port is verified, set
`OS_CONFIG_TEST_ENABLE` back to `0` and the suite leaves the image entirely.

---

Full configuration options, the integration contract, task-priority rules, how
the scheduler and the context switch actually work, and every module's API are
documented in the
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
generator emits a competing `PendSV_Handler` - and [Vendor
notes](#vendor-notes) above covers it.

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
