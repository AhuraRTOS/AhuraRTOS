/**
 * @file soc_cb.c
 * @brief RP2350/RP2354-specific SoC code, Arm cores: the inter-core interrupt.
 *
 * Everything else the kernel needs from this chip is in ../common/soc_common.c, which is compiled
 * into this package: the core id, the spinlock, the CPU clock, the SysTick vector and booting
 * core 1 are all identical on the RP2350 and the RP2040, because they are SIO or plain SDK either
 * way.
 *
 * What is here is the one genuine difference today. The RP2350 adds doorbells - a purpose-built
 * inter-core interrupt - so a core signals the other by ringing one, leaving the FIFO free for
 * the SDK and the application. The RP2040 package does the same job through the FIFO because it
 * has no doorbells.
 *
 * This is also where RP2350-only work lands as it arrives, TrustZone first: the Cortex-M33 has
 * the Security Extension and the RP2040's Cortex-M0+ does not, so os_arch_tz_context_save_cb()
 * and os_arch_tz_context_restore_cb() can only ever be implemented on this side of the split.
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

#include "soc_common.h"

#include "hardware/irq.h"
#include "hardware/powman.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/nvic.h"
#include "pico/multicore.h"
#include "pico/time.h"

/** Referenced by nothing, and that is its entire job.
 *
 *  Every other symbol in this file is either a callback the kernel gives a weak default to, or is
 *  compiled out on a single-core build. So on such a build nothing in the link names anything here,
 *  the linker never extracts this object from the archive at all, and every callback it holds loses
 *  silently to a weak default - the POWMAN wake source simply vanishes and the port falls back to
 *  SysTick, which is how it was found: a self-test reporting a 112-tick ceiling where a 32-bit one
 *  was expected.
 *
 *  soc.cmake names this in a -u link option, which is what forces the extraction. It exists
 *  unconditionally on purpose: a symbol behind the same #if as the things it is meant to rescue
 *  would disappear with them. */
const uint32_t soc_rp235x_arm_anchor = 0U;


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

#if !defined(SOC_CONFIG_SLEEP_MODE)
#error "soc_config.h is incomplete: SOC_CONFIG_SLEEP_MODE is required when OS_CONFIG_TICKLESS_ENABLE is 1."
#elif (SOC_CONFIG_SLEEP_MODE != OS_CONFIG_SLEEP_MODE_LIGHT) && \
    (SOC_CONFIG_SLEEP_MODE != OS_CONFIG_SLEEP_MODE_DEEP)
#error "SOC_CONFIG_SLEEP_MODE must be OS_CONFIG_SLEEP_MODE_LIGHT or OS_CONFIG_SLEEP_MODE_DEEP."
#endif

/* No wake source to choose: the depth decides it. LIGHT keeps the clocks running and DEEP gates
 * them, and the POWMAN timer below serves both - it lives in the always-on domain, so it is the
 * one alarm on this part that outlives clk_sys. SysTick and the TIMER blocks do not.
 *
 * It used to be five flags plus the mode, with an arithmetic rule saying exactly one flag had to
 * be 1, another naming the sources this chip does not physically have, and a third refusing deep
 * sleep against a source that stops with the clocks. None of those states can be expressed any
 * more, so none of those rules exists.
 *
 * DEEP stops the PLL. The cheap way was tried first and measured: zeroing SLEEP_EN0/1 with
 * SLEEPDEEP set gates peripheral clocks but not the generator the core runs from, and the
 * self-test's cycle counter went on counting straight through a 20-window sleep - 24 million
 * cycles, exactly a plain WFI. So the sleep below drops clk_sys back onto clk_ref and powers the
 * PLL down, which is where this part's idle power actually goes.
 *
 * clk_sys feeds BOTH cores. The prepare hook below first parks core 1 cooperatively in its
 * idle task, with all configurable interrupts masked, before the kernel opens the time window.
 * A busy peer or peripheral declines the pass. Neither core may resume normal execution until
 * the clocks are restored; core 1 stays parked until the kernel has also reconciled its time.
 *
 * It deliberately stops short of the deepest route - moving clk_ref onto LPOSC and stopping the
 * crystal as well. That saves more and is also the one where a mistake leaves the core with no
 * clock to execute the restore from. See os_arch_soc_sleep_cb() below. */


