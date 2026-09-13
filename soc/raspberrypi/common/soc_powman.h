/**
 * @file soc_powman.h
 * @brief RP235x tickless wake source: the POWMAN always-on timer.
 *
 * Shared by both RP235x packages, and nothing in it is architecture-specific - POWMAN is a chip
 * peripheral and the same silicon carries it either way. Included by a package's own soc_cb.c
 * rather than compiled on its own, so a build that does not use this source carries none of it.
 *
 * The Arm package takes its windows from here whatever the sleep depth: SysTick and the TIMER
 * blocks stop when the clocks do, and this one does not. The RISC-V package uses mtime while the
 * clocks are up and only reaches for this under DEEP, where mtime - clocked from clk_sys - stops
 * with the PLL.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef SOC_RPI_POWMAN_H
#define SOC_RPI_POWMAN_H

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include <stdbool.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/powman.h"

#ifdef __cplusplus
extern "C" {
#endif

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
static uint32_t soc_powman_ceiling_ticks(void)
{
    return soc_powman_ready_get() ? UINT32_MAX : 0U;
}

/******************************************************************************************************/
/**
 * @brief The shortest window worth sleeping through.
 *
 * A millisecond is this timer's whole resolution, so a window has to span at least two of them
 * before its length can be told from rounding. That is two milliseconds converted TO ticks,
 * rounded up so a slow tick never asks for a floor of zero: at 1 kHz it comes to the two ticks the
 * kernel insists on anyway, and below 1 kHz a single tick already covers the two milliseconds.
 *
 * @return uint32_t  Floor on one window, in ticks.
 */
static uint32_t soc_powman_floor_ticks(void)
{
    return (uint32_t)(((2UL * OS_CONFIG_TICK_HZ) + 999UL) / 1000UL);
}

/******************************************************************************************************/
/**
 * @brief Open a window of `ticks` tick periods.
 *
 * @param[in] ticks  Tick periods to sleep.
 * @return None.
 */
static void soc_powman_window_open(uint32_t ticks)
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
static uint32_t soc_powman_window_close(void)
{
    uint64_t elapsed_ms;
    uint32_t elapsed_ticks;

    /* Disarmed first: a window that ran its length leaves the alarm fired, and one cut short
     * leaves it armed for a moment nobody will wait for. Either way it must not survive into the
     * next window. */
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

#ifdef __cplusplus
}
#endif

#endif /* SOC_RPI_POWMAN_H */
