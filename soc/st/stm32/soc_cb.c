/**
 * @file soc_cb.c
 * @brief SoC-owned kernel callbacks for STMicroelectronics STM32.
 *
 * A short file, and deliberately so: CMSIS-Pack startup already gives the kernel its PendSV
 * vector and SystemCoreClock, and single-core parts need no core id, IPI or spinlock.
 *
 * Everything here is weak, so one strong definition in the application replaces that callback and
 * leaves the rest. Nothing is mandatory - with the HAL absent, or the soc_config.h options off,
 * each body compiles to nothing and the kernel behaves as it does with no package at all.
 *
 * Not here on purpose: the PendSV vector name (a CubeMX setting, see doc/vendor-notes.md), and
 * programming the tick (the port does that; only the VECTOR is here).
 *
 * Why the package is shaped this way, and what an LPTIM or RTC tick would need on the L and U
 * families: doc/stm32.md, "The SoC package".
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "ahura.h"

/*
 * The application's SoC configuration: copy template/soc_config.h into Core/Inc beside
 * os_config.h. It is found on the include path OS_CONFIG_DIR already provides.
 *
 * REQUIRED, and so is every option in it - the same terms as os_config.h. The file holds ONLY the
 * values a user may need to decide; there are deliberately no built-in defaults, so a missing
 * option is rejected at compile time rather than silently read as 0. Whatever this package can
 * decide for itself is not in the file at all.
 */
#if defined(__has_include)
#if !__has_include("soc_config.h")
#error "No soc_config.h found: copy soc/st/stm32/template/soc_config.h into Core/Inc beside os_config.h."
#endif
#endif

#include "soc_config.h"

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Reject an incomplete configuration rather than filling the gap: a missing option reads as 0 in
 * an #if, silently turning off the clock refresh or the timebase handling. */
#if !defined(SOC_CONFIG_CLOCK_AUTO_UPDATE) || !defined(SOC_CONFIG_SYSTICK_VECTOR)
#error "soc_config.h is incomplete: it must define every option listed in soc/st/stm32/template/soc_config.h."
#endif

/* The tickless half is asked for only when there is tickless idle to configure. Undefined, both
 * read as 0 in the #if tests below - which is LIGHT and "do not touch the HAL tick", exactly what
 * a build without tickless wants. */
#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
#if !defined(SOC_CONFIG_TICKLESS_HAL_TICK)
#error "soc_config.h is incomplete: OS_CONFIG_TICKLESS_ENABLE is 1, so SOC_CONFIG_TICKLESS_HAL_TICK is required too."
#endif
#endif

/* How many cores this package supports scheduling on. The STM32 range is overwhelmingly
 * single-core, and the dual-core parts that exist (H7 dual, WB, WL, MP1) pair a Cortex-M7 with a
 * Cortex-M4, or an M4 with an A7 - asymmetric designs that run two separate images rather than
 * the shared ready lists OS_CONFIG_CORE_COUNT describes. */
#define SOC_CORE_COUNT          1U

#if (OS_CONFIG_CORE_COUNT > SOC_CORE_COUNT)
#error "OS_CONFIG_CORE_COUNT is above 1, which the st/stm32 package does not support: STM32 dual-core parts are asymmetric (M7+M4), not the SMP model the kernel's core count describes."
#endif

/* CubeMX generates main.h for every project, and it includes that family's HAL header itself -
 * so one name is right across the whole STM32 range. It sits in Core/Inc, which is already on the
 * kernel's include path because os_config.h lives there.
 *
 * Whether the HAL is USED is still the application's call: SOC_CONFIG_CLOCK_AUTO_UPDATE and
 * SOC_CONFIG_TICKLESS_HAL_TICK compile the two HAL-touching bodies away when they are 0. */
#include "main.h"


/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TICKLESS_ENABLE == 1U) && (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U)
/* Defined with the LPTIM driver further down; declared here because the init callback above the
 * driver is what calls it. */
static void soc_lptim_init(void);
static void soc_lptim_rate_verify(void);
#endif

/******************************************************************************************************/
/**
 * @brief Prepare the SoC for the kernel. Called by os_init(), first thing.
 *
 * One job on this vendor: make sure SystemCoreClock describes the clock tree the application just
 * configured, because os_init() is about to compute the tick period from it and every
 * os_delay_us() busy-wait reads it afterwards. A stale value gives a tick at the wrong rate with
 * nothing anywhere reporting it, which is a slow and unpleasant thing to find.
 *
 * CubeMX's generated SystemClock_Config() normally leaves the variable correct already, so this
 * is insurance rather than a fix - it costs one call at start-up and makes the guarantee hold for
 * a hand-written clock setup too.
 *
 * Strong, like the RP2 package's, and for the reason the kernel's own weak default spells out: two
 * weak definitions of one symbol are not an error. The linker keeps whichever it reaches first and
 * drops the other without a word. This was weak, the kernel's empty default won that link, and it
 * went unnoticed for as long as the only thing here was SystemCoreClockUpdate() - insurance for
 * something CubeMX had already done. It stopped being unnoticeable the moment an NVIC enable was
 * added below and the LPTIM counted out every window with its interrupt still masked.
 */
void os_arch_soc_init_cb(void)
{
#if (SOC_CONFIG_CLOCK_AUTO_UPDATE != 0U)
    SystemCoreClockUpdate();
#endif

#if (OS_CONFIG_TICKLESS_ENABLE == 1U) && (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U)
    soc_lptim_init();
#endif
}

#if (OS_CONFIG_TICK_SOURCE == OS_CONFIG_TICK_SOURCE_SYSTICK) && (SOC_CONFIG_SYSTICK_VECTOR != 0U)