/* Milliseconds of POWMAN timer in one kernel tick. The timer is driven from a 1 kHz tick source,
 * so a millisecond IS a count - and at the default 1 kHz kernel tick that is one count per tick,
 * which is as clean as this arithmetic ever gets. */
#define SOC_POWMAN_MS_PER_TICK      (1000UL / OS_CONFIG_TICK_HZ)

#if (SOC_POWMAN_MS_PER_TICK == 0U)
#error "OS_CONFIG_TICK_HZ is above 1000, so one kernel tick is less than one millisecond and the \
POWMAN timer cannot express a window. Use a slower tick, or light sleep."
#endif

/** Timer reading when the open window started, in milliseconds. */
static uint64_t soc_powman_entry_ms = 0U;

/** Whether soc_powman_entry_ms describes a window that is actually running.
 *
 * The kernel raises its own os_tickless_window_open BEFORE it plans, and only calls
 * os_arch_tick_suppress_cb afterwards - and not at all when the plan comes out too short to sleep.
 * So "the kernel says a window is open" is NOT the same as "this timer has an entry stamp", and a
 * peek in that gap would subtract the PREVIOUS window's stamp and report an arbitrarily large
 * elapsed time. That is a clock that races, not one that lags.
 *
 * Written by the time-base owner only, read by any core. Raised after the stamp and lowered before
 * the close touches the accumulator, so a peer that sees it true is looking at a whole,
 * still-running window. */
static __IO bool soc_powman_window_armed = false;

/** Time measured but not yet announced, in milliseconds x OS_CONFIG_TICK_HZ.
 *
 * A window's last, incomplete tick used to be dropped: elapsed was a truncating division and the
 * next window re-read its reference from the timer, so up to one whole tick went missing EVERY time
 * the core slept. Nothing in a single window shows it; a run that sleeps once a second loses a
 * second every few minutes.
 *
 * Scaling by OS_CONFIG_TICK_HZ rather than pre-dividing also makes the conversion exact for tick
 * rates that do not divide 1000 - 300 Hz, 768 Hz - where SOC_POWMAN_MS_PER_TICK is a rounded number
 * and every window inherited its error. What is left after the whole ticks are taken out stays here
 * and is spent by a later window, so announced time converges on real time.
 *
 * Always below 1000, by construction. */
static uint64_t soc_powman_accum_ms_hz = 0U;

/** Raised once the timer is running and its vector is taken. */
static bool     soc_powman_ready    = false;

/******************************************************************************************************/
/**
 * @brief The POWMAN alarm vector: the wake is the whole product, so this only clears the alarm.
 *
 * How long the window really was is read from the timer when the kernel asks, not recorded here,
 * so an alarm that arrives late - or not at all, because another interrupt woke the core first -
 * cannot change the answer.
 *
 * @return None.
 */
static void soc_powman_isr(void)
{
    powman_clear_alarm();
}

/******************************************************************************************************/
/**
 * @brief Start the always-on timer and take its vector, once.
 *
 * Deferred to first use rather than done at SoC init, so a build that never suppresses pays
 * nothing for it.
 *
 * Started only if it is not already running: this timer belongs to the always-on domain, it
 * survives what puts the rest of the chip to sleep, and an application may be keeping wall-clock
 * time on it. Restarting it would move that clock.
 *
 * @return bool  True once the timer is usable.
 */
static bool soc_powman_ready_get(void)
{
    if (!soc_powman_ready)
    {
        if (!powman_timer_is_running())
        {
            /* LPOSC rather than XOSC: it is the one that keeps running when the crystal and the
             * PLLs do not, which is the entire reason this timer is the deep-sleep wake source.
             *
             * MEASURED rather than assumed, and that is not defensive - it was measured wrong
             * first. LPOSC is an untrimmed RC oscillator, and powman_timer_set_1khz_tick_source_lposc()
             * trims it only from an OTP calibration row; where that row is blank the SDK's
             * powman_timer_get_lposc_calib_freq() returns 0, and _with_hz(0) then quietly skips the
             * frequency write and leaves the nominal 32.768 kHz in place. On this board the real
             * oscillator is about 9% away from that, which came out as every window running 9%
             * long: the kernel clock fell 13 ticks behind over 20 windows and the cycle counter
             * disagreed with the tick by 9%. Both were caught by the self-test rather than by a
             * battery.
             *
             * The frequency counter reads it against the crystal, which is accurate and is still
             * running here at start-up, so one measurement at init costs a few milliseconds once
             * and makes the window lengths mean what they say. */
            uint32_t lposc_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_LPOSC_CLKSRC);

            if (lposc_khz != 0U)
            {
                powman_timer_set_1khz_tick_source_lposc_with_hz(lposc_khz * 1000U);
            }
            else
            {
                /* No reading at all means the counter could not see it; the nominal rate is a
                 * worse answer than none, but a stopped timer is worse still. */
                powman_timer_set_1khz_tick_source_lposc();
            }

            powman_timer_start();
        }

        irq_set_exclusive_handler(POWMAN_IRQ_TIMER, soc_powman_isr);
        soc_powman_ready = true;
    }

    return soc_powman_ready;
}

