# Tickless idle

[← Back to the documentation index](README.md)

The kernel's tick is a periodic interrupt. On a board that spends most of its
life idle, that interrupt is the single biggest reason the core cannot stay
asleep: at 1 kHz it wakes 1000 times a second to discover there is nothing to
do. Tickless idle stops that. When the idle task can prove nothing is due for
the next *N* ticks, the tick is suppressed for the length of that window, the
core sleeps, and on waking the kernel is told how much time actually passed.

The whole point is in that last clause. Suppressing a tick is easy; **knowing
how long it was suppressed for, and being unable to overstate it**, is the part
everything else here exists to guarantee.

This page is the one place the contract is written down in full: who masks, who
plans, who clamps, who measures, who may sleep, and what each port and each SoC
package actually does. Enabling it is two lines; the rest of the page is what
those two lines commit you to.

---

## Turn it on

```c
/* os_config.h */
#define OS_CONFIG_TICKLESS_ENABLE       1U
#define OS_CONFIG_TICKLESS_MIN_IDLE_MS  2U   /* shortest window worth sleeping for */
```

That is enough to build and run. Whether it *saves* anything depends on the SoC
package underneath: with no wake source, `os_arch_max_suppressed_ticks_get()`
answers 0, the kernel skips the sleep entirely, and the idle task falls back to
the plain `WFI` it did before. Nothing is claimed that is not delivered - see
[What a board actually gets](#what-a-board-actually-gets) for which of the four
packaged parts does what.

The second half of the configuration is the SoC package's, in `soc_config.h`:
which timer ends a window and how deep the sleep may go. Those two are per-chip
and are documented with each package - [STM32](stm32.md#the-soc-package),
[Raspberry Pi](raspberry-pi.md#the-packages-chip-by-chip).

---

## Who does what

Five layers touch a suppressed window, and each of them owns exactly one
question. Reading this table top to bottom is reading one idle pass.

| Layer | File | Owns |
|---|---|---|
| **The idle task** | `kernel/os_task.c` | Calling `os_tickless_idle_process()` on every pass, and doing the ordinary `WFI`/`WFE` afterwards |
| **The kernel** | `kernel/os_tick.c` | *How long is safe.* Masks interrupts, walks the deadline lists, applies both ceilings and both floors, decides whether to sleep at all, announces the result |
| **The port** | `arch/common/os_arch_tickless.c` + a per-ISA adapter | *How a window opens and closes.* Silences the tick, remembers what was promised, clamps what is reported |
| **The SoC package** | `soc/<vendor>/<part>/soc_cb.c` | *What ends the window, and how deep the sleep may be.* An LPTIM, an RTC, an always-on alarm, `mtimecmp` |
| **The application** | `os_cb.c` (optional) | `os_tickless_pre_sleep_cb()` / `os_tickless_post_sleep_cb()` - flush a UART, park a sensor, restore them after |

The important boundary is the third row against the fourth. **No port names a
timer.** The entire interface between them is four callbacks, which is what lets
the same port code serve a chip nobody has written a package for yet:

```c
uint32_t os_arch_tick_suppress_max_cb(void);      /* how many ticks I can wake you after */
uint32_t os_arch_tick_suppress_min_cb(void);      /* below this, do not bother */
void     os_arch_tick_suppress_cb(uint32_t ticks);/* wake me in this many ticks */
uint32_t os_arch_tick_resume_cb(void);            /* how many whole ticks that really was */
```

All four have weak defaults that suppress nothing, so a package can implement
none, some, or all of them.

---

## One idle pass, end to end

```
os_task_idle_entry()                                     os_task.c
  |
  |- os_tickless_idle_process()                          os_tick.c
  |    |
  |    |- core 0 only; once per tick, plus once per expiry armed  <- see SMP below
  |    |- mask = os_arch_kernel_mask_save()                  <- decide with interrupts OFF
  |    |- [SMP] os_tickless_window_open = true
  |    |
  |    |- planned  = min(next timer expiry, next sleeping task)
  |    |- ceiling  = os_arch_max_suppressed_ticks_get()      <- the port's / package's limit
  |    |- planned  = min(planned, ceiling, OS_TICKLESS_MAX_IDLE_TICKS)
  |    |- floor    = max(OS_TICKLESS_MIN_IDLE_TICKS, os_arch_min_suppressed_ticks_get())
  |    |
  |    `- if ceiling != 0 AND planned >= floor:
  |         os_tickless_pre_sleep_cb()                       <- application
  |         OS_ARCH_SLEEP(planned)
  |           |- os_arch_sleep_prepare(planned)              <- port: open the window
  |           `- os_arch_soc_sleep_cb()                      <- package: WFI, Stop, dormant...
  |         elapsed = os_arch_elapsed_ticks_get()            <- port: close it, clamped
  |         os_tickless_post_sleep_cb()                      <- application
  |         os_tick_announce(elapsed)                        <- the clock catches up
  |         os_arch_sleep_finish()                           <- port: release its own mask
  |
  |    |- [SMP] os_tickless_window_open = false
  |    `- os_arch_kernel_mask_restore(mask)
  |
  `- os_arch_soc_idle_cb()                                   <- the ordinary idle, OUTSIDE the mask
```

Four details in that order are load-bearing, and each of them was a bug at some
point:

- **The mask is taken before the deadlines are read**, not before the sleep. An
  ISR that starts a timer between "how long is safe" and "sleep" pends nothing,
  so there would be no wake-up event and the timer would fire late by the whole
  remaining window. A `WFI` still wakes on a pending interrupt while masked, so
  anything arriving after the mask shortens the sleep instead of being missed.
- **`ceiling != 0` guards the sleep itself**, not just the clamp. A port that can
  arm nothing has no window to measure - and `os_arch_soc_sleep_cb()` is entitled
  to be the chip's deepest mode. Entered with no wake source armed, an STM32 Stop
  entry also stops SysTick, the core waits on whatever unrelated interrupt happens
  along, and the port correctly reports 0 elapsed. The whole sleep then goes
  missing from the clock.
- **Measure, restore, announce, unmask - in that order.** Between measuring and
  announcing, `os_tick_count` is short by the entire sleep; anything that ran in
  that gap would decide against a clock hundreds of ticks behind reality.
- **The ordinary idle is outside the mask.** `os_arch_soc_idle_cb()` is a `WFE`
  on the parts where another core must be able to wake this one, and a latching
  `WFE` inside a masked region is a different thing entirely.

---

## The two ceilings and the two floors

A window is bounded four times, and each bound answers a different question.

| | Where from | What it means |
|---|---|---|
| Ceiling 1 | `os_arch_max_suppressed_ticks_get()` | What the hardware can count to. SysTick's 24-bit reload, an LPTIM's 16 bits, a POWMAN alarm's 64. **0 means "cannot suppress at all"** and stops the sleep happening |
| Ceiling 2 | `OS_TICKLESS_MAX_IDLE_TICKS` (`UINT32_MAX / 2`) | What a 32-bit tick counter can be asked to jump in one announcement. Binds only on a part whose timer effectively never runs out |
| Floor 1 | `OS_CONFIG_TICKLESS_MIN_IDLE_MS` | What the application prefers |
| Floor 2 | `os_arch_tick_suppress_min_cb()` | What arming and leaving the window *costs* on this chip. A property of the silicon, so the kernel takes whichever floor is larger and an application cannot configure its way below it |

Below the floor the mask is simply handed back and the pass behaves like a plain
`WFI`. That is not a failure; it is the correct answer for a window too short to
pay for itself.

---

## The clamp, and why it is the whole game

`os_arch_elapsed_ticks_get()` never returns more than the window was opened for.

A wake source that overshoots - a late wake, a 64-bit counter read across a
wrap, a source coarser than a tick - would otherwise push `os_tick_count`
**forward** past deadlines that had not expired. Software timers fire early.
Delays end before they ran out. Timeouts expire on tasks that were still within
them. And nothing downstream can notice, because from the kernel's point of view
the time simply happened.

Being **late** is the recoverable failure: the next tick catches up and every
deadline is merely served late. Being **early** is not recoverable at all. So the
port clamps, on the stated principle that *the window was never promised more
than this* - and the same rule is why `os_arch_tick_resume_cb()` is specified to
return **whole tick periods only**. A window cut short by an unrelated interrupt
is the normal case, not an error, and reporting the partial remainder would have
the kernel announce time that has not happened yet.

---

## What each port does

There are exactly two mechanisms, and which one a port can use is decided by
something outside the tick entirely: **whether the port has a cycle counter that
does not come from the tick.**

- **Mask the tick interrupt, let a SoC timer end the window.** The tick counter
  keeps running at its usual cadence, so the tick grid keeps its phase and any
  period arithmetic built on it stays valid. Only the interrupts are missed, and
  they are handed back as a count at the end. Needs a timer the package owns.
- **Stretch the tick timer itself.** Cheaper - it needs no other hardware at all -
  but it moves SysTick's reload, and where `os_arch_cycle_count_get()` is
  *synthesized* from SysTick (counting whole periods and multiplying by the
  reload it reads live) that silently rescales every reading `os_delay_us()` and
  the busy-wait half of `os_delay_ms()` depend on.

| Port | Mechanism | Why |
|---|---|---|
| **v6m** (M0, M0+, M23) | SoC timer only | ARMv6-M has no DWT at all, so the SysTick-derived counter is the *only* cycle counter that exists |
| **v7m** (M3, M4, M7) | SoC timer only | DWT is architecturally optional, and the port must work on parts without it |
| **v8m** (M33, M35P, M52, M55, M85) | SoC timer **or** SysTick itself | Prefers the package's source when there is one - it is not bounded by SysTick's 24 bits and it is the only kind that survives a sleep deep enough to gate SysTick's clock. Falls back to reprogramming SysTick where DWT CYCCNT is present, and to nothing at all where it is not |
| **rv32** (Hazard3) | SoC timer only | The tick *is* `mtimecmp`, and the privileged spec deliberately does not say where that register lives, so only the package can move it. `mcycle` is architectural, so a window costs the cycle counter nothing |

All four share one implementation. `arch/common/os_arch_tickless.c` holds the
contract - the five functions the kernel calls, the four weak SoC callbacks, the
"was a window even armed" guard, and the clamp - and a port answers three
questions to specialise it:

| | Answers |
|---|---|
| `OS_ARCH_TICKLESS_TICK_SILENCE()` / `_RESTORE()` | How the periodic tick *interrupt* is silenced for a window and put back. One SysTick CSR bit on Cortex-M; empty on RISC-V |
| `OS_ARCH_TICKLESS_CYCLE_FROM_TICK` | Whether the cycle counter is fed by the tick interrupt, and therefore has to be credited with what a window cost it. 1 on Cortex-M, 0 on RISC-V |
| `OS_ARCH_TICKLESS_SELF_SUPPRESS` | Whether the port can stretch its own tick timer. 1 on v8m only |

Those five functions used to be written three times, and the copies disagreed -
not about the mechanism, about the guards. One checked whether a window had
actually been armed before asking how long it lasted and two did not; one
clamped and one handed the package's number straight to the clock; one
advertised a ceiling a tick above what it would honour. Each was a real defect,
and each was the same line missing from a different copy. That is why there is
one copy now.

---

## What a board actually gets

| Package | Wake source | Deepest mode | Ceiling |
|---|---|---|---|
| `st/stm32` | LPTIM, clocked from LSI or LSE | Stop (`OS_CONFIG_TICKLESS_DEEP_ENABLE = 1U`) | Three quarters of the LPTIM's 16-bit span, the rest kept as wrap headroom - 1500 ticks on a 32.768 kHz clock at 1 kHz ticks, 1536 on a 32 kHz LSI |
| `raspberrypi/rp2040` | An alarm on the always-on microsecond timer | Core sleep, clocks running | 60 s |
| `raspberrypi/rp235x_arm` | A POWMAN alarm, which outlives the clocks | PLL_SYS powered down (`OS_CONFIG_TICKLESS_DEEP_ENABLE = 1U`), with core 1 parked by agreement | Effectively unbounded - `OS_TICKLESS_MAX_IDLE_TICKS` is the binding limit |
| `raspberrypi/rp235x_riscv` | `mtimecmp` | Core sleep | As above |
| *(none)* | - | - | 0: the kernel skips the sleep, the idle task does a plain `WFI` |

On an ARMv8-M part with **no** package wake source, the v8m port still suppresses
using SysTick - which is why a NUCLEO-H503 with tickless on and no LPTIM
configured reports a ceiling of 67 ticks (24 bits at 250 MHz, 1 kHz ticks) rather
than 0.

Each package must also declare a link anchor in its `soc.cmake`
(`AHURA_SOC_LINK_OPTIONS -u soc_<name>_anchor`). Without it the whole `soc_cb.c`
object can be dropped from a static-archive link, taking the tick with it -
`CMakeLists.txt` now refuses to configure a package that has not declared one.
See [SoC packages](soc.md).

---

## Multi-core

Only **core 0** runs the tickless pass. It owns the kernel time base, exactly as
`os_tick_handler()` does: announcing a suppressed window from another core would
add its idle time to counters core 0's tick interrupt is already advancing, so
every sleep would be counted twice and the clock would run fast. Other cores
still idle - they just do it in a plain `WFI` and announce nothing.

That leaves one hazard. Core 0 plans a window from the deadlines that exist *when
it looks*; a task on core 1 can start a timer a microsecond later, and core 0 is
by then asleep past it. Asking whether the other cores are idle would not help -
a core can take work the instant after it answers.

What does help is the core **creating** the nearer deadline saying so:

- `os_tickless_window_open` is raised by core 0 *before* it reads any deadline,
  and lowered before it unmasks.
- Every path that puts a new expiry on a kernel time source calls
  `os_tickless_deadline_armed()`, which reads that flag and sends an IPI to core 0.
- The IPI ends the window the same way any early wake does: a `WFI` wakes on a
  pending interrupt even behind the kernel mask, so core 0 leaves the sleep,
  measures what really elapsed, and announces it. Nothing new has to be correct
  for this to work.

The one thing the kernel cannot check for itself is that the IPI exists.
`os_arch_core_ipi_request_cb()` is a SoC callback, and an empty default is fine
for ordinary preemption - the target core picks the work up at its next tick -
and **not** fine here, because a suppressed window is precisely the absence of a
next tick. So `template/soc_cb.c` withdraws its empty default for exactly this
combination: an unpackaged multi-core target that enables tickless fails to
*link*, naming the symbol, rather than quietly missing deadlines.

---

## Application hooks

Two optional callbacks bracket the sleep, both called with interrupts masked and
the window already armed:

```c
void os_tickless_pre_sleep_cb(void);   /* about to sleep for a while */
void os_tickless_post_sleep_cb(void);  /* awake; the clock has not caught up yet */
```

Use them for things the kernel cannot know about: flushing a UART before its
clock stops, parking a sensor, saving and restoring a peripheral that does not
survive Stop mode. Keep them short - they are inside the masked region, and time
spent there is interrupt latency.

They are **not** required to enable tickless idle. The SoC package supplies weak
defaults, and defining either one strongly displaces the package's default for
that hook alone.

---

## Proving it on a board

The self-test suite has four groups for this, and they are worth reading in
order because each one tests something the previous cannot:

| Group | What it establishes |
|---|---|
| **Tickless Sleep Hooks** | Both hooks return promptly and leave kernel state intact |
| **Tickless Bounds** | The window never outlasts a known deadline: it reports the port's own ceiling with nothing pending, is capped at 10 by a timer due in 10 ticks, and at 38 by a task with 38 ticks of delay left |
| **Tickless Sleep** | End to end on real hardware: the interrupt mask comes back as it was, a 33-tick window sleeps and is *measured* as 33, and the timer that bounded it fired exactly once |
| **Tickless Drift** | 20 consecutive 8-tick windows against a free-running counter that never stops, so an error that is invisible in one window accumulates into view |

Enable it with `OS_CONFIG_TEST_ENABLE` and read [the self-test
page](self-test.md). A board that passes all four has proven its package's wake
source, its ceiling, its clamp and its accounting - which is everything on this
page except the SMP handshake, and that needs a dual-core part.

---

## Common mistakes

| Symptom | Cause |
|---|---|
| Board freezes a few characters into its banner | The SoC package was dropped from the link - it needs `-u soc_<name>_anchor` in `AHURA_SOC_LINK_OPTIONS`. `SysTick_Handler` is then the startup file's `Default_Handler`, an infinite loop |
| Delays and timers all run **late**, by roughly the sleep time | A window was entered that nothing announced: `os_arch_tick_resume_cb()` returned 0 because nothing was armed. Check that `os_arch_tick_suppress_max_cb()` is non-zero for this configuration |
| Timers fire **early**, delays end short | Something is announcing more than the window was promised. The port clamps, so this points at a package's `os_arch_tick_suppress_max_cb()` claiming a ceiling its arming path cannot honour |
| `os_delay_us()` is wrong after the first sleep | A port reprogrammed the tick reload while the cycle counter was synthesized from it. This is what `os_arch_dwt_available` gates on v8m, and why v6m and v7m never take that path |
| Clock gains a tick per window | The window was armed from a zeroed `CVR` rather than from the remainder of the tick already running, so it is one period short of what gets announced |
| Nothing sleeps at all | Expected, and correct, when the package supplies no wake source. Check `os_tickless_max_suppressed_ticks_get()`: 0 means the kernel is deliberately skipping the sleep |

---

## See also

- **[How the kernel works](design.md)** - the tick, the scheduler, and the source layout
- **[Platform support](porting.md)** - the callbacks a platform must supply
- **[SoC packages](soc.md)** - who owns which callback, and how to write a package
- **[Self-test suite](self-test.md)** - how to run the four groups above