/******************************************************************************************************/
/**
 * @brief SysTick vector: advance the kernel clock.
 *
 * The kernel's port owns PendSV outright and used to leave this one to the application, which on
 * a CubeMX project meant the installer reaching into a generated SysTick_Handler and writing the
 * same three lines into its USER CODE section - on every project, and again after every
 * regeneration. The package writes it once instead, exactly as the raspberrypi package already
 * does for isr_systick.
 *
 * CubeMX must therefore NOT generate one: Pinout & Configuration -> System Core -> NVIC -> Code
 * generation -> uncheck 'Generate IRQ handler' for 'System tick timer'. Two definitions of one
 * symbol is a link error naming SysTick_Handler, which is a good failure - loud, immediate, and
 * it says which symbol - but the installer checks for it first and explains this rather than
 * leaving it to the linker.
 *
 * STRONG, and deliberately. The CMSIS-Pack startup file defines SysTick_Handler weak, aliased to
 * Default_Handler; between two weak definitions the linker keeps whichever it reached first, and
 * that is the startup object every time. A weak definition here would lose in silence and the
 * kernel clock would simply never advance - the same trap os_arch_soc_init_cb() above records
 * having fallen into once already.
 *
 * HAL_IncTick() is deliberately absent. The HAL runs off its own timer (CubeMX -> SYS -> Timebase
 * Source), so the two time bases never share this interrupt.
 *
 * An application that needs its own work on the tick sets SOC_CONFIG_SYSTICK_VECTOR to 0 and
 * writes SysTick_Handler itself; one that drives the kernel from a different timer entirely
 * selects OS_CONFIG_TICK_SOURCE_EXTERNAL, which removes this along with the port's SysTick
 * programming.
 *
 * @return None.
 */
void SysTick_Handler(void)
{
    os_tick_handler();
}

#endif /* OS_CONFIG_TICK_SOURCE_SYSTICK && SOC_CONFIG_SYSTICK_VECTOR */

/** Referenced by nothing, and that is its entire job.
 *
 *  A static archive gives up an object only when the link still has an undefined symbol that object
 *  defines. Everything in this file fails that test: SysTick_Handler is already DEFINED by the
 *  startup file - weakly, aliased to Default_Handler - and every _cb here has a weak default in the
 *  kernel. So there is never an unresolved reference pointing this way, the linker never extracts
 *  this object, and the whole package loses silently to those defaults.
 *
 *  What that costs is the kernel tick. SysTick's vector keeps the startup file's Default_Handler,
 *  which is `b .`: the first tick jumps into an infinite loop and the board stops dead, with
 *  os_tick_count still 0, SysTick both active and pending, and no fault recorded anywhere to say
 *  why. Found on a Nucleo-G431RB, where it froze twelve characters into the self-test banner.
 *
 *  It stayed hidden for as long as it did because OS_CONFIG_TICKLESS_ENABLE was on in every project
 *  that had run this package. os_tick.c references os_tickless_pre_sleep_cb, the kernel deliberately
 *  ships no default for it, and that one undefined symbol was enough to drag the object - and
 *  SysTick_Handler with it - into the link by accident. Turn tickless off and the accident stops.
 *
 *  soc.cmake names this symbol in a -u link option, which is what forces the extraction.
 *  Unconditional on purpose: a symbol behind the same #if as the things it rescues would disappear
 *  along with them. The raspberrypi packages carry the same anchor for the same reason. */
const uint32_t soc_stm32_anchor = 0U;


#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Configuration rules
 * ***********************************************************************************************************
 *
 * Build failures rather than run-time zeros. Every way this can be set wrong fails SILENTLY on the
 * board - a window that never suppresses, a core that never wakes - and a silent fault here is
 * expensive to find. A refused build naming the settings that disagree costs nothing to read.
*/

/* No wake source to choose, and that is the whole of it: the depth decides. LIGHT leaves this
 * package supplying no source at all, and the port suppresses against SysTick itself - which is
 * exactly right, since SysTick is clocked from the core and the core clock is still running. DEEP
 * gates that clock, so the LPTIM below takes the window instead.
 *
 * The pairing that used to need checking - deep sleep against a source that dies in Stop - cannot
 * be expressed any more, so there is nothing left to check.
 *
 * An RTC wake-up timer would break the tie, being the one source that survives Stop on a series
 * with no LPTIM. When it is written, the source stops being derivable and comes back as an option
 * of its own. */

/* Deep sleep is the application's own line of HAL, because ST has no single Stop entry: the H5
 * takes a regulator argument, the L4/G4/U5 families number their Stop modes, the H7 wants a power
 * domain. Rather than an enum this package would only have to translate back into that exact call,
 * SOC_CONFIG_DEEP_SLEEP IS the call - see the comment on it in template/soc_config.h for the list
 * per series, and for the clock restore that has to follow it.
 *
 * Required only under DEEP, like the LPTIM names below: an option nothing reads is an option
 * nobody should have to fill in. Missing it is refused rather than defaulted, because the default
 * that suggests itself - a plain WFI - is precisely the light sleep this build just said it did
 * not want, and it would save nothing while reporting nothing. */
#if (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U) && !defined(SOC_CONFIG_DEEP_SLEEP)
#error "OS_CONFIG_TICKLESS_DEEP_ENABLE needs SOC_CONFIG_DEEP_SLEEP: this series' HAL Stop entry, \
written out as the statement to run. soc/st/stm32/template/soc_config.h lists the call for every \
family."
#endif

/* Selecting the LPTIM here is only half of it: enabling the peripheral in CubeMX is what brings the
 * HAL module into the project at all. Without that step this would fail as a heap of missing
 * declarations from inside the package, which says nothing about what to do about it. */