/******************************************************************************************************/
/**
 * @brief How many ticks one window may skip.
 *
 * No ceiling of this timer's own. It is 64 bits and the alarm is an ABSOLUTE deadline, so there is
 * no wrap for a window to fall foul of - what bounds one here is the kernel's own 32-bit tick
 * count, which is what this reports. The rp2040's alarm needs a cap because it compares the low 32
 * bits of a 1 MHz counter; this one does not.
 *
 * @return uint32_t  Ceiling in ticks.
 */
uint32_t os_arch_tick_suppress_max_cb(void)
{
    return soc_powman_ready_get() ? UINT32_MAX : 0U;
}

/******************************************************************************************************/
/**
 * @brief The shortest window worth sleeping through.
 *
 * A millisecond is this timer's whole resolution, so a window has to span at least two of them
 * before its length can be told from rounding. At a 1 kHz tick that is two ticks, which is already
 * the floor the kernel insists on regardless - so this decides nothing today. It is here so that a
 * faster tick, where a millisecond spans several ticks, raises the bar by itself.
 *
 * @return uint32_t  Floor on one window, in ticks.
 */
uint32_t os_arch_tick_suppress_min_cb(void)
{
    return (uint32_t)(2UL * SOC_POWMAN_MS_PER_TICK);
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
    if (soc_powman_ready_get())
    {
        /* Read before anything else: this is where the window starts, and every microsecond after
         * it - the alarm write, the sleep itself - is inside what gets measured. */
        /* Rounded UP: a window must never end before the tick it was asked for.
         *
         * soc_powman_accum_ms_hz is deliberately NOT subtracted here, and getting that wrong is
         * worth a note because it looks like the symmetric thing to do and it is not. The
         * accumulator holds time that has already ELAPSED and merely has not been announced yet - it
         * sits behind the reference this window is about to take. The kernel's deadline is `ticks`
         * from NOW. Netting the accumulator off makes the window end that much before the deadline,
         * while the close adds the same amount back into `elapsed` - so the kernel is told a full
         * window passed when it did not, the clock runs fast, and the next window is planned shorter
         * still. On an STM32 that showed up as a 50-tick sleep measuring 5. */
        uint64_t wanted_ms = (((uint64_t)ticks * 1000ULL) + (uint64_t)OS_CONFIG_TICK_HZ - 1ULL) /
                             (uint64_t)OS_CONFIG_TICK_HZ;

        soc_powman_entry_ms = powman_timer_get_ms();

        powman_clear_alarm();
        irq_set_enabled(POWMAN_IRQ_TIMER, true);
        powman_timer_enable_alarm_at_ms(soc_powman_entry_ms + wanted_ms);

        /* Last, and after a barrier: from here a peer's peek may read the stamp above. */
        OS_ARCH_DMB();
        soc_powman_window_armed = true;
    }
}

/******************************************************************************************************/
/**
 * @brief Close the window and report the whole tick periods that really elapsed.
 *
 * Measured from the timer rather than from whether the alarm fired: an interrupt of the
 * application's own can end the window early, and that is ordinary rather than exceptional.
 *
 * @return uint32_t  Whole tick periods since os_arch_tick_suppress_cb().
 */
