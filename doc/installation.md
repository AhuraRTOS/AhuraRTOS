# Installing AhuraRTOS

[← Documentation index](README.md)

**The kernel takes over exactly one exception - PendSV - and asks the
application for exactly one thing: a periodic call to `os_tick_handler()`.** It
claims no `SVC_Handler`, no `SysTick_Handler`, no HAL, and no vendor headers.
That is the whole integration contract, and it is why the same kernel drops onto
an STM32, an nRF52 and an LPC without changing anything but a config file.

This page is the procedure itself, independent of vendor, IDE and build system.
Two companion pages go with it:

- **[Vendor notes](vendor-notes.md)** - the one thing that differs per vendor,
  and what to do about it on STM32, Nordic, NXP and everything else.
- **[STM32CubeMX / STM32CubeIDE, step by step](stm32cubemx.md)** - the same six
  steps carried out on a concrete board, with the exact checkboxes, file paths,
  CMake lines and build commands. Read it if you use ST tooling; skim it as a
  worked example if you do not.

Paths below assume the kernel sits at `AhuraRTOS/kernel/` in your project, which
is what step 1 produces. Nothing depends on that layout - only on the build
being able to see the files.

---

## Contents

[1. Get the source](#step-1---get-the-source) ·
[2. Copy three files](#step-2---copy-three-files-into-your-project) ·
[3. Add the kernel to the build](#step-3---add-the-kernel-to-the-build) ·
[4. Give the kernel its tick](#step-4---give-the-kernel-its-tick) ·
[5. Check `PendSV_Handler`](#step-5---make-sure-nothing-else-defines-pendsv_handler) ·
[6. Boot it](#step-6---boot-it) ·
[If it does not build](#if-it-does-not-build) ·
[Keeping it up to date](#keeping-the-kernel-up-to-date) ·
[Next steps](#next-steps)

---

## Step 1 - get the source

Everything lives in one repository - the kernel, the examples and these docs.
There are no submodules, so a plain clone is the whole story:

```bash
git clone https://github.com/AhuraRTOS/AhuraRTOS.git
```

Then get `kernel/` into your project. Both routes below land it at
`AhuraRTOS/kernel/`, which is the path the rest of this page assumes.

**Copy it in** - the simplest thing that works, and the recommended default.
Nothing about the build is git-specific:

```bash
mkdir -p my_project/AhuraRTOS
cp -r AhuraRTOS/kernel my_project/AhuraRTOS/kernel
```

**Or track it as a submodule** of your project, if you would rather updates be a
`git pull`. This brings `doc/` and `examples/` along with it, which is a little
over a megabyte:

```bash
cd my_project
git submodule add https://github.com/AhuraRTOS/AhuraRTOS.git AhuraRTOS
```

Either way, see [Keeping the kernel up to date](#keeping-the-kernel-up-to-date)
below once you are running.

## Step 2 - copy three files into your project

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

Every option is documented in the
[kernel reference → Configuration](kernel.md#configuration).

## Step 3 - add the kernel to the build

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

## Step 4 - give the kernel its tick

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
that is CubeMX → SYS → Timebase Source → any spare timer; see
[Vendor notes](vendor-notes.md).

## Step 5 - make sure nothing else defines `PendSV_Handler`

Usually nothing does, and there is nothing to do: the port defines
`PendSV_Handler`, which is the name every CMSIS startup file already has in the
vector table. The one common exception is a vendor IDE that generates an empty
stub - **STM32CubeMX does**, and it is a checkbox; see
[Vendor notes](vendor-notes.md).

If your vector table calls entry 14 something else (a hand-written startup file,
a bootloader's own table), point the kernel at that name instead:

```c
#define OS_CONFIG_ARCH_PENDSV_HANDLER  my_pendsv_vector_name
```

The kernel verifies the live vector table at boot and traps immediately if it
was not wired up, instead of hanging silently.

## Step 6 - boot it

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

**Learning a specific feature?** The
[examples](../examples/README.md) hold one standalone
`os_main_<feature>.c` per feature - mutexes, queues, events, timers, the work
queue, notifications, atomics, the heap, logging, and so on. Each one *is* an
`os_main.c`: it includes only `ahura.h` and `<stdio.h>`, needs no board support
beyond `printf`, and drops straight into the build in place of the copy from
step 2. Swap one in, build, read the console, swap the next.

## If it does not build

| Message | Cause |
|---|---|
| `No os_config.h found` | Step 2 missed, or `OS_CONFIG_DIR` does not point at it |
| `os_config.h is incomplete` | An option was deleted from the copy - start again from the template |
| `multiple definition of 'PendSV_Handler'` | Step 5: something else defines it, usually a vendor IDE stub |
| `undefined reference to 'os_main'` | `os_main.c` is not in the application build (step 2) |
| `undefined reference to 'os_assert_failed_cb'` (or `os_stack_overflow_cb`, `os_log_output_cb`) | `os_cb.c` is not in the application build, or that callback was deleted from it |
| Duplicate symbols from the port | `arch/arm/common/*.c` was added to the build (step 3) - remove it |
| Builds and runs, but nothing happens | Step 4: the tick is not reaching `os_tick_handler()` |

## Keeping the kernel up to date

Updating is a replacement, never a merge. You never edited a kernel file, so
there is nothing of yours inside `kernel/` to preserve - your three files
(`os_config.h`, `os_cb.c`, `os_main.c`) live in your own tree and are not
touched by any of this.

**If you copied it in**, delete the directory and copy the new one over:

```bash
cd AhuraRTOS-checkout && git pull
rm -rf my_project/AhuraRTOS/kernel
cp -r kernel my_project/AhuraRTOS/kernel
```

Deleting first rather than copying over the top matters: a file removed upstream
would otherwise linger and keep compiling.

**If you tracked it as a submodule**, pull the pointer forward and commit it:

```bash
cd my_project
git submodule update --remote AhuraRTOS
git add AhuraRTOS && git commit -m "Update AhuraRTOS"
```

Then, either way, two things to check afterwards:

1. **Diff the config template against your copy.** New releases add options, and
   a missing one is a hard error rather than a silent default - by design, but
   only helpful if you know to look:

   ```bash
   diff AhuraRTOS/kernel/os_config_template.h Core/Inc/os_config.h
   ```

   Add any new `#define` to your `os_config.h`; leave your edited values alone.

2. **Re-run the [self-test suite](self-test.md)** before shipping. It is the
   fastest confirmation that the new kernel still agrees with your port, your
   tick and your callbacks.

If `os_cb_template.c` grew a callback you do not have, the link error names it.

## Next steps

- **[Vendor notes](vendor-notes.md)** if your silicon vendor's tooling generates
  its own `PendSV_Handler`, or its SysTick is unusable.
- **[Run the self-test suite](self-test.md)** to prove the port before writing
  anything on top of it - it validates every enabled feature with no application
  code at all.
- **[Kernel reference](kernel.md)**
  for every configuration option, every API, and how the kernel works inside.