#if (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U) && !defined(HAL_LPTIM_MODULE_ENABLED)
#error "OS_CONFIG_TICKLESS_DEEP_ENABLE needs the LPTIM to end its windows, and this project has no \
LPTIM HAL module. In CubeMX \
enable the LPTIM named by SOC_CONFIG_TICKLESS_LPTIM_HANDLE, give it LSI or LSE as its clock, \
leave its NVIC entry OFF - the package owns that - and regenerate."
#endif


#if (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Tickless wake source: LPTIM
 * ***********************************************************************************************************
 *
 * The kernel asks "wake me in N ticks", then "how many ticks really passed".
 *
 * The counter that answers both never stops and is never reset. It is started once, wraps freely
 * over its full range, and a window is a pair of readings from it; the wake is a compare against
 * the value the window should end at. Everything between the two readings is inside the
 * measurement, the time spent arming included.
 *
 * That last point is the design, and it was learned the hard way. An earlier version restarted the
 * timer for each window, and the restart cost about eighteen LPTIM periods of flag waits - REPOK,
 * ARROK, DIEROK, each up to three low-speed cycles - spent with the counter stopped at zero. Half a
 * millisecond of real time per window that no clock in the system was counting. Over twenty
 * eight-tick windows the kernel clock fell ten ticks behind the CPU's own counter: a 6% error on a
 * short window, and under 1% on a long one, which is why it hid so well. A running counter cannot
 * lose time it is not asked about.
 *
 * So nothing is written to DIER once the timer is going and nothing is ever stopped. Arming is one
 * write to the compare register.
 *
 * The peripheral belongs to CubeMX - clock, LSI or LSE mux, prescaler, handle. This borrows it,
 * takes the NVIC entry because the vector has to reach the kernel, and sets the reload to the full
 * range, because a window measured as a difference wants the longest unambiguous span it can get.
*/

/** CubeMX generates one handle per enabled instance, named for it. Borrowed rather than
 *  re-initialised: a second HAL_LPTIM_Init() would fight MX_LPTIM1_Init() for the same registers.
 *
 *  Taken by NAME from soc_config.h rather than built here from an instance number. Pasting a digit
 *  onto a prefix looks tidier and is wrong across the range: the G0 folds LPTIM1 into
 *  TIM6_DAC_LPTIM1_IRQn, so the handle, the NVIC entry and the vector are three independent facts
 *  about a project and only the project can state them.
 *
 *  Aliased rather than used directly, so the rest of this file reads as SOC_LPTIM_* and none of it
 *  has to know where the names came from. */
extern LPTIM_HandleTypeDef          SOC_CONFIG_TICKLESS_LPTIM_HANDLE;

#define SOC_LPTIM_HANDLE            (&SOC_CONFIG_TICKLESS_LPTIM_HANDLE)
#define SOC_LPTIM_IRQN              SOC_CONFIG_TICKLESS_LPTIM_IRQN
#define SOC_LPTIM_IRQ_HANDLER       SOC_CONFIG_TICKLESS_LPTIM_VECTOR

/** A rate this high cannot be a low-power clock: it is the reset default, PCLK3, which means the
 *  CubeMX clock mux was never pointed at LSI or LSE. Left alone it would count the core clock -
 *  dying in any sleep deeper than the lightest, and reaching a window of well under one tick. */
#if (SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ > 1000000UL)
#error "SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ is too high to be a low-speed clock. In CubeMX, Clock \
Configuration tab, point the LPTIM1 mux at LSI or LSE - the default is PCLK3."
#endif

/** The prescaler, pinned here rather than taken from CubeMX, and pinned to /1 - the counter runs
 *  at its source rate.
 *
 *  Pinned rather than configurable because a bigger divider is a bad trade at this tick rate. It
 *  buys a longer window, but arming one waits for the compare write to cross into the counter's
 *  clock domain - three of the PRESCALED periods - so that wait scales straight with the divider.
 *  Measured on an H503 at 32 kHz: 94 us to arm at /1, 3.0 ms at /32. Three milliseconds of
 *  busy-wait with the kernel mask held is most of an 8 ms window, and a board set that way stops
 *  sleeping through short windows at all - its kernel clock falls behind and its interrupt latency
 *  grows by the same 3 ms.
 *
 *  A window longer than the 2047 ticks this gives is not refused, it is just taken in hops: the
 *  kernel clamps each window to the ceiling, so a 5 s delay wakes three times and lands on time.
 *
 *  Written here at all - rather than left to CubeMX - because it used to be CubeMX's while
 *  SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ restated its result by hand, and the two disagreeing made
 *  every window the wrong length with nothing to say so. With the divider pinned they cannot
 *  disagree: that option is simply what the counter counts at.
 *
 *  Written once in soc_lptim_init(), before HAL_LPTIM_Init(), never touched again: this driver's
 *  design rests on a counter started once and never stopped, and changing a prescaler means
 *  stopping it. */
#define SOC_LPTIM_PRESCALER         LPTIM_PRESCALER_DIV1

/** Counts in one kernel tick, ROUNDED DOWN. Used only where a rough size is wanted - the arming
 *  floor below - and deliberately not for measuring a window: see soc_lptim_accum for why one
 *  pre-divided constant cannot express this ratio and what is used instead. Refused below if it
 *  comes out as 0. */
#define SOC_LPTIM_COUNTS_PER_TICK   (SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ / OS_CONFIG_TICK_HZ)

#if (SOC_LPTIM_COUNTS_PER_TICK == 0U)
#error "SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ is slower than OS_CONFIG_TICK_HZ, so one kernel tick \
is less than one LPTIM count and no window could be expressed. Use a faster low-speed clock or a \
slower tick."
#endif

/** The counter's full span: its wrap point, and the modulus every window difference is taken
 *  against. */
#define SOC_LPTIM_PERIOD            0xFFFFUL