uint32_t os_arch_tick_resume_cb(void)
{
    uint64_t elapsed_ms;
    uint32_t elapsed_ticks;

    /* Disarmed first: a window that ran its length leaves the alarm fired, and one cut short
     * leaves it armed for a moment nobody will wait for. Either way it must not survive into the
     * next window. */
    /* First: a peek from here on answers 0 and the caller falls back to the announced tick, which
     * is behind but never ahead. Nothing below is safe to read from another core. */
    soc_powman_window_armed = false;
    OS_ARCH_DMB();

    powman_timer_disable_alarm();
    powman_clear_alarm();
    irq_set_enabled(POWMAN_IRQ_TIMER, false);

    elapsed_ms = powman_timer_get_ms() - soc_powman_entry_ms;

    /* Banked first, then spent. The whole ticks come out; the fraction of a tick left over stays in
     * the accumulator for the next window rather than being thrown away - see
     * soc_powman_accum_ms_hz. */
    soc_powman_accum_ms_hz += elapsed_ms * (uint64_t)OS_CONFIG_TICK_HZ;

    elapsed_ticks = (uint32_t)(soc_powman_accum_ms_hz / 1000ULL);

    soc_powman_accum_ms_hz -= (uint64_t)elapsed_ticks * 1000ULL;

    return elapsed_ticks;
}

/******************************************************************************************************/
/**
 * @brief How much of the open window has already elapsed, for a core that is not the owner.
 *
 * Deliberately ignores soc_powman_accum_ms_hz, and that is the whole design rather than an
 * oversight. The accumulator is the one piece of state os_arch_tick_resume_cb WRITES, so reading it
 * from another core races the close: a torn 64-bit read would be worth up to a whole tick, and a
 * tick read that comes out too HIGH makes the kernel clock step backwards when the owner finally
 * announces.
 *
 * Dropping it can only under-report - floor(elapsed) instead of floor(carry + elapsed) - so this
 * never exceeds what the announce will produce. The clock may lag by at most one tick during a
 * window and never runs ahead of itself. Everything it does read (the entry stamp, the hardware
 * timer) is fixed for the life of the window.
 *
 * @return uint32_t  Whole tick periods elapsed so far, never more than the close will report.
 */
uint32_t os_arch_tick_elapsed_peek_cb(void)
{
    uint32_t elapsed_ticks = 0U;

    OS_ARCH_DMB();

    if (soc_powman_window_armed)
    {
        uint64_t elapsed_ms = powman_timer_get_ms() - soc_powman_entry_ms;

        elapsed_ticks = (uint32_t)((elapsed_ms * (uint64_t)OS_CONFIG_TICK_HZ) / 1000ULL);
    }

    return elapsed_ticks;
}

#if (SOC_CONFIG_SLEEP_MODE == OS_CONFIG_SLEEP_MODE_DEEP)

#include "soc_sleep.h"

/* The owner alone writes these local bookkeeping values. The kernel pairs prepare/finish
 * outside its global lock, and holds its scheduling mask across the pair. */
static uint32_t soc_sleep_owner_primask = 0U;
static bool soc_sleep_owner_held = false;
#if (OS_CONFIG_TEST_ENABLE == 1U)
/* A debugger can distinguish real PLL-off entries from safe LIGHT fallbacks. */
static __IO uint32_t soc_sleep_deep_entries = 0U;
#endif

#if (OS_CONFIG_CORE_COUNT > 1U)
/* Each shared word has exactly one writer. Publication and observation are ordered with DMB;
 * no kernel API, SDK lock or hardware spinlock is used while either core is parked. Generation
 * zero means released. A new request is never issued until the old acknowledgement is cleared,
 * so even generation wrap cannot let a late acknowledgement authorize another sleep. */
static __IO uint32_t soc_sleep_request = 0U;       /* core 0 writes */
static __IO uint32_t soc_sleep_ack = 0U;           /* core 1 writes */
static __IO uint32_t soc_sleep_abort = 0U;         /* core 1 writes */
static __IO uint32_t soc_sleep_peer_idle = 0U;     /* core 1 writes, advisory only */
static uint32_t soc_sleep_generation = 0U;

/* An idle peer normally acknowledges in a few microseconds. Never wait indefinitely for a
 * preempted idle callback or a core which is busy. The TIMER source runs normally during this
 * rendezvous; no clock has been changed and the tickless window has not yet opened. */
#define SOC_SLEEP_RENDEZVOUS_US 100U
#endif

/******************************************************************************************************/
/**
 * @brief Additional board veto for protocols whose clock requirements registers cannot reveal.
 *
 * Override strongly for external/polled activity. Called with configurable interrupts masked
 * on both participating cores. It must only inspect board state: no waits or kernel API calls.
 */
