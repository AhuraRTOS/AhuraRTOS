/**
 * @file soc_cb.c
 * @brief RP2040-specific SoC code: the inter-core interrupt, carried on the SIO FIFO.
 *
 * Everything else the kernel needs from this chip is in ../common/soc_common.c, which is
 * compiled into this package: the core id, the spinlock, the CPU clock, the SysTick vector and
 * booting core 1 are all identical on the RP2040 and the RP2350, because they are SIO or plain
 * SDK either way.
 *
 * What is here is the one genuine difference. The RP2040 has no doorbells, so a core signals the
 * other by pushing a word into the inter-core FIFO and taking that FIFO's IRQ at the far end. The
 * RP2350 package does the same job with a claimed doorbell.
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
#include "hardware/timer.h"
#include "pico/multicore.h"

#if (OS_CONFIG_CORE_COUNT > 1U)

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Sent through the FIFO to mean "re-evaluate scheduling". The value is never read - the receiving
 * core drains the FIFO and pends PendSV whatever arrived - but a recognisable constant is worth
 * more than a zero when it turns up in a trace. */
#define SOC_IPI_TOKEN           0xA1U

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
     * chip "the other core" is what the FIFO actually addresses, so passing our own id through it
     * would signal the wrong core. */
    if (core_id == (uint32_t)get_core_num())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }
    else
    {
        /* Non-blocking on purpose. A full FIFO already means an unhandled signal is waiting at
         * the other core, which is the same result this call wants, and blocking here would
         * stall a scheduler path with interrupts masked. */
        (void)multicore_fifo_push_timeout_us(SOC_IPI_TOKEN, 0);
    }
}

/******************************************************************************************************/
/**
 * @brief Enable the inter-core interrupt on the calling core. Runs once per core.
 */