/** Ceiling on one window, in counts: three quarters of the span, leaving a quarter as headroom.
 *
 *  The resume path takes its difference modulo the span, so a core still asleep past the compare -
 *  a masked NVIC line, a debugger halt - wraps it and reports a full window as an empty one, and
 *  every deadline then runs late by the difference. A quarter of the span is half a second at
 *  32 kHz. Costs nothing: the ceiling goes from ~2047 ticks to 1536, and windows are bounded by the
 *  next expiry long before this. */
#define SOC_LPTIM_MAX_COUNTS        (((SOC_LPTIM_PERIOD + 1UL) * 3UL) / 4UL)

/** What one window costs to arm, in counts. The compare write takes three low-speed periods to
 *  reach the counter's clock domain and the arming path waits for it; one more is carried for the
 *  read and the arithmetic around it. Everything else - the WFI, the wake, the counter read - is
 *  core-clock work and disappears next to a single low-speed period. */
#define SOC_LPTIM_ARM_COUNTS        4UL

/** Polls allowed while a register write reaches the low-speed domain. Three LPTIM periods is what
 *  it takes, which against a fast core is a great many instructions; the bound is only there so a
 *  clock that never starts cannot hold the idle path forever. */
#define SOC_LPTIM_CMPOK_POLLS       100000UL

/*
 * ***********************************************************************************************************
 * Two generations of LPTIM HAL, one driver
 * ***********************************************************************************************************
 *
 * ST rewrote the LPTIM HAL part-way through the range, and the two versions are not
 * source-compatible. Everything below is written against the difference rather than against one of
 * them, because a package that says "STM32" and only builds on half of it is worse than one that
 * says which half.
 *
 *   CHANNELLED (H5, U5, H7 recent, WBA)   The compare belongs to a channel: LPTIM_CHANNEL_1, its
 *                                         own CC1/CMP1OK flags, an Init.Period field, and
 *                                         HAL_LPTIM_PWM_Start_IT(h, channel). Writes to DIER are
 *                                         themselves acknowledged, through DIEROK.
 *   LEGACY (G0, G4, L0, L4, L5, F7, WB)   One compare, no channels: CMPM/CMPOK, no Init.Period -
 *                                         the reload is an argument to
 *                                         HAL_LPTIM_PWM_Start_IT(h, Period, Pulse) - and no DIEROK.
 *
 * LPTIM_CHANNEL_1 is the discriminator: it exists only in the channelled headers. What is shared -
 * HAL_LPTIM_Init, HAL_LPTIM_ReadCounter, HAL_LPTIM_IRQHandler, the CompareMatch and
 * AutoReloadMatch callbacks, __HAL_LPTIM_GET_FLAG and __HAL_LPTIM_CLEAR_FLAG - is used directly
 * and appears nowhere here.
 */
#if defined(LPTIM_CHANNEL_1)

#define SOC_LPTIM_FLAG_MATCH        LPTIM_FLAG_CC1
#define SOC_LPTIM_FLAG_CMPOK        LPTIM_FLAG_CMP1OK
#define SOC_LPTIM_COMPARE_SET(v)    __HAL_LPTIM_COMPARE_SET(SOC_LPTIM_HANDLE, LPTIM_CHANNEL_1, (v))
#define SOC_LPTIM_START_IT()        HAL_LPTIM_PWM_Start_IT(SOC_LPTIM_HANDLE, LPTIM_CHANNEL_1)

/* The reload is a handle field here, applied by HAL_LPTIM_Init. */
#define SOC_LPTIM_PERIOD_APPLY()    do { SOC_LPTIM_HANDLE->Init.Period = SOC_LPTIM_PERIOD; } while (0)

/* CMP1OK is not wanted as an interrupt: every window writes the compare register and the arming
 * path waits on that very flag, so a handler would clear it before the wait could see it and the
 * interrupt left pending would drop the following sleep straight back out. Disabling it is itself a
 * DIER write, which this generation acknowledges through DIEROK - hence the wait. */
#define SOC_LPTIM_QUIET_CMPOK()                                                                   \
    do {                                                                                          \
        __HAL_LPTIM_CLEAR_FLAG(SOC_LPTIM_HANDLE, LPTIM_FLAG_DIEROK);                              \
        __HAL_LPTIM_DISABLE_IT(SOC_LPTIM_HANDLE, LPTIM_IT_CMP1OK);                                \
        soc_lptim_write_settle(LPTIM_FLAG_DIEROK);                                                \
    } while (0)

#else

#define SOC_LPTIM_FLAG_MATCH        LPTIM_FLAG_CMPM
#define SOC_LPTIM_FLAG_CMPOK        LPTIM_FLAG_CMPOK
#define SOC_LPTIM_COMPARE_SET(v)    __HAL_LPTIM_COMPARE_SET(SOC_LPTIM_HANDLE, (v))

/* Period and Pulse are start-up arguments rather than handle fields. The pulse is 0 because no
 * output pin is routed; the first real compare is written by the first window. */
#define SOC_LPTIM_START_IT()        HAL_LPTIM_PWM_Start_IT(SOC_LPTIM_HANDLE, SOC_LPTIM_PERIOD, 0U)
#define SOC_LPTIM_PERIOD_APPLY()    do { } while (0)

/* Same reasoning as the channelled branch, minus the DIEROK acknowledgement this generation does
 * not have: the DIER write here takes effect directly. */
#define SOC_LPTIM_QUIET_CMPOK()                                                                   \
    do {                                                                                          \
        __HAL_LPTIM_DISABLE_IT(SOC_LPTIM_HANDLE, LPTIM_IT_CMPOK);                                 \
    } while (0)

#endif /* LPTIM_CHANNEL_1 */

/** Counter reading the open window started from. */
static uint32_t soc_lptim_start = 0U;

/** Counts the open window was armed for; 0 when no window is open. */
static uint32_t soc_lptim_armed = 0U;