OS_WEAK bool soc_deep_sleep_allowed_cb(void)
{
    return true;
}

/******************************************************************************************************/
/** @brief Pending work on this core, including scheduler exceptions. Never clears a source. */
static bool soc_sleep_work_pending(void)
{
    uint32_t exceptions = scb_hw->icsr;
    bool pending = (exceptions & (M33_ICSR_PENDSVSET_BITS | M33_ICSR_PENDSTSET_BITS |
                                  M33_ICSR_PENDNMISET_BITS)) != 0U;

    pending = pending || ((nvic_hw->ispr[0] & nvic_hw->iser[0]) != 0U);
    pending = pending || ((nvic_hw->ispr[1] & nvic_hw->iser[1]) != 0U);
    return pending;
}

/******************************************************************************************************/
/** @brief Restore precisely the PRIMASK received by this module, leaving BASEPRI untouched. */
static void soc_sleep_primask_restore(uint32_t primask)
{
    OS_ARCH_DSB();
    __asm volatile("msr primask, %0" :: "r"(primask) : "memory");
    OS_ARCH_ISB();
}

/******************************************************************************************************/
/** @brief Cancel/release the peer before returning the owner's saved interrupt state. */
static void soc_sleep_release(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    OS_ARCH_DMB();
    soc_sleep_request = 0U;
    OS_ARCH_DSB();
    OS_ARCH_SEV();
#endif
    soc_sleep_owner_held = false;
    soc_sleep_primask_restore(soc_sleep_owner_primask);
}

/******************************************************************************************************/
/**
 * @brief Freeze both cores before the kernel freezes its shared time base.
 *
 * A secondary core must be allowed to finish an earlier kernel operation and reach idle before
 * acknowledgement. Waiting after the kernel opens its time window would instead prevent that
 * operation from completing. The owner holds no global lock here, even during cancellation.
 */
bool os_arch_soc_sleep_prepare_cb(void)
{
    bool ready = false;
    bool proceed;

    soc_sleep_owner_primask = os_arch_primask_get();
    OS_ARCH_IRQ_DISABLE();
    OS_ARCH_DSB();
    OS_ARCH_ISB();

    proceed = !soc_sleep_work_pending();
    if (proceed)
    {
#if (OS_CONFIG_CORE_COUNT > 1U)
        /* The hint avoids polling on every tick while the other core is busy. It is never proof
         * of idleness: the generation-matched acknowledgement below is the only permission. */
        OS_ARCH_DMB();
        if ((soc_sleep_peer_idle != 0U) && (soc_sleep_ack == 0U))
        {
            uint64_t started = time_us_64();
            uint32_t generation = soc_sleep_generation + 1U;

            if (generation == 0U)
            {
                generation = 1U;
            }
            soc_sleep_generation = generation;
            OS_ARCH_DMB();
            soc_sleep_request = generation;
            OS_ARCH_DSB();
            OS_ARCH_SEV();

            while ((soc_sleep_ack != generation) &&
                   (soc_sleep_abort != generation) &&
                   !soc_sleep_work_pending() &&
                   ((time_us_64() - started) < SOC_SLEEP_RENDEZVOUS_US))
            {
                OS_ARCH_DMB();
            }

            OS_ARCH_DMB();
            ready = (soc_sleep_ack == generation) && (soc_sleep_abort != generation) &&
                    !soc_sleep_work_pending();
        }
#else
        ready = true;
#endif
    }

    if (ready)
    {
        /* Interrupt handlers on either core cannot start new peripheral work after this check.
         * Autonomous/external protocols still require the board veto described above. */
        ready = soc_deep_peripherals_ready() && soc_deep_sleep_allowed_cb();
    }

    if (ready)
    {
        soc_sleep_owner_held = true;
    }
    else
    {
        soc_sleep_release();
    }
    /* A busy peer/peripheral prevents shared-clock shutdown, not ordinary
     * tickless sleep on core 0. The sleep hook uses WFI with clocks unchanged
     * when preparation did not retain the peer. Pending local work declines
     * this pass instead. */
    return proceed;
}