void soc_ipi_arm(void)
{
    /* Each core has its own SIO FIFO IRQ, numbered from SIO_IRQ_PROC0. */
    uint irq = (uint)SIO_IRQ_PROC0 + get_core_num();

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
 * @brief Inter-core interrupt handler: drain the FIFO and ask for a reschedule.
 *
 * The signal carries no information beyond "look again", so nothing is decoded. Pending PendSV
 * rather than switching here is what keeps the context switch in the one place able to do it.
 */
static void soc_ipi_handler(void)
{
    multicore_fifo_clear_irq();
    multicore_fifo_drain();

    OS_ARCH_CONTEXT_SWITCH_REQUEST();
}

#endif /* OS_CONFIG_CORE_COUNT > 1U */

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Tickless idle
 * ***********************************************************************************************************
 *
 * The Arm port cannot suppress its own tick here: reprogramming SysTick's reload would strand
 * os_arch_cycle_systick.c, which is the ONLY cycle counter an ARMv6-M core has and the one
 * os_delay_us() runs on. So the port masks the tick interrupt instead and asks this package to
 * wake it, which needs a timer SysTick's silence cannot affect.
 *
 * The always-on microsecond timer is that: 64 bits at a fixed 1 MHz, independent of clk_sys and of
 * anything the core does to SysTick. One of its four alarms is claimed for the window.
 *
 * Whole ticks only, both ways. The kernel counts in ticks and announcing a partial one would move
 * os_tick_count somewhere the tick grid never was.
*/

/** Microseconds in one kernel tick, settled once so neither callback divides at run time. */
#define SOC_TICKLESS_US_PER_TICK    (1000000UL / OS_CONFIG_TICK_HZ)

/** Alarm the window uses, claimed from the SDK on first use rather than chosen here. A hard-coded
 *  number is what broke: pico_time's default pool already owns one, and the application may own
 *  others. -1 until claimed. */
static int32_t soc_tickless_alarm = -1;

/** Timer reading when the window opened, so the close can measure against it. */
static uint64_t soc_tickless_entry_us;

/******************************************************************************************************/
/**
 * @brief Alarm handler: exists only to end the WFI.
 *
 * Nothing is done here on purpose. The wake itself is the whole product, and the kernel measures
 * the window from the timer rather than from anything this could record - a handler that ran late,
 * or not at all because something else woke the core first, must not change the answer.
 *
 * @return None.
 */
static void soc_tickless_alarm_isr(void)
{
    hw_clear_bits(&timer_hw->intr, 1UL << (uint32_t)soc_tickless_alarm);
}

/******************************************************************************************************/
/**
 * @brief Take an alarm and its vector from the SDK, once.
 *
 * Deferred to first use rather than done at SoC init, so a build without tickless idle costs no
 * alarm at all - this part has four and the application may want them.
 *
 * The claim goes through the SDK's allocator on purpose. It is what tells pico_time and any driver
 * that asks later that this alarm is spoken for, and asking for a specific number instead is what
 * put the kernel's handler on top of pico_time's.
 *
 * @return bool  True once an alarm is owned; false when all four are already taken, which leaves
 *                the port suppressing nothing rather than fighting for one.
 */
static bool soc_tickless_alarm_ready(void)
{
    if (soc_tickless_alarm < 0)
    {
        int32_t claimed = (int32_t)hardware_alarm_claim_unused(false);

        if (claimed >= 0)
        {
            soc_tickless_alarm = claimed;

            /* Exclusive is right here and safe now: the allocator just handed this one over, so
             * nothing else holds its vector. */
            irq_set_exclusive_handler((uint)(TIMER_IRQ_0 + claimed), soc_tickless_alarm_isr);
        }
    }

    return (soc_tickless_alarm >= 0);
}

/******************************************************************************************************/
/**
 * @brief How many ticks one window may skip.
 *
 * The alarm compares the low 32 bits of a 1 MHz counter, so a window cannot reach the wrap at
 * roughly 71 minutes. Capping well below that leaves the comparison unambiguous and is far longer
 * than any deadline the kernel will actually offer.
 *
 * @return uint32_t  Ceiling in ticks.
 */
uint32_t os_arch_tick_suppress_max_cb(void)
{
    uint32_t max_ticks = 0U;

    /* Also where the alarm gets claimed, which is why this is the callback the port asks first: a
     * package with no alarm to spare answers 0 and the port stops there. */
    if (soc_tickless_alarm_ready())
    {
        max_ticks = (60UL * 1000000UL) / SOC_TICKLESS_US_PER_TICK;   /* one minute */
    }

    return max_ticks;
}

/******************************************************************************************************/
/**
 * @brief Arm the alarm to wake this core in `ticks` tick periods.
 *
 * @param[in] ticks  Tick periods to sleep.
 * @return None.
 */
void os_arch_tick_suppress_cb(uint32_t ticks)
{
    /* Guarded rather than assumed: the port only gets here after os_arch_tick_suppress_max_cb
     * answered non-zero, which is what claims the alarm, but a caller that skipped that order would
     * otherwise index the alarm array with -1. */
    if (soc_tickless_alarm_ready())
    {
        uint32_t alarm  = (uint32_t)soc_tickless_alarm;
        uint64_t target;

        soc_tickless_entry_us = timer_time_us_64(timer_hw);
        target                = soc_tickless_entry_us + ((uint64_t)ticks * SOC_TICKLESS_US_PER_TICK);

        hw_set_bits(&timer_hw->inte, 1UL << alarm);
        irq_set_enabled((uint)(TIMER_IRQ_0 + alarm), true);

        /* Writing the alarm arms it against the low 32 bits, which is why the ceiling above keeps
         * the window short of the wrap. */
        timer_hw->alarm[alarm] = (uint32_t)target;
    }
}

/******************************************************************************************************/
/**
 * @brief Disarm the alarm and report the whole tick periods that actually elapsed.
 *
 * Measured from the timer rather than from whether the alarm fired: an interrupt of the
 * application's own can end the window early, and that is ordinary rather than exceptional.
 *
 * @return uint32_t  Whole tick periods since os_arch_tick_suppress_cb.
 */
uint32_t os_arch_tick_resume_cb(void)
{
    uint64_t elapsed_us;

    /* Disarm first: a window that ran to completion leaves the alarm fired and its bit set, and a
     * window cut short leaves it armed for a moment that will never be waited for. */
    hw_clear_bits(&timer_hw->inte, 1UL << (uint32_t)soc_tickless_alarm);
    hw_clear_bits(&timer_hw->intr, 1UL << (uint32_t)soc_tickless_alarm);
    irq_set_enabled((uint)(TIMER_IRQ_0 + soc_tickless_alarm), false);

    elapsed_us = timer_time_us_64(timer_hw) - soc_tickless_entry_us;

    return (uint32_t)(elapsed_us / SOC_TICKLESS_US_PER_TICK);
}

#endif /* OS_CONFIG_TICKLESS_ENABLE */