/** Time this driver has measured but not yet announced, in counts x OS_CONFIG_TICK_HZ.
 *
 * Two separate errors used to live where this now sits, and neither was visible in a single window.
 *
 *  1. SOC_LPTIM_COUNTS_PER_TICK is an integer division. At the usual 32768 Hz against a 1 kHz tick
 *     it is 32, so a "tick" measured against it is 32/32768 s = 976.6 us rather than 1000. Every
 *     window therefore ran the kernel clock 2.4% FAST, while SysTick outside the window kept it
 *     right - an error that changes sign depending on how much of the second was spent asleep.
 *  2. The leftover counts of the final, incomplete tick were dropped: elapsed was a truncating
 *     division and the next window re-read its reference from the counter, so up to a whole tick
 *     went missing per window - this time SLOW. The two did not cancel; their sum moved with the
 *     workload.
 *
 * Scaling by OS_CONFIG_TICK_HZ instead of pre-dividing removes both. A count is worth TICK_HZ
 * units and a tick is worth SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ of them, which is exact for every
 * ratio including the ones that are not whole. What is left after the whole ticks are taken out
 * stays here and is spent by a later window, so the announced time converges on the real elapsed
 * time rather than drifting away from it at a fixed rate.
 *
 * Always below SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ, by construction. 64-bit because the
 * multiplication is by a frequency and this is the idle path, where one library division per
 * WINDOW - not per tick - costs nothing worth counting. */
static uint64_t soc_lptim_accum = 0U;

/******************************************************************************************************/
/**
 * @brief Counts to wait for a given number of whole kernel ticks. Rounded UP: a window must never
 *        end before the tick it was asked for.
 *
 * soc_lptim_accum is deliberately NOT subtracted here, and getting that wrong is worth a note
 * because it looks like the symmetric thing to do and it is not. The accumulator holds time that has
 * already ELAPSED and merely has not been announced yet - it sits behind the reference this window
 * is about to take. The kernel deadline is ticks from NOW. Netting the accumulator off makes the
 * window end that much before the deadline, while os_arch_tick_resume_cb() adds the same amount back
 * into elapsed - so the kernel is told a full window passed when it did not. The clock then runs
 * fast, every deadline looks nearer than it is, and the next window is planned shorter still. On the
 * H503 that fed back until a 50-tick sleep measured 5.
 *
 * @param[in] ticks  Kernel ticks the window should cover.
 * @return uint32_t  Counts to arm.
 */
static uint32_t soc_lptim_ticks_to_counts(uint32_t ticks)
{
    uint64_t wanted = (uint64_t)ticks * (uint64_t)SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ;

    return (uint32_t)((wanted + (uint64_t)OS_CONFIG_TICK_HZ - 1U) / (uint64_t)OS_CONFIG_TICK_HZ);
}

/******************************************************************************************************/
/**
 * @brief Read the counter as one whole sample.
 *
 * CNT is clocked in the low-speed domain and read from the bus, so a single read can catch it
 * mid-update. Two equal reads in a row is the reference manual's answer. It matters more here than
 * it would with a counter that restarted every window: a torn sample of a free-running counter does
 * not produce a slightly wrong elapsed, it produces an absurd one.
 *
 * HAL_LPTIM_ReadCounter is the single read, so the repetition is this function's job.
 *
 * @return uint32_t  Counter value.
 */
static uint32_t soc_lptim_count_get(void)
{
    uint32_t first;
    uint32_t second;

    do
    {
        first  = HAL_LPTIM_ReadCounter(SOC_LPTIM_HANDLE);
        second = HAL_LPTIM_ReadCounter(SOC_LPTIM_HANDLE);
    } while (first != second);

    return second;
}

/******************************************************************************************************/
/**
 * @brief Wait, with a bound, for a register write to reach the low-speed domain.
 *
 * Writes to the LPTIM's compare and interrupt registers cross into the counter's own clock domain
 * and are not live until the matching flag says so; a second write over one still in flight is
 * lost. Three low-speed periods is what the reference manual asks for. The poll bound is not a
 * timeout in any useful sense - it is there so that a clock which never starts cannot hold the idle
 * path forever.
 *
 * @param[in] flag  SOC_LPTIM_FLAG_CMPOK, or LPTIM_FLAG_DIEROK where that generation has one.
 * @return None.
 */
static void soc_lptim_write_settle(uint32_t flag)
{
    uint32_t polls = 0U;

    while ((__HAL_LPTIM_GET_FLAG(SOC_LPTIM_HANDLE, flag) == 0U) && (polls < SOC_LPTIM_CMPOK_POLLS))
    {
        polls++;
    }
}

/******************************************************************************************************/
/**
 * @brief Refuse to run if the counter's real clock is not the one this project declared.
 *
 * SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ is hand-written, and nothing downstream can tell it from the
 * truth: the driver arms in counts and converts the same counts back with the same constant, so a
 * wrong rate makes every window the wrong LENGTH while the arithmetic stays self-consistent. Seen
 * for real on an H743 whose CubeMX project left the mux on the APB at 120 MHz while this option
 * still said 32768 - a clean build, every guard passing, and no symptom to follow.
 *
 * The mux register is read rather than HAL_RCCEx_GetPeriphCLKFreq(), which looks like the obvious
 * call and is not: the H7 implementation has no LPTIM case and answers 0, so on the one family that
 * produced the bug it would check nothing. What the mux says is also more than a rate - a source
 * that is neither LSI nor LSE is refused outright, whatever it runs at, because this file is
 * compiled only for deep sleep and Stop mode gates every other clock the mux offers.
 *
 * Exact, with no tolerance band: LSI_VALUE and LSE_VALUE are nominal numbers from the same headers
 * CubeMX wrote the project against, not measurements, so the two either name one clock or they name
 * two. A band wide enough to be worth having would have let 32768-against-32000 through - 2.4% on
 * every window. A real oscillator's own spread is a different question, invisible here.
 *
 * Runs after HAL_LPTIM_Init(), because the generated MspInit is what programs the mux, and is
 * unconditional rather than an OS_ASSERT: a build with assertions compiled out is the last one that
 * can afford to sleep for the wrong length of time silently. Only LPTIM1 is checked - every family
 * spells the accessor for its other instances differently, and all of them name that one.
 */