/******************************************************************************************************/
/** @brief Called only after hardware, elapsed ticks and the kernel window have been restored. */
void os_arch_soc_sleep_finish_cb(void)
{
    if (soc_sleep_owner_held)
    {
        soc_sleep_release();
    }
}

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Cooperatively park core 1 from idle; an IRQ requests wake but cannot run an ISR yet.
 *
 * Save/disable SysTick without clearing a pending tick or changing its reload/current value.
 * Core 0 owns elapsed-time accounting; core 1 resumes its local scheduling cadence on release.
 * SEVONPEND catches an IRQ arriving between the pending check and WFE. The releasing SEV is also
 * latched, closing the release-versus-WFE race. No IRQ or scheduler state is consumed here.
 */
static void soc_sleep_peer_park(void)
{
    uint32_t primask = os_arch_primask_get();
    uint32_t generation;

    OS_ARCH_IRQ_DISABLE();
    OS_ARCH_DSB();
    OS_ARCH_ISB();
    OS_ARCH_DMB();
    generation = soc_sleep_request;

    if ((generation != 0U) && os_task_current_is_idle() && !soc_sleep_work_pending())
    {
        uint32_t systick = OS_ARCH_REG_SYST_CSR;
        uint32_t scr = scb_hw->scr;
        bool wake_sent = false;

        OS_ARCH_REG_SYST_CSR = systick & ~(OS_ARCH_SYST_CSR_ENABLE_MSK |
                                         OS_ARCH_SYST_CSR_TICKINT_MSK);
        scb_hw->scr = scr | M33_SCR_SEVONPEND_BITS | M33_SCR_SLEEPDEEP_BITS;
        OS_ARCH_DSB();

        /* SysTick may have pended between the first test and disabling its counter. Preserve
         * that work and reject this request instead of clearing it to manufacture an idle core. */
        if (!soc_sleep_work_pending() && (soc_sleep_request == generation))
        {
            OS_ARCH_DMB();
            soc_sleep_ack = generation;
            OS_ARCH_DSB();
            OS_ARCH_SEV();

            while (soc_sleep_request == generation)
            {
                OS_ARCH_DMB();
                if (!wake_sent && soc_sleep_work_pending())
                {
                    soc_sleep_abort = generation;
                    OS_ARCH_DSB();
                    /* A real enabled IPI wakes core 0's WFI even with PRIMASK raised. SEV alone
                     * would only wake WFE, and would leave the owner asleep until its timer. */
                    os_arch_core_ipi_request_cb(0U);
                    wake_sent = true;
                }
                OS_ARCH_DSB();
                OS_ARCH_WFE();
            }
        }
        else
        {
            soc_sleep_abort = generation;
            OS_ARCH_DSB();
            OS_ARCH_SEV();
        }

        scb_hw->scr = scr;
        OS_ARCH_REG_SYST_CSR = systick;
        OS_ARCH_DSB();
        /* Publish zero only after this generation's peripheral state is whole again. Core 0
         * declines another request while a cancelled/finished acknowledgement remains visible. */
        soc_sleep_ack = 0U;
        OS_ARCH_DMB();
    }
    soc_sleep_primask_restore(primask);
}
#endif

/******************************************************************************************************/
/**
 * @brief The sleep itself: drop off the PLL, stop it, and halt the core until POWMAN wakes it.
 *
 * The kernel's own OS_ARCH_SLEEP would do a plain WFI here, which halts the core and leaves every
 * clock generator running. On this part that is nearly all of the idle power, and the PLL is the
 * bulk of it.
 *
 * WHAT THIS DOES NOT DO, and why. The deepest route is to move clk_ref onto LPOSC as well and stop
 * the crystal, which saves more again - and it is also the one where a mistake leaves the core with
 * no clock to execute the restore from, recoverable only through BOOTSEL. clk_ref is deliberately
 * left where it was: the core is running from a known-good oscillator the whole way through, so the
 * restore below cannot fail to have a clock. What is given up is the last slice; what is bought is
 * a sleep that always comes back.
 *
 * An earlier attempt did less than this and said so: zeroing SLEEP_EN0/1 with SLEEPDEEP set gates
 * peripheral clocks but not the generator the core runs from, and the self-test's cycle counter
 * went on counting straight through the sleep - 24 million cycles over 20 windows, exactly a plain
 * WFI. That is why the PLL is stopped here rather than merely asked to idle.
 *
 * The PLL is restored from its own registers rather than recomputed. Reading back what was there
 * and writing it again cannot disagree with the clock tree the application configured, where a
 * recomputed VCO and post-divider pair very easily could.
 *
 * @return None.
 */
