# AhuraRTOS on STM32CubeMX / STM32CubeIDE

[← Documentation index](README.md) · [Installation](installation.md) ·
[Vendor notes](vendor-notes.md)

> On a Raspberry Pi Pico instead? The same two installers exist for the Pico SDK
> - see **[Raspberry Pi Pico SDK](pico-sdk.md)**.

The [six general installation steps](installation.md) carried out on real ST
hardware. **Three ways to do it - pick one:**

| | Route | Good for |
|---|---|---|
| **A** | **[Automatic](#automatic---one-command)** - one command | A CubeMX project generated with the **CMake** toolchain. Seconds, shows the diff first, safe to re-run |
| **B** | **[Offline](#offline---no-internet-on-the-machine)** - one command, no network | The same project on a machine with no route out: an air-gapped lab, a locked-down corporate network, a CI agent |
| **C** | **[Manual](#manual---step-by-step)** - eight steps by hand | CubeIDE projects, non-CMake builds, or when you want to make every edit yourself |

Both end in the same place. Verified end to end on a **NUCLEO-H503RB**
(Cortex-M33) built with `arm-none-eabi-gcc` 14.3.1 from the STM32Cube toolchain;
the only board-specific names below are the `stm32h5xx_*` file names and `TIM7`.

ST tooling differs from every other vendor's in exactly two places, and both are
CubeMX checkboxes: it generates its own `PendSV_Handler`, and its HAL takes
SysTick for `HAL_GetTick()`. The automatic route deals with both for you; manual
steps 2 and 3 are where you do it yourself.

---

## Automatic - one command

Run it from the root of your project - the directory holding `CMakeLists.txt`
and the `.ioc`.

**Windows** (PowerShell):

```powershell
irm https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_stm32_online.py | python -
```

**Linux and macOS** (bash, zsh):

```bash
curl -fsSL https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_stm32_online.py | python3 -
```

That is the whole thing. It prints the exact diff it wants to apply and waits
for a `y` before touching anything. Python 3.8+ and nothing else - no
`pip install`, and the script runs straight out of the pipe, so no installer
file is left behind in your project.

### What it does

Exactly the manual steps below, in the same order:

| Step | |
|---|---|
| 1 | Puts the AhuraRTOS repository at `AhuraRTOS/` in your project |
| 2 | Disables a generated `PendSV_Handler` by wrapping it in `#if 0` |
| 3 | Checks the HAL time base is off SysTick, and stops if it is not |
| 4 | Copies `os_config.h`, `os_cb.c` and `os_main.c` into `Core/` |
| 5 | Appends the kernel block to the top-level `CMakeLists.txt` |
| 6 | Calls `os_tick_handler()` from `SysTick_Handler` |
| 7 | Calls `os_init()` / `os_start()` at the end of `main()` |
| 8 | Leaves the build to you |

### Options

| Option | Effect |
|---|---|
| `--dry-run` | Print the diff and stop. Writes nothing |
| `--yes`, `-y` | Skip the confirmation |
| `--app-dir DIR` | Which source tree to install into, on a dual-core part |
| `--tick external` | Drive `os_tick_handler()` from your own timer instead of SysTick |
| `--ref REF` | Branch or tag to download (default: `main`) |
| `--project DIR` | Project root (default: the current directory) |
| `--source PATH` | Use this checkout instead of downloading |
| `--update` | Replace an `AhuraRTOS/` already in the project with the current version |
| `--force-templates` | Overwrite an existing `os_config.h` / `os_cb.c` / `os_main.c` |
| `--uninstall` | Take the whole integration back out |

They go after the `-`, which is where the pipe leaves room for the script's own
arguments:

```bash
curl -fsSL .../tools/install_stm32_online.py | python3 - --dry-run
```

### Running it twice

Nothing happens twice. Each run works out what is already in place and fills in
only what is missing:

| Already there | What the next run does |
|---|---|
| `AhuraRTOS/` | Left exactly as it is - `--update` replaces it |
| `os_config.h`, `os_cb.c`, `os_main.c` | Kept, never overwritten - they are yours the moment they exist |
| The CMake block, the tick, the boot calls | Rebuilt at the correct anchor, so a call that was moved or lost comes back |

With everything in place it does no work and no network access at all, and says
so. That last row is also the repair: if CubeMX regenerates over the
integration, or a block gets deleted by hand, run the command again.

### What it will not do

**It never opens the `.ioc`.** That file is what CubeMX generates *from*, and
every fact the script needs is in the generated sources - which is also all the
compiler ever sees.

**It never overwrites your three files.** Once `os_config.h`, `os_cb.c` and
`os_main.c` exist they are yours, edits and all.

**It never loses code it disables.** A generated `PendSV_Handler` is wrapped in
`#if 0` rather than deleted, because the kernel's port defines that symbol and
two definitions are a link error. `--uninstall` restores it verbatim.

Every C edit it makes goes inside a CubeMX `USER CODE` section, so regeneration
keeps it. The one exception is that `PendSV_Handler`, because CubeMX owns the
function and offers no section inside it - so regenerating brings the stub back,
and re-running the installer disables it again. To stop it being generated at
all, do [manual step 2](#2-cubemx-stop-generating-pendsv_handler) once.

It stops with an explanation, **before writing anything**, if the HAL still owns
SysTick or if FreeRTOS is already in the project. The fix is a CubeMX checkbox
in both cases, and the message names it.

Installed? Go straight to **[Build and flash](#8-build-and-flash)**.

---

## Offline - no internet on the machine

Same installation, same six steps, same result - the only difference is where
the kernel comes from. `install_stm32_offline.py` never touches the network: it
uses a copy of the repository already on the machine, and it does not import
`urllib`, `tarfile` or `socket` to do it.

Use it on an air-gapped lab machine, behind a corporate proxy that blocks
GitHub, or on a build agent with no route out.

### 1. Get the repository, on a machine that has a connection

Either a clone or the green **Code → Download ZIP** button:

```bash
git clone https://github.com/AhuraRTOS/AhuraRTOS.git
```

### 2. Copy it into your project and run it

```text
MyProject/
├── CMakeLists.txt        <- the top-level one CubeMX generated
├── MyProject.ioc
├── Core/
└── AhuraRTOS/            <- the repository you copied in
    ├── ahura.h
    ├── kernel/  arch/  soc/  template/
    └── tools/install_stm32_offline.py
```

```bash
cd MyProject
python3 AhuraRTOS/tools/install_stm32_offline.py
```

That is the whole procedure. It finds the kernel beside itself, reads the
project, prints the same diff and waits for a `y` - exactly like the online
installer, because it *is* the online installer: everything that reads the
project, computes the edits and writes them with rollback is imported from
`install_stm32_online.py`. Only the "where does the kernel come from" step differs, so
the two cannot drift apart.

A ZIP download unpacks as `AhuraRTOS-main/` rather than `AhuraRTOS/`. That works
too - it is found under either name, and installed to `AhuraRTOS/` so the CMake
block matches what the online installer would have written.

### Where it looks for the kernel

In this order, most explicit first, stopping at the first real checkout:

| | Location |
|---|---|
| 1 | `--source PATH`, if given. A path that is not a checkout is an **error**, never a reason to keep looking - silently installing some other copy is how the wrong kernel version ends up in a build |
| 2 | The checkout this script is running from |
| 3 | `<project>/AhuraRTOS` |
| 4 | `<project>/AhuraRTOS*` - a ZIP still under its branch name |
| 5 | Next to the project, one level up, under either name - for when several projects share one downloaded copy |

If none of them holds a kernel it says so and stops. It will not download one;
that is the entire point of this script.

### Already in the right place

Offline, the source and the destination are routinely the **same directory**:
the repository is at `<project>/AhuraRTOS`, which is exactly where the installer
would otherwise copy it. Copying a directory onto itself is not a copy - the
destination is cleared first, so it would destroy the source.

So "already in place" is treated as nothing to copy, and any copy whose source
and destination resolve to one path is refused. Symlinks, junctions and
Windows paths differing only in case are all resolved before that comparison, so
none of them slips past as a different directory.

```text
  note: the kernel is already at AhuraRTOS/ - installed in place, nothing copied
```

### Offline options

The same as the online installer, minus `--ref` - there is nothing to fetch:

| Option | Effect |
|---|---|
| `--dry-run` | Print the diff and stop. Writes nothing |
| `--yes` | Skip the confirmation |
| `--source PATH` | Use this checkout, wherever it is |
| `--project DIR` | Project root (default: the current directory) |
| `--app-dir DIR` | Which source tree to install into, on a dual-core part |
| `--tick external` | Drive `os_tick_handler()` from your own timer instead of SysTick |
| `--update` | Refresh `AhuraRTOS/` in the project from `--source` |
| `--force-templates` | Overwrite an existing `os_config.h` / `os_cb.c` / `os_main.c` |
| `--uninstall` | Take the whole integration back out |

`--uninstall` needs no kernel at all - it only removes managed blocks and the
installed directory - so it keeps working on a machine where the original
download has since been deleted.

---

## Manual - step by step

The same result by hand: eight steps, because two of the six general ones are
CubeMX settings rather than code.

### 1. Add the kernel to the project

From the project root (the directory holding `CMakeLists.txt` and the `.ioc`),
copy a clone of AhuraRTOS in as `AhuraRTOS/`:

```bash
git clone https://github.com/AhuraRTOS/AhuraRTOS.git AhuraRTOS
rm -rf AhuraRTOS/.git      # or keep it, and update with git pull
```

Or track the whole repository as a submodule instead, if you would rather
updates be a `git pull`:

```bash
git submodule add https://github.com/AhuraRTOS/AhuraRTOS.git AhuraRTOS
```

Both put the tree at `AhuraRTOS/`, which is the path every CMake line
below uses. See [Installation → Step 1](installation.md#step-1---get-the-source)
for the details and [Keeping the kernel up to
date](installation.md#keeping-the-kernel-up-to-date) for later.

### 2. CubeMX: stop generating `PendSV_Handler`

> **System Core → NVIC → Code generation tab → clear "Generate IRQ handler" for
> *Pendable request for system service*.**

CubeMX otherwise writes a non-weak empty `PendSV_Handler` into
`Core/Src/stm32h5xx_it.c`, which collides with the kernel's at link time:

```text
multiple definition of `PendSV_Handler'
```

Deleting the function by hand works until the next code generation puts it
back. The checkbox is stored in the `.ioc`, so regeneration keeps honouring it.

**Clear *System service call via SWI instruction* as well** if you are going to
run the [self-test suite](self-test.md). The kernel itself never uses `SVC` -
that is deliberate, and it is what keeps it compatible with SoftDevice, TF-M and
vendor ROM APIs - but the suite installs its own handler to reach ISR context
from a task, and on CMSIS-Pack that handler is named `SVC_Handler` too. A
generated one collides exactly the same way. Turning it off costs an application
that does not use `SVC` nothing.

This is what the *Code generation* tab should look like when you are done -
everything at its default except the two cleared rows:

| Row | Generate IRQ handler |
|---|---|
| Non maskable interrupt, Hard fault, Memory management fault, Pre-fetch fault, Undefined instruction, Debug monitor | ✔ default |
| **System service call via SWI instruction** (`SVC_Handler`) | ☐ **clear it** - see above |
| **Pendable request for system service** (`PendSV_Handler`) | ☐ **clear it** - the kernel owns this vector |
| **System tick timer** (`SysTick_Handler`) | ✔ **keep it** - `os_tick_handler()` goes in it, step 6 |
| Time base: *<your timer>* global interrupt | ✔ appears once step 3 is done - CubeMX ticks it for you |

The *Select for init sequence ordering* and *Call HAL handler* columns are not
involved; leave them as they are.

> **On the installer route?** It disables a generated `PendSV_Handler` for you
> by wrapping it in `#if 0`, but it cannot touch `SVC_Handler` - nothing in the
> generated sources says whether you intend to run the self-test. If you enable
> the suite later and the link fails on `SVC_Handler`, this checkbox is the fix.

### 3. CubeMX: move the HAL time base off SysTick

> **System Core → SYS → Timebase Source → any spare timer.**

`HAL_Init()` otherwise claims SysTick for `HAL_GetTick()`, and the kernel needs
it. Two time bases sharing one interrupt drift against each other, so move one
rather than sharing the handler.

**Which timer does not matter.** The kernel never touches it - it belongs to the
HAL from here on, and the only requirement is that nothing else in your design
already wants it. `TIM7` is what the rest of this page names because it is what
the NUCLEO-H503RB was verified with; `TIM6` and `TIM17` are the other usual
picks, and on a part that has none of those any spare timer in the dropdown does
the same job. Substitute your own everywhere `TIM7` appears below.

Whichever you pick, CubeMX does three things by itself: it adds
`Core/Src/stm32h5xx_hal_timebase_tim.c` to the project, it writes that timer's
`IRQHandler` into `stm32h5xx_it.c`, and it ticks *Time base: <timer> global
interrupt* in the NVIC table from step 2. From then on `HAL_Delay()` and
`HAL_GetTick()` run off it while SysTick belongs to the kernel. Do **not** call
`HAL_IncTick()` from `SysTick_Handler` afterwards.

You can confirm it landed in the `.ioc` without opening CubeMX again - these two
lines name whichever timer you chose:

```text
NVIC.TimeBase=TIM7_IRQn
NVIC.TimeBaseIP=TIM7
```

Regenerate the code once the checkboxes are set (**Project → Generate Code**),
then confirm in `Core/Src/stm32h5xx_it.c` that `PendSV_Handler` is gone (and
`SVC_Handler` with it, if you cleared that too), while `SysTick_Handler` is
still there and the time-base timer's `IRQHandler` has appeared.

`HAL_Delay()` still busy-waits - it does not yield. Use `os_delay_ms()` in task
code and keep `HAL_Delay()` for driver init that runs before `os_start()`.

### 4. Copy the three files

```bash
cp AhuraRTOS/template/os_config.h Core/Inc/os_config.h
cp AhuraRTOS/template/os_cb.c     Core/Src/os_cb.c
cp AhuraRTOS/template/os_main.c   Core/Src/os_main.c
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

### 5. Edit the top-level `CMakeLists.txt`

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

# Editing os_config.h re-runs CMake by itself (see the self-test page).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OS_CONFIG_DIR}/os_config.h)

# Link the self-test suite iff os_config.h asks for it.
file(READ ${OS_CONFIG_DIR}/os_config.h _os_config_contents)
if(_os_config_contents MATCHES "#define[ \t]+OS_CONFIG_TEST_ENABLE[ \t]+1")
    message(STATUS "Ahura self-test suite: ENABLED (os_main() is not run in this build)")
    add_subdirectory(AhuraRTOS/test)
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

That is the whole build change. The middle two stanzas are explained in
[One switch instead of two](self-test.md#one-switch-instead-of-two); drop them
and the block still works, at the cost of switching the test suite on in two
places by hand.

### 6. Route the tick in `Core/Src/stm32h5xx_it.c`

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

### 7. Boot the kernel in `Core/Src/main.c`

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

### 8. Build and flash

With the STM32 VS Code extension, the usual **CMake: Build** is enough. From a
shell, with `arm-none-eabi-gcc`, `cmake` and `ninja` on `PATH` (STM32CubeCLT
provides all three):

```bash
cmake --preset Debug
cmake --build build/Debug
```

The configure log should end with the kernel naming its port:

```text
-- Ahura kernel arch: arm/cortex_m33
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

---

## What regeneration touches

| File | Owner | Survives CubeMX regeneration |
|---|---|---|
| `Core/Inc/os_config.h`, `Core/Src/os_cb.c`, `Core/Src/os_main.c` | you | yes - CubeMX does not know they exist |
| `CMakeLists.txt` | you, after the first generation | yes - generated once only |
| `Core/Src/stm32h5xx_it.c`, `Core/Src/main.c` | CubeMX | yes, **if** the edits are inside `USER CODE` markers |
| `PendSV_Handler` and `SVC_Handler` staying absent, TIM7 time base | the `.ioc` | yes - all three are stored settings, not hand edits |

If a regeneration does lose something, the
[automatic installer](#automatic---one-command) puts it back - running it again
is the repair, whichever route you used the first time.

## Next step

Prove the integration before writing anything on top of it:
**[Run the self-test suite](self-test.md)**.