static void soc_lptim_rate_verify(void)
{
#if defined(LPTIM1) && defined(RCC_LPTIM1CLKSOURCE_LSI) && defined(RCC_LPTIM1CLKSOURCE_LSE)
    if (SOC_LPTIM_HANDLE->Instance == LPTIM1)
    {
        uint32_t src = __HAL_RCC_GET_LPTIM1_SOURCE();

        /* Zero unless the mux names a clock that survives Stop, so any other source fails the
         * comparison below on its own - no legal value of the option is 0. */
        uint32_t actual = 0U;

        if (src == RCC_LPTIM1CLKSOURCE_LSI)
        {
            actual = (uint32_t)LSI_VALUE;
        }
        else if (src == RCC_LPTIM1CLKSOURCE_LSE)
        {
            actual = (uint32_t)LSE_VALUE;
        }

        if (actual != (uint32_t)SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ)
        {
            os_arch_config_fault_trap();
        }
    }
#endif
}

/******************************************************************************************************/
/**
 * @brief Start the counter and take the interrupt vector. Called once, from os_arch_soc_init_cb().
 *
 * Everything expensive lives here so that arming a window later is a single register write.
 *
 * @return None.
 */
static void soc_lptim_init(void)
{
    /* The least urgent level there is, which is where the port also puts SysTick and for a stronger
     * version of the same reason: the tick at least does scheduling bookkeeping, while this handler
     * does nothing at all. Neither has any business delaying a device interrupt.
     *
     * The wake does not depend on the priority. A WFI leaves on a pending interrupt even while the
     * kernel mask holds it off, so the core is running again before this ever executes. */
    NVIC_SetPriority(SOC_LPTIM_IRQN, (1UL << __NVIC_PRIO_BITS) - 1UL);
    NVIC_EnableIRQ(SOC_LPTIM_IRQN);

    /* CubeMX picks a reload for whatever it imagined the timer was for; a free-running counter wants
     * the whole range. The rest of the handle - clock source, prescaler - is left as generated,
     * which is the part CubeMX is authoritative about. */
    SOC_LPTIM_PERIOD_APPLY();
    SOC_LPTIM_HANDLE->Init.Clock.Prescaler = SOC_LPTIM_PRESCALER;

    if (HAL_LPTIM_Init(SOC_LPTIM_HANDLE) == HAL_OK)
    {
        /* Now, and not earlier: the mux this checks is programmed by the generated MspInit that
         * HAL_LPTIM_Init just ran. */
        soc_lptim_rate_verify();

        /* Continuous counting with a compare interrupt, started and never stopped again. The name
         * describes the output waveform, which is nothing to do with this: no LPTIM output pin is
         * routed. What matters is the order inside, which is the order the peripheral insists on
         * and which this driver got wrong by hand - enable, then the interrupt enables with their
         * DIEROK wait around them, then the compare channel, then start.
         *
         * The wrap interrupt comes with it. That fires once per full span of the counter, which is
         * the same 2 seconds as the longest window this source will ever be asked for, so it costs
         * at most one extra wake per window and usually none. */
        if (SOC_LPTIM_START_IT() == HAL_OK)
        {
            /* The compare-ok interrupt is not wanted; see SOC_LPTIM_QUIET_CMPOK above for why, and
             * for what each HAL generation needs in order to do it. ARROK, REPOK and UPDATE are
             * left as they are: they answer writes to registers this driver never touches after
             * start-up. */
            SOC_LPTIM_QUIET_CMPOK();
        }
    }
}

/******************************************************************************************************/
/**
 * @brief The LPTIM vector.
 *
 * @return None.
 */
void SOC_LPTIM_IRQ_HANDLER(void)
{
    HAL_LPTIM_IRQHandler(SOC_LPTIM_HANDLE);
}

/******************************************************************************************************/
/**
 * @brief HAL's compare-match callback - the end of a window. Deliberately empty.
 *
 * The wake is the whole product. How long the window really was is read from the counter when the
 * kernel asks, not recorded here, so a callback that arrives late - or not at all, because another
 * interrupt woke the core first - cannot change the answer.
 *
 * @param[in] hlptim  The instance HAL is reporting for.
 * @return None.
 */
void HAL_LPTIM_CompareMatchCallback(LPTIM_HandleTypeDef *hlptim)
{
    (void)hlptim;
}

/******************************************************************************************************/
/**
 * @brief HAL's wrap callback. Empty for the same reason, and this one is not even a window ending -
 *        just the free-running counter going round.
 *
 * @param[in] hlptim  The instance HAL is reporting for.
 * @return None.
 */
void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef *hlptim)
{
    (void)hlptim;
}

/******************************************************************************************************/
/**
 * @brief How many ticks one window may skip.
 *
 * @return uint32_t  Ceiling in ticks.
 */
uint32_t os_arch_tick_suppress_max_cb(void)
{
    /* Whole ticks that fit in the longest window this counter can express, on the same exact ratio
     * soc_lptim_accum uses. The pre-divided form this replaced answered 2048 where the counter can
     * really only hold 1999 (at 32768 Hz / 1 kHz): the kernel then planned a window the arming path
     * silently clamped, and reported the shortfall as time that never happened. */
    return (uint32_t)(((uint64_t)SOC_LPTIM_MAX_COUNTS * (uint64_t)OS_CONFIG_TICK_HZ) /
                      (uint64_t)SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ);
}