void os_arch_soc_sleep_cb(void)
{
    /* The kernel's pre-sleep board callback ran since prepare. Recheck autonomous peripheral
     * activity and incoming work immediately before touching a shared clock. A rejected deep
     * entry still closes the already-armed tickless window through its normal wake path. */
    bool ready = soc_sleep_owner_held && !soc_sleep_work_pending();

#if (OS_CONFIG_CORE_COUNT > 1U)
    OS_ARCH_DMB();
    ready = ready && (soc_sleep_ack == soc_sleep_request) && (soc_sleep_request != 0U) &&
            (soc_sleep_abort != soc_sleep_request);
#endif
    ready = ready && soc_deep_peripherals_ready() && soc_deep_sleep_allowed_cb();
    if (!ready)
    {
        __wfi();
        return;
    }

    uint32_t pll_cs   = pll_sys_hw->cs;
    uint32_t pll_fb   = pll_sys_hw->fbdiv_int;
    uint32_t pll_prim = pll_sys_hw->prim;
    uint32_t pll_pwr  = pll_sys_hw->pwr;
    uint32_t sys_ctrl = clocks_hw->clk[clk_sys].ctrl;
    uint32_t sys_div  = clocks_hw->clk[clk_sys].div;
    uint32_t sys_selected = clocks_hw->clk[clk_sys].selected;
    uint32_t scr = scb_hw->scr;

    /* Off the PLL first, and glitchlessly: clk_sys back to clk_ref, which is still running from
     * whatever the application put it on. Only once nothing is fed from the PLL may it be stopped -
     * pulling it out from under a running clk_sys stops the core where it stands. */
    clocks_hw->clk[clk_sys].ctrl = sys_ctrl & ~CLOCKS_CLK_SYS_CTRL_SRC_BITS;

    while ((clocks_hw->clk[clk_sys].selected & 1U) == 0U)
    {
    }

    pll_sys_hw->pwr = PLL_PWR_BITS;   /* every block powered down */

    scb_hw->scr = scr | M33_SCR_SLEEPDEEP_BITS;

#if (OS_CONFIG_TEST_ENABLE == 1U)
    soc_sleep_deep_entries++;
#endif
    OS_ARCH_DSB();
    __wfi();
    OS_ARCH_ISB();

    scb_hw->scr = scr;

    /* Back up in the order it came down: the PLL has to be locked before anything is fed from it. */
    pll_sys_hw->cs        = pll_cs;
    pll_sys_hw->fbdiv_int = pll_fb;
    pll_sys_hw->prim      = pll_prim;
    pll_sys_hw->pwr       = pll_pwr | PLL_PWR_POSTDIVPD_BITS;

    while ((pll_sys_hw->cs & PLL_CS_LOCK_BITS) == 0U)
    {
    }

    pll_sys_hw->pwr = pll_pwr;

    clocks_hw->clk[clk_sys].div  = sys_div;
    clocks_hw->clk[clk_sys].ctrl = sys_ctrl;
    while (clocks_hw->clk[clk_sys].selected != sys_selected)
    {
    }
    OS_ARCH_DSB();
    OS_ARCH_ISB();
}

#endif /* SOC_CONFIG_SLEEP_MODE_DEEP */

#endif /* OS_CONFIG_TICKLESS_ENABLE */

/******************************************************************************************************/
/** @brief Package idle override: normal WFE, with cooperative DEEP parking on the second core. */
void os_arch_soc_idle_cb(void)
{
#if (OS_CONFIG_TICKLESS_ENABLE == 1U) && \
    (SOC_CONFIG_SLEEP_MODE == OS_CONFIG_SLEEP_MODE_DEEP) && (OS_CONFIG_CORE_COUNT > 1U)
    if ((get_core_num() == 1U) && os_task_current_is_idle())
    {
        soc_sleep_peer_idle = 1U;
        OS_ARCH_DMB();
        soc_sleep_peer_park();
        OS_ARCH_WFE();
        soc_sleep_peer_park();
        soc_sleep_peer_idle = 0U;
        OS_ARCH_DMB();
    }
    else
#endif
    {
        OS_ARCH_WFE();
    }
}