/******************************************************************************************************/
/**
 * @brief The shortest window worth sleeping through.
 *
 * A window has to last longer than it takes to set up, or the sleep costs more than it saves. Here
 * that is the compare write settling in the low-speed domain, rounded up to whole ticks.
 *
 * At the usual ratio - a 32 kHz source against a 1 kHz tick - this is one tick, well under the two
 * the kernel insists on anyway, so it decides nothing. It starts deciding when the ratio moves: a
 * faster tick, or a source divided down by a prescaler, and a couple of counts is suddenly several
 * ticks.
 *
 * Deep sleep does not add a term here, and the reason is arithmetic rather than principle: measured
 * on an H503 over 783 windows, a deep one ends 240 us late on average and 500 us at worst - Stop
 * exit, the PLL relock, and this counter's own 31.25 us step - against a floor the kernel already
 * holds at two whole tick periods. That lateness is charged to the deadline and to interrupt
 * latency, never to the clock: the counter runs through Stop and is read after the restore, so the
 * elapsed figure is the truth. A part with a slower wake, or an application whose
 * SOC_CONFIG_DEEP_SLEEP relocks a long-settling PLL, is where the term starts to bind - measure it
 * before adding it.
 *
 * @return uint32_t  Floor on one window, in ticks.
 */
uint32_t os_arch_tick_suppress_min_cb(void)
{
    return (uint32_t)((SOC_LPTIM_ARM_COUNTS + (uint32_t)SOC_LPTIM_COUNTS_PER_TICK - 1UL) /
                      (uint32_t)SOC_LPTIM_COUNTS_PER_TICK);
}

/******************************************************************************************************/
/**
 * @brief Open a window of `ticks` tick periods.
 *
 * @param[in] ticks  Tick periods to sleep.
 * @return None.
 */
void os_arch_tick_suppress_cb(uint32_t ticks)
{
    uint32_t counts = soc_lptim_ticks_to_counts(ticks);

    if (counts > SOC_LPTIM_MAX_COUNTS)
    {
        counts = SOC_LPTIM_MAX_COUNTS;
    }

    /* Read before anything else: this is where the window starts, and every microsecond after it -
     * the compare write, the wait below, the sleep itself - is inside what gets measured. */
    soc_lptim_start = soc_lptim_count_get();
    soc_lptim_armed = counts;

    /* Cleared before the new target is written rather than after. The old target is still live for
     * the length of the wait below, so it could match in there and leave a flag that wakes this
     * window instantly - a wasted idle pass, and harmless. Clearing afterwards would instead risk
     * discarding a real match, which is a sleep that never ends. */
    __HAL_LPTIM_CLEAR_FLAG(SOC_LPTIM_HANDLE, SOC_LPTIM_FLAG_MATCH);
    __HAL_LPTIM_CLEAR_FLAG(SOC_LPTIM_HANDLE, SOC_LPTIM_FLAG_CMPOK);

    /* The peripheral flag and the NVIC's latched pending bit are two separate things, and clearing
     * the first above leaves the second exactly where it was. That matters here and almost nowhere
     * else: a WFI with anything already pending is a no-op, so a bit left over from the window that
     * just ended drops the sleep below straight back out. Under LIGHT that costs one wasted idle
     * pass; under DEEP it is a Stop mode that is never entered at all, with every power figure it
     * was chosen for quietly failing to appear and nothing saying why.
     *
     * Nothing is lost by clearing it. The match it refers to has already been paid for - the kernel
     * measures a window from the counter, never from this interrupt - and the new target goes in on
     * the next line, so a real match from here on sets the bit again and ends the sleep. */
    NVIC_ClearPendingIRQ(SOC_LPTIM_IRQN);

    SOC_LPTIM_COMPARE_SET((soc_lptim_start + counts) & (uint32_t)SOC_LPTIM_PERIOD);

    /* Waited out rather than left to land during the sleep, because the next window can arrive
     * sooner than three low-speed periods and a compare written over one still in flight is lost.
     * It costs nothing in accuracy: the counter is running through the wait, so the wait is inside
     * the window being measured. Which is the whole point of this driver. */
    soc_lptim_write_settle(SOC_LPTIM_FLAG_CMPOK);
}

/******************************************************************************************************/
/**
 * @brief Close the window and report the whole tick periods that really elapsed.
 *
 * @return uint32_t  Whole tick periods since os_arch_tick_suppress_cb().
 */
uint32_t os_arch_tick_resume_cb(void)
{
    uint32_t elapsed = 0U;

    if (soc_lptim_armed != 0U)
    {
        /* The counter never stopped, so one subtraction is the entire window: the arming, the
         * sleep, and whatever ended it. Masked to the counter's span, which is what makes a wrap
         * inside the window come out right. */
        uint32_t counts = (soc_lptim_count_get() - soc_lptim_start) & (uint32_t)SOC_LPTIM_PERIOD;

        /* Banked first, then spent. The whole ticks come out; the fraction of a tick that is left
         * stays in the accumulator for the next window instead of being thrown away, which is what
         * stops the clock losing up to a tick every time it sleeps. See soc_lptim_accum. */
        soc_lptim_accum += (uint64_t)counts * (uint64_t)OS_CONFIG_TICK_HZ;

        elapsed = (uint32_t)(soc_lptim_accum / (uint64_t)SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ);

        soc_lptim_accum -= (uint64_t)elapsed * (uint64_t)SOC_CONFIG_TICKLESS_LPTIM_CLOCK_HZ;
        soc_lptim_armed  = 0U;
    }

    return elapsed;
}

#endif /* OS_CONFIG_TICKLESS_DEEP_ENABLE */

/* SysTick is the port's own, so this package supplies none of the three suppress callbacks for it
 * - os_arch_port_v8m.c reprograms the reload itself. The weak defaults in the port answer 0, which
 * is exactly right: nothing here to arm, nothing here to measure. */

/******************************************************************************************************/
/**
 * @brief Called right before the idle sleep: stop the HAL timebase so the sleep runs its length.
 *
 * This is the one thing that makes tickless idle worth enabling on an STM32, and the reason it
 * needs saying is that leaving the hook empty fails silently rather than loudly.
 *
 * The kernel owns SysTick, which means CubeMX has been told to run the HAL from a spare timer
 * (SYS -> Timebase Source). That timer keeps firing at its own period, 1 kHz by default, and WFI
 * wakes on any pending interrupt whether or not it is masked. So a sleep the kernel planned for
 * 500 ms ends after 1 ms, every time, and tickless idle looks like it simply does not work.
 *
 * HAL_SuspendTick() is ST's own hook for this. HAL_GetTick() does not advance while suspended,
 * which loses no time - the counter just does not run while the CPU is asleep - but it does mean
 * a HAL driver's own timeout cannot be relied on across the sleep window.
 *
 * Deliberately does NOT select a deeper mode, and that is not the omission it once was: how deep
 * the core goes is SOC_CONFIG_DEEP_SLEEP, run by os_arch_soc_sleep_cb() below at the moment of
 * the sleep. It has to be there rather than here, because the wake source is not armed yet when
 * this runs - os_arch_sleep_prepare() does that on the line after - and a Stop entered from here
 * would have nothing left to end it.
 */
OS_WEAK void os_tickless_pre_sleep_cb(void)
{
#if (SOC_CONFIG_TICKLESS_HAL_TICK != 0U)
    HAL_SuspendTick();
#endif
}

/******************************************************************************************************/
/**
 * @brief Called right after wakeup: restart the HAL timebase.
 *
 * Runs with the kernel's interrupts still masked and before the sleep has been announced, so it
 * must stay short: its whole duration is added to interrupt latency.
 *
 * The clock restore a Stop mode needs does NOT go here - it is already done, in
 * os_arch_soc_sleep_cb() below, on the instruction after the wake. By the time this runs the
 * tree is back and HAL_ResumeTick() is resuming against the clock it was configured for.
 */
OS_WEAK void os_tickless_post_sleep_cb(void)
{
#if (SOC_CONFIG_TICKLESS_HAL_TICK != 0U)
    HAL_ResumeTick();
#endif
}

/******************************************************************************************************/
/**
 * @brief The sleep itself: a plain WFI under LIGHT, this series' own Stop entry under DEEP.
 *
 * Called with the window already armed - the tick silenced, the LPTIM counting the wake out - which
 * is what makes a Stop mode safe to enter here and nowhere else in the idle path. The LPTIM runs
 * from LSI or LSE, so it counts straight through Stop; the pairing rule at the top of this file is
 * what guarantees the source under it is one of those.
 *
 * Under DEEP this is one statement the application wrote, run verbatim. The package does not wrap
 * it, check it or undo it afterwards: ST's Stop entries already set SLEEPDEEP, execute the WFI and
 * clear SLEEPDEEP again on the way out, and anything else the mode costs - a clock restore above
 * all - belongs in the same statement, where it runs before this returns.
 *
 * Strong rather than weak, and for the reason os_arch_soc_init_cb() spells out at the top of this
 * file: two weak definitions of one symbol are not an error, the linker keeps whichever it reaches
 * first, and the one it dropped would be this. A build that silently linked the kernel's WFI
 * instead would enter no Stop mode at all and report nothing.
 *
 * @return None.
 */
#if (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U)
/* CubeMX generates this into main.c on every project it makes, and generates it NON-static -
 * it simply never puts a prototype in main.h. Declared here rather than asking each application
 * to edit a generated header. A project that has no such function fails to LINK, naming the
 * symbol, and only under DEEP: write your own SystemClock_Config() that reprograms the tree, or
 * turn the deep sleep off. */
void SystemClock_Config(void);
#endif

void os_arch_soc_sleep_cb(void)
{
#if (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U)
#if (OS_CONFIG_TEST_ENABLE == 1U)
    /* Counted before the entry, not after: the wake path below must not decide whether this
     * sleep happened. See os_test_deep_sleep_entries in ahura.h. */
    os_test_deep_sleep_entries++;
#endif
    SOC_CONFIG_DEEP_SLEEP();

    /* Stop gates the PLL and drops the core onto HSI or CSI, so the wake returns to a machine
     * running at a fraction of the speed it went to sleep at - and nothing anywhere reports it.
     * The tick period, every UART baud rate and every os_delay_us() would be wrong from here to
     * the end of the run. Putting the clock back is not optional, which is exactly why it is in
     * the package rather than left as a line for each application to remember.
     *
     * Here and not in os_tickless_post_sleep_cb(), which is only microseconds later: the elapsed
     * measurement between them, and HAL_ResumeTick() inside it, would run against a clock tree
     * that is not the one they were set up for.
     *
     * Two costs, both real and neither obvious. HSE and the PLL have to relock, measured at
     * about 35000 core cycles per window on an H503 - charged to interrupt latency, since the
     * kernel mask is still held. And HAL_RCC_ClockConfig() ends by refreshing SystemCoreClock
     * through HAL_RCC_GetSysClockFreq(), which on the H5 and several other families computes
     * the PLL ratio in FLOAT. That sets CONTROL.FPCA on whichever task ran the sleep, and every
     * later context switch of that task carries the FP register file: about 63 cycles more each
     * way, for the life of the task. Normally that task is idle and it costs nothing anyone can
     * measure; it is worth knowing before driving a deep sleep by hand from a latency-critical
     * one. */
    SystemClock_Config();
#else
    OS_ARCH_IDLE();
#endif
}

#endif /* OS_CONFIG_TICKLESS_ENABLE */