#if (OS_CONFIG_CORE_COUNT > 1U)

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Checked, not defaulted, for the same reason as every option in soc_common.h: this one decides
 * which piece of hardware carries a scheduling nudge between cores, and an invented answer is one
 * nobody chose. Doorbells are RP2350-only, so it lives in this package rather than the shared
 * header. */
#if !defined(SOC_CONFIG_IPI_DOORBELL)
#error "soc_config.h is incomplete: SOC_CONFIG_IPI_DOORBELL is required by the raspberrypi/rp235x_arm package."
#endif

/* Sent through the FIFO when soc_config.h opts out of doorbells. Never read - the receiving core
 * drains the FIFO and pends PendSV whatever arrived - but a recognisable constant is worth more
 * than a zero when it turns up in a trace. */
#define SOC_IPI_TOKEN           0xA1U

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

#if (SOC_CONFIG_IPI_DOORBELL != 0U)
/* Claimed in soc_ipi_arm() on core 0, then read by core 1. */
static uint soc_doorbell = 0U;
#endif

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void soc_ipi_handler(void);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Interrupt another core so it re-evaluates which task should be running.
 *
 * Without this a core notices a newly ready task only at its next tick, which is correct but adds
 * up to a whole tick of latency to every cross-core wake.
 */
OS_WEAK void os_arch_core_ipi_request_cb(uint32_t core_id)
{
    /* Broadcast an event first, and unconditionally. The interrupt below is what makes the
     * other core RESCHEDULE; this is what makes sure it is awake to notice. An idle core
     * here sits in WFE (see os_arch_soc_idle_cb), and the event register latches - so this
     * lands even if it arrives before that core reaches the instruction. Costs one cycle on
     * a path that is already doing cross-core work. */
    OS_ARCH_SEV();

    /* Nudging the core already executing this call needs no interrupt at all - and on a two-core
     * chip "the other core" is what the primitives below actually address, so passing our own id
     * through them would signal the wrong core. */
    if (core_id == (uint32_t)get_core_num())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }
    else
    {
#if (SOC_CONFIG_IPI_DOORBELL != 0U)
        multicore_doorbell_set_other_core(soc_doorbell);
#else
        /* Non-blocking on purpose. A full FIFO already means an unhandled signal is waiting at
         * the other core, which is the same result this call wants, and blocking here would
         * stall a scheduler path with interrupts masked. */
        (void)multicore_fifo_push_timeout_us(SOC_IPI_TOKEN, 0);
#endif
    }
}

/******************************************************************************************************/
/**
 * @brief Enable the inter-core interrupt on the calling core. Runs once per core.
 */
void soc_ipi_arm(void)
{
#if (SOC_CONFIG_IPI_DOORBELL != 0U)
    /* One doorbell serves both directions; claim it once, on core 0, then read it on core 1. */
    if (get_core_num() == 0U)
    {
        soc_doorbell = (uint)multicore_doorbell_claim_unused((1u << 0) | (1u << 1), true);
    }

    uint irq = multicore_doorbell_irq_num(soc_doorbell);
#else
    /* The RP2350 banks one FIFO IRQ number per core, unlike the RP2040's pair. */
    uint irq = (uint)SIO_IRQ_FIFO;
#endif

    irq_set_exclusive_handler(irq, soc_ipi_handler);

    /* Lowest priority, matching what the port gives SysTick. Anything higher would let a
     * scheduling nudge preempt the application's own interrupts, and would also put the handler
     * above OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY, where the kernel's mask can no longer reach it. */
    irq_set_priority(irq, PICO_LOWEST_IRQ_PRIORITY);
    irq_set_enabled(irq, true);
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Inter-core interrupt handler: clear the signal and ask for a reschedule.
 *
 * The signal carries no information beyond "look again", so nothing is decoded. Pending PendSV
 * rather than switching here is what keeps the context switch in the one place able to do it.
 */
static void soc_ipi_handler(void)
{
#if (SOC_CONFIG_IPI_DOORBELL != 0U)
    multicore_doorbell_clear_current_core(soc_doorbell);
#else
    multicore_fifo_clear_irq();
    multicore_fifo_drain();
#endif

    OS_ARCH_CONTEXT_SWITCH_REQUEST();
}

#endif /* OS_CONFIG_CORE_COUNT > 1U */
