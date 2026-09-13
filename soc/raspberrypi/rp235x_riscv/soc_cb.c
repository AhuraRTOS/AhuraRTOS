/**
 * @file soc_cb.c
 * @brief SoC-owned callbacks for the RP2350 / RP2354 running their Hazard3 RISC-V cores.
 *
 * The RISC-V sibling of ../rp235x_arm: same silicon, different core, almost every answer
 * different - which is why it is a separate package rather than an #if inside that one.
 *
 * PendSV's replacement is SIO_RISCV_SOFTIRQ, which drives mip.MSIP and carries a set and a clear
 * bit PER CORE. So "reschedule me" and "reschedule the other core" are the same write with a
 * different bit, and both callbacks are one line. The datasheet settles the race too: set beats
 * clear on the same cycle, so a request arriving while the handler acknowledges is never lost.
 *
 * The tick comes in as external IRQ 29, not mip.MTIP, and that is not a preference: os_arch_in_isr()
 * reads Hazard3's meicontext, which knows about external IRQs and nothing about MTIP. A tick on
 * cause 7 would run with the kernel believing it was in task context.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#include "ahura.h"
#include "os_arch_port.h"
#include "soc_config.h"

#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/riscv_platform_timer.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/time.h"

/* SIO_RISCV_SOFTIRQ bit positions, from the RP2350 datasheet. Named here rather than taken from the
 * SDK's regs header so the intent is readable at the point of use. */
#define SOC_SOFTIRQ_SET(core)   (1UL << (core))
#define SOC_SOFTIRQ_CLR(core)   (1UL << ((core) + 8U))

#if (OS_CONFIG_CORE_COUNT > 1U)
static void soc_core1_entry(void);

/* Set by core 1 as its first act, read by os_arch_soc_diagnose_cb() on core 0. A flag rather than
 * a printf: the SDK's stdio takes a mutex that core 0 holds almost continuously while producing
 * output, so printing from core 1 would not report progress - it would block the core being
 * diagnosed. 0xFF means it never arrived. */
static __IO uint8_t soc_core_reached = 0xFFU;
#endif

/*
 * ***********************************************************************************************************
 * The context-switch request
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Raise this core's machine software interrupt: the context-switch request.
 */
void os_arch_swi_request_cb(void)
{
    sio_hw->riscv_softirq = SOC_SOFTIRQ_SET(get_core_num());
}

/******************************************************************************************************/
/**
 * @brief Acknowledge this core's machine software interrupt.
 *
 * mip.MSIP is read-only to software and follows this register, so without this write the trap would
 * re-enter the moment it returned, forever.
 */
void os_arch_swi_clear_cb(void)
{
    sio_hw->riscv_softirq = SOC_SOFTIRQ_CLR(get_core_num());
}

#if (OS_CONFIG_CORE_COUNT > 1U)

/******************************************************************************************************/
/**
 * @brief Interrupt another core so it re-evaluates scheduling.
 *
 * The same register as os_arch_swi_request_cb(), aimed at a different core. On the Arm side this
 * needs a doorbell and its own IRQ; here the mechanism the kernel already uses to reschedule itself
 * happens to be per-core addressable, so there is nothing further to claim or arm.
 *
 * @param[in] core_id  Core to interrupt.
 */
void os_arch_core_ipi_request_cb(uint32_t core_id)
{
    sio_hw->riscv_softirq = SOC_SOFTIRQ_SET(core_id);
}

/******************************************************************************************************/
/**
 * @brief This core's index, from SIO's CPUID.
 */
uint32_t os_arch_core_id_get_cb(void)
{
    return (uint32_t)get_core_num();
}

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Check that mhartid really is this core's index, once, at boot.
 *
 * soc.cmake tells the port that it is (OS_CONFIG_ARCH_CORE_ID_MHARTID), which turns every core-id
 * read in the kernel into a single inline CSR instruction. That claim is worth one compare per core
 * to stand behind: were it ever wrong, every per-core structure the kernel owns - the critical
 * nesting counts, the saved masks, os_task_current[], the idle tasks - would be indexed with the
 * wrong core's number, and nothing downstream could notice. It is the same bargain
 * OS_CONFIG_ARCH_VECTOR_CHECK makes, and it gets the same answer: park where a debugger lands on
 * the cause instead of running on wrong.
 *
 * Unconditional rather than an OS_ASSERT. A build with assertions compiled out is exactly the one
 * that can least afford to be quietly wrong about which core it is running on, and the cost is a
 * CSR read, a load and a branch that happen once.
 */
static void soc_core_id_verify(void)
{
    if (OS_ARCH_CSR_READ(mhartid) != (uint32_t)get_core_num())
    {
        os_arch_config_fault_trap();
    }
}
#endif

/******************************************************************************************************/
/**
 * @brief Release the secondary core into the kernel.
 *
 * @param[in] core_id  Core to start; only core 1 can be started on this chip.
 */
void os_arch_core_launch_cb(uint32_t core_id)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    if (core_id == 1U)
    {
        multicore_launch_core1(soc_core1_entry);
    }
#else
    (void)core_id;
#endif
}

/* One trap stack per secondary core. Core 0 keeps the stack the linker script gave it; every other
 * core needs one the port can point at, because nothing in the SDK reserves one for a core the
 * kernel started itself. */
static uint8_t soc_handler_stack[OS_CONFIG_CORE_COUNT - 1U][SOC_CONFIG_HANDLER_STACK_SIZE]
    __attribute__((aligned(16)));

/******************************************************************************************************/
/**
 * @brief Top of the trap stack for the given core.
 */
uint32_t os_arch_handler_stack_top_cb(uint32_t core_id)
{
    uint32_t top = 0U;                              /* core 0 keeps the linker's stack */

    if (core_id != 0U)
    {
        top = (uint32_t)(uintptr_t)&soc_handler_stack[core_id - 1U][SOC_CONFIG_HANDLER_STACK_SIZE];
    }

    return top;
}

/******************************************************************************************************/
/**
 * @brief Limit of the trap stack for the given core.
 */
uint32_t os_arch_handler_stack_limit_cb(uint32_t core_id)
{
    uint32_t limit = 0U;                            /* core 0 keeps the linker's stack */

    if (core_id != 0U)
    {
        limit = (uint32_t)(uintptr_t)&soc_handler_stack[core_id - 1U][0];
    }

    return limit;
}

/*
 * ***********************************************************************************************************
 * Spinlock
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Take the kernel's cross-core lock.
 *
 * Routed through the SDK's spin_lock API rather than the port's own lr.w/sc.w for the same reason
 * the Arm package does it: the SDK carries the errata workarounds for these locks, and going
 * through it means the kernel inherits them instead of keeping its own copy.
 *
 * @param[in,out] lock  Unused; the hardware lock is the one named by SOC_CONFIG_SPINLOCK_ID.
 */
void os_arch_spinlock_acquire_cb(os_arch_spinlock_t *lock)
{
    (void)lock;

    spin_lock_unsafe_blocking(spin_lock_instance(SOC_CONFIG_SPINLOCK_ID));
}

/******************************************************************************************************/
/**
 * @brief Release the kernel's cross-core lock.
 */
void os_arch_spinlock_release_cb(os_arch_spinlock_t *lock)
{
    (void)lock;

    spin_unlock_unsafe(spin_lock_instance(SOC_CONFIG_SPINLOCK_ID));
}

#endif /* OS_CONFIG_CORE_COUNT > 1U */

/*
 * ***********************************************************************************************************
 * Clock
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief The CPU clock in Hz.
 *
 * Read live rather than cached, so a set_sys_clock_khz() before os_init() comes out right without
 * the application having to tell the kernel about it.
 */
uint32_t os_arch_clock_hz_get(void)
{
    return (uint32_t)clock_get_hz(clk_sys);
}

/*
 * ***********************************************************************************************************
 * Tick
 * ***********************************************************************************************************
*/

/* Counts of mtime per kernel tick, computed once when the tick is programmed. */
static uint32_t soc_tick_interval;

/******************************************************************************************************/
/**
 * @brief Tick vector: advance the kernel clock and re-arm the comparator.
 *
 * mtimecmp is a comparator, not a reload register - the interrupt stays asserted while
 * mtime >= mtimecmp - so pushing it forward is what acknowledges the interrupt. Advancing it by a
 * fixed interval from its PREVIOUS value rather than from the current mtime keeps the tick free of
 * drift: any latency in reaching this handler is absorbed rather than added to the next period.
 */
static void soc_tick_isr(void)
{
    riscv_timer_set_mtimecmp(riscv_timer_get_mtimecmp() + (uint64_t)soc_tick_interval);

    os_tick_handler();
}

/** Referenced by nothing, and that is its entire job.
 *
 *  A static archive gives up an object only while the link still has an undefined symbol it
 *  defines. Nothing here qualifies: every callback this package supplies has a weak default in the
 *  kernel or the port, so the linker has no reason to extract the object and the whole package
 *  loses to those defaults - silently. On this part that costs the mtimecmp tick and the tickless
 *  wake source together.
 *
 *  It stayed hidden here for the same reason it did in soc/st/stm32: os_tick.c references
 *  os_tickless_pre_sleep_cb, the kernel ships no default for it, and that one undefined symbol
 *  dragged the object in by accident on every build that happened to enable tickless.
 *
 *  soc.cmake names this in a -u link option, which is what forces the extraction. Unconditional on
 *  purpose: a symbol behind the same #if as the things it rescues would disappear with them. */
const uint32_t soc_rp235x_riscv_anchor = 0U;

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

/* No wake source to choose: the depth decides it. LIGHT keeps the clocks running, so mtime is
 * still counting and takes the window; DEEP gates them, and only POWMAN would survive that.
 *
 * It used to be five flags plus the mode, with an arithmetic rule saying exactly one flag had to
 * be 1, another naming the sources this chip does not physically have, and a third refusing deep
 * sleep against a source that stops with the clocks. None of those states can be expressed any
 * more, so none of those rules exists.
 *
 * DEEP therefore changes the wake source as well as the depth: it stops the PLL, mtime counts
 * clk_sys and stops with it, so the window is taken by the POWMAN timer in the always-on domain
 * instead - the same source the Arm package uses at either depth, shared from ../common. */
#if (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U)
/* Only the deep path needs these: the POWMAN wake source, and the walk that checks nothing else
 * is mid-transfer before the shared clocks stop. Both are shared with the Arm package and included
 * rather than compiled, so a LIGHT build carries neither. */
#include "soc_powman.h"
#include "soc_sleep.h"
#endif

/*
 * ***********************************************************************************************************
 * Tickless idle
 * ***********************************************************************************************************
 *
 * mtime free-runs at clk_sys and mtimecmp is an absolute deadline, so a suppressed window is one
 * write: put the next interrupt N tick periods out instead of one. The port cannot do it because
 * the privileged spec never says where these registers live; this package can.
*/

#if (OS_CONFIG_TICKLESS_DEEP_ENABLE == 0U)

/** Deadline of the tick that would have fired next, captured when the window opened. */
static uint64_t soc_tickless_base;

/** Ticks the kernel asked to skip, so the report can be clamped to what was actually promised. */
static uint32_t soc_tickless_planned;

/******************************************************************************************************/
/**
 * @brief How many ticks this chip can skip in one window.
 *
 * mtimecmp is 64 bits against a clk_sys-rate mtime, so nothing here runs out before the kernel's
 * own ceiling does: at 150 MHz the counter needs about 3900 years to wrap.
 *
 * @return uint32_t  The kernel's configured ceiling.
 */
uint32_t os_arch_tick_suppress_max_cb(void)
{
    /* An unprogrammed tick has no grid to suppress against.
     *
     * Otherwise no ceiling of this timer's own, and that is not a shrug: mtime and mtimecmp are 64
     * bits, the deadline is absolute, and ticks * soc_tick_interval cannot approach that width for
     * any interval a kernel tick could have. UINT32_MAX is this callback's way of saying "the
     * hardware imposes nothing" - the kernel then applies the limit its own 32-bit tick counter
     * imposes, OS_TICKLESS_MAX_IDLE_TICKS in os_tick.c, which is where that rule belongs. Reporting
     * the kernel's number from here would only duplicate it, and duplicated limits drift apart.
     *
     * The 24-bit answer this used to give came from the config, and was SysTick's limit on a part
     * that has no SysTick. */
    return (soc_tick_interval != 0U) ? UINT32_MAX : 0U;
}

/******************************************************************************************************/
/**
 * @brief Push the tick deadline out so no interrupt arrives for `ticks` tick periods.
 *
 * The interrupt is programmed for tick `ticks`, which sits at base + (ticks - 1) * interval: the
 * deadline already in mtimecmp IS the first of them, so only the remaining ones are added.
 *
 * @param[in] ticks  Tick periods to skip; the kernel guarantees at least the floor
 *                   OS_CONFIG_TICKLESS_MIN_IDLE_MS sets.
 * @return None.
 */
void os_arch_tick_suppress_cb(uint32_t ticks)
{
    if ((soc_tick_interval != 0U) && (ticks > 1U))
    {
        soc_tickless_base    = riscv_timer_get_mtimecmp();
        soc_tickless_planned = ticks;

        riscv_timer_set_mtimecmp(soc_tickless_base +
                                 ((uint64_t)(ticks - 1U) * (uint64_t)soc_tick_interval));
    }
    else
    {
        soc_tickless_planned = 0U;
    }
}

/******************************************************************************************************/
/**
 * @brief Close the window: report the whole ticks that really passed and restore the cadence.
 *
 * The tick whose deadline has already been reached is deliberately NOT counted here. Its interrupt
 * is latched in the SIO controller behind the kernel's mask and the ordinary soc_tick_isr delivers
 * it - advancing mtimecmp by one interval of its own - the moment that mask is released. Counting
 * it in both places would advance the clock twice for one tick.
 *
 * @return uint32_t  Whole ticks elapsed, excluding that pending one.
 */
uint32_t os_arch_tick_resume_cb(void)
{
    uint32_t elapsed = 0U;

    if (soc_tickless_planned != 0U)
    {
        uint64_t now = riscv_timer_get_mtime();

        if (now >= soc_tickless_base)
        {
            /* Boundaries crossed: the one at base, plus one per whole interval since. */
            elapsed = 1U + (uint32_t)((now - soc_tickless_base) / (uint64_t)soc_tick_interval);
        }

        /* One short of the promise at most: the last tick of a fully elapsed window is the one
         * already pending, and a window that woke early cannot have passed more than it planned. */
        if (elapsed > (soc_tickless_planned - 1U))
        {
            elapsed = soc_tickless_planned - 1U;
        }

        /* Back onto the grid, not onto `now`. base + elapsed * interval is the next boundary that
         * has not passed, so the cadence resumes exactly where it would have been had every
         * suppressed tick fired - which is what keeps repeated windows from drifting.
         *
         * Skipped when the window ran to completion: the pending interrupt's own ISR advances
         * mtimecmp by one interval from the deadline it was programmed with, which lands on that
         * same boundary. Writing it here as well would push the cadence one tick further out. */
        if (elapsed < (soc_tickless_planned - 1U))
        {
            riscv_timer_set_mtimecmp(soc_tickless_base +
                                     ((uint64_t)elapsed * (uint64_t)soc_tick_interval));
        }

        soc_tickless_planned = 0U;
    }

    return elapsed;
}

#else /* OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U */

/******************************************************************************************************/
/**
 * @brief How many ticks one window may skip.
 *
 * @return uint32_t  Ceiling in ticks.
 */
uint32_t os_arch_tick_suppress_max_cb(void)
{
    return (soc_tick_interval != 0U) ? soc_powman_ceiling_ticks() : 0U;
}

/******************************************************************************************************/
/**
 * @brief The shortest window worth sleeping through.
 *
 * @return uint32_t  Floor on one window, in ticks.
 */
uint32_t os_arch_tick_suppress_min_cb(void)
{
    return soc_powman_floor_ticks();
}

/******************************************************************************************************/
/**
 * @brief Open a window of `ticks` tick periods, and silence the tick that cannot outlive it.
 *
 * The comparator is switched off rather than pushed out, which is what the LIGHT path above does:
 * mtime counts clk_sys, this window is about to stop clk_sys, and a deadline expressed in a counter
 * whose rate is about to change describes nothing on the other side of it.
 *
 * @param[in] ticks  Tick periods to sleep.
 * @return None.
 */
void os_arch_tick_suppress_cb(uint32_t ticks)
{
    if (soc_tick_interval != 0U)
    {
        irq_set_enabled(SIO_IRQ_MTIMECMP, false);
        soc_powman_window_open(ticks);
    }
}

/******************************************************************************************************/
/**
 * @brief Close the window, report what really elapsed, and put the tick back on a live grid.
 *
 * Re-based on the mtime that exists NOW rather than resumed from the deadline it had before. The
 * counter ran at whatever clk_sys was during the window - a fraction of its normal rate with the
 * PLL down - so its old comparator value describes a moment that never arrives. Writing a fresh
 * deadline is also what de-asserts the line, mip.MTIP being nothing more than mtime >= mtimecmp.
 *
 * @return uint32_t  Whole tick periods since os_arch_tick_suppress_cb().
 */
uint32_t os_arch_tick_resume_cb(void)
{
    uint32_t elapsed = 0U;

    if (soc_tick_interval != 0U)
    {
        elapsed = soc_powman_window_close();

        riscv_timer_set_mtimecmp(riscv_timer_get_mtime() + (uint64_t)soc_tick_interval);
        irq_set_enabled(SIO_IRQ_MTIMECMP, true);
    }

    return elapsed;
}


/*
 * ***********************************************************************************************************
 * Deep sleep
 * ***********************************************************************************************************
 *
 * The mechanism is the Arm package's, because it is the chip's rather than the core's: clk_sys is
 * dropped back onto clk_ref, PLL_SYS is powered down, and POWMAN wakes the core. Three things had
 * to be said differently here, and none of them is the sleep itself.
 *
 * PRIMASK becomes mstatus.MIE, through the kernel's own mask API - RISC-V has no priority threshold
 * to leave alone, so there is nothing for a raw register write to protect.
 *
 * The pending-work test becomes mip & mie. On the Arm side that is ICSR for the scheduler
 * exceptions plus NVIC pending-and-enabled; here the scheduler's own request IS an interrupt
 * (MSIP, from SIO's per-core softirq), so one masked read covers both.
 *
 * SEVONPEND and SLEEPDEEP map onto Hazard3's xh3pwr extension rather than onto nothing. SLEEPDEEP
 * only told the Arm core what to do on WFI - the clocks are stopped by writing the clock
 * registers, which is architecture-neutral - and the event register the Arm package uses for the
 * core-1 rendezvous has an exact counterpart here: h3.block / h3.unblock, the OS_ARCH_WFE() /
 * OS_ARCH_SEV() the port supplies. A block wakes on an unblock event, and an unblock received
 * since the last block is latched, closing the same release-versus-block race SEV/WFE closes.
 *
 * What the rendezvous must NOT do on this core is use MSIP for the park or its release. Here the
 * context-switch request and the cross-core doorbell are the same level-sensitive interrupt, so
 * a release sent that way arrives on core 1 indistinguishable from a reschedule - its parked
 * work test sees "work", aborts with an IPI of its own back to core 0, and core 0's MSIP then
 * stands for the whole masked window, where nothing can take the trap that would clear it. Every
 * subsequent WFI returns on the spot, every window measures zero, and the collapsed windows keep
 * the two cores in that phase against each other. h3.unblock carries no interrupt state at all,
 * which is why the park, the request and the release below all go through it, and MSIP is left
 * with its one real job.
*/

/* The owner alone writes these. The kernel pairs prepare/finish outside its global lock and holds
 * its scheduling mask across the pair. */
static uint32_t soc_sleep_owner_mask = 0U;
static bool     soc_sleep_owner_held = false;

#if (OS_CONFIG_CORE_COUNT > 1U)
/* Each shared word has exactly one writer, ordered with a fence; no kernel API, SDK lock or
 * hardware spinlock is used while either core is parked. Generation zero means released, and a new
 * request is never issued until the old acknowledgement is cleared, so even a wrap cannot let a
 * late acknowledgement authorize another sleep. */
static __IO uint32_t soc_sleep_request = 0U;       /* core 0 writes */
static __IO uint32_t soc_sleep_ack = 0U;           /* core 1 writes */
static __IO uint32_t soc_sleep_abort = 0U;         /* core 1 writes */
static __IO uint32_t soc_sleep_peer_idle = 0U;     /* core 1 writes, advisory only */
static uint32_t soc_sleep_generation = 0U;

/* An idle peer normally acknowledges in a few microseconds. Never wait indefinitely for a preempted
 * idle callback or a core which is busy. Nothing has been slowed yet at this point. */
#define SOC_SLEEP_RENDEZVOUS_US 100U
#endif

/******************************************************************************************************/
/**
 * @brief Additional board veto for protocols whose clock requirements registers cannot reveal.
 *
 * Override strongly for external or polled activity. Called with interrupts masked on both
 * participating cores. It must only inspect board state: no waits or kernel API calls.
 *
 * @return bool  True when the board has nothing that would object.
 */
OS_WEAK bool soc_deep_sleep_allowed_cb(void)
{
    return true;
}

/** The scheduler's own request: mip.MSIP, bit 3. */
#define SOC_SLEEP_MIP_SWI           (1UL << 3)

/** Bound on the retire loop above: the bit clears in a handful of cycles, and a bound only stops a
 *  core that cannot clear it at all from holding the idle path forever. */
#define SOC_SLEEP_SWI_POLLS         64U

/******************************************************************************************************/
/**
 * @brief Pending work on this core. Never clears a source.
 *
 * The scheduler's own request is not always work, and that distinction is the difference between a
 * core that sleeps and one that only looks like it does. On Arm the equivalent is PendSV, which is
 * not pending at this point because the kernel took it before idling. On RISC-V the same request is
 * mip.MSIP, only the trap handler clears it, and the trap cannot run while the idle path holds its
 * mask - so a request that pended before the mask went up can still be standing here. The park and
 * release no longer manufacture one (they ride h3.block/h3.unblock instead of the softirq), which
 * is what made the deep window real: before that change mip was 0x88 against mie 0x808 at every
 * sleep, a WFI that returns on the spot, every window measuring zero, and a deep entry that never
 * happened.
 *
 * Discounting it for the sleep decision is safe because the kernel has already decided: it plans a
 * window only when nothing is runnable, so a request still standing here is one whose reason has
 * gone. Nothing is discarded - the bit is retired just before the WFI, or taken the moment the
 * mask lifts if the window is declined.
 *
 * @param[in] swi_matters  Whether a standing scheduler request counts as work.
 * @return bool  True when something is pending and enabled.
 */
static bool soc_sleep_work_pending_ex(bool swi_matters)
{
    uint32_t pending = (uint32_t)(OS_ARCH_CSR_READ(mip) & OS_ARCH_CSR_READ(mie));

    if (!swi_matters)
    {
        pending &= ~(uint32_t)SOC_SLEEP_MIP_SWI;
    }

    return (pending != 0U);
}

/******************************************************************************************************/
/**
 * @brief Retire a scheduler request that has already been overtaken, just before sleeping.
 *
 * WFI leaves on any pending-and-enabled interrupt whatever mstatus.MIE says, so a standing MSIP
 * returns it on the spot: measured on a Pico 2 as every window ending after zero ticks and 99k
 * cycles where a real one takes 24M. The bit cannot be masked away instead - MSIE is also how the
 * OTHER core wakes this one, and closing it turns a cross-core wake into a wait for the window to
 * expire, which hangs anything expecting prompt delivery.
 *
 * Clearing it is safe here and nowhere else. The kernel opens a window only when nothing is
 * runnable, so a request still standing at this point has been overtaken; and it is not lost even
 * if that judgement is wrong, because os_tick_announce_elapsed() re-tests the scheduler and pends a
 * fresh request the moment the window closes. Anything arriving AFTER this line still sets MSIP and
 * still cuts the sleep short, which is the behaviour the kernel documents.
 *
 * @return None.
 */
static void soc_sleep_swi_retire(void)
{
    uint32_t polls = 0U;

    while (((OS_ARCH_CSR_READ(mip) & SOC_SLEEP_MIP_SWI) != 0UL) && (polls < SOC_SLEEP_SWI_POLLS))
    {
        os_arch_swi_clear_cb();

        /* Read the register back before believing the write. It is a posted write to SIO and
         * mip.MSIP only follows it once it lands; a memory fence orders the store but does not
         * wait for the bit to change, and a WFI issued in that gap still sees the old value - which
         * is exactly the every-window-measures-zero symptom this whole path exists to remove. */
        (void)sio_hw->riscv_softirq;
        polls++;
    }
}

/******************************************************************************************************/
/**
 * @brief Pending work, a standing scheduler request included.
 *
 * @return bool  True when something is pending and enabled.
 */
static bool soc_sleep_work_pending(void)
{
    return soc_sleep_work_pending_ex(true);
}

/******************************************************************************************************/
/**
 * @brief Cancel or release the peer, then return the owner's saved interrupt state.
 *
 * @return None.
 */
static void soc_sleep_release(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    OS_ARCH_DMB();
    if (soc_sleep_request != 0U)
    {
        soc_sleep_request = 0U;
        OS_ARCH_DSB();
        /* The release is an unblock event, not an interrupt, for the reason the Deep sleep block
         * comment spells out: on this core an interrupt aimed at core 1 IS MSIP, the scheduler's
         * own request, and a parked core cannot tell the two apart. The event is sent only when a
         * request was actually standing, so the peer's idle block is not disturbed on the many
         * passes core 0 declines. */
        OS_ARCH_SEV();
    }
#endif
    soc_sleep_owner_held = false;
    os_arch_kernel_mask_restore(soc_sleep_owner_mask);
}

/******************************************************************************************************/
/**
 * @brief Freeze both cores before the kernel freezes its shared time base.
 *
 * A secondary core must be allowed to finish an earlier kernel operation and reach idle before
 * acknowledging. Waiting after the kernel opens its time window would instead prevent that
 * operation from completing.
 *
 * @return bool  Whether the ordinary tickless pass may go ahead.
 */
bool os_arch_soc_sleep_prepare_cb(void)
{
    bool ready = false;
    bool proceed;

    soc_sleep_owner_mask = os_arch_kernel_mask_save();
    OS_ARCH_DSB();

    proceed = !soc_sleep_work_pending();
    if (proceed)
    {
#if (OS_CONFIG_CORE_COUNT > 1U)
        /* The hint avoids polling on every tick while the other core is busy. It is never proof of
         * idleness: the generation-matched acknowledgement below is the only permission. */
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
            /* Core 1 sits in h3.block between its two park checks, so an unblock event is all it
             * takes to notice the request - no MSIP, so nothing on either core mistakes the
             * request for a reschedule. The event latch covers the race with the request word:
             * an unblock arriving before the block falls straight through it. */
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
         * Autonomous and external protocols still require the board veto above. */
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

    /* A busy peer or peripheral prevents shared-clock shutdown, not ordinary tickless sleep on
     * core 0. Pending local work declines this pass instead. */
    return proceed;
}

/******************************************************************************************************/
/**
 * @brief Called only after hardware, elapsed ticks and the kernel window have been restored.
 *
 * @return None.
 */
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
 * @brief Cooperatively park core 1 from idle, so clk_sys may be stopped under both cores.
 *
 * Its own tick comparator is switched off first, for the reason the DEEP resume path gives: mtime
 * is about to stop counting at the rate its deadline was written in. Core 0 owns elapsed-time
 * accounting throughout; core 1 re-bases its own cadence on release.
 *
 * The wait is h3.block, not wfi, and that is not a preference. A block wakes on the unblock event
 * the owner sends to release the park - a wake that carries no interrupt state, where an MSIP
 * wake would be indistinguishable from a reschedule - AND on every pending-and-enabled interrupt,
 * exactly as a WFI does, mstatus.MIE or no. So a real request that arrives while this core is
 * parked is never missed: it wakes the block, the work test above catches it, and the abort below
 * carries it to core 0, which ends the window early rather than sleeping through it. A softirq
 * still standing when the park exits is deliberately left for the mask-restore to take, so the
 * request it carries is delivered, never discarded.
 *
 * @return None.
 */
static void soc_sleep_peer_park(void)
{
    uint32_t mask = os_arch_kernel_mask_save();
    uint32_t generation;

    OS_ARCH_DSB();
    OS_ARCH_DMB();
    generation = soc_sleep_request;

    if ((generation != 0U) && os_task_current_is_idle() && !soc_sleep_work_pending())
    {
        bool wake_sent = false;

        irq_set_enabled(SIO_IRQ_MTIMECMP, false);
        OS_ARCH_DSB();

        /* The tick may have pended between the first test and disabling its line. Preserve that
         * work and reject this request rather than clearing it to manufacture an idle core. */
        if (!soc_sleep_work_pending() && (soc_sleep_request == generation))
        {
            OS_ARCH_DMB();
            soc_sleep_ack = generation;
            OS_ARCH_DSB();

            while (soc_sleep_request == generation)
            {
                OS_ARCH_DMB();
                if (!wake_sent && soc_sleep_work_pending())
                {
                    soc_sleep_abort = generation;
                    OS_ARCH_DSB();
                    /* A real interrupt on the owner is what cuts its WFI short, an event would
                     * not - core 0 sleeps in WFI, not in a block. MSIP is the one channel that
                     * is, and it is correct that this looks to the owner like a reschedule:
                     * it closes the window early and the kernel re-plans, which is the whole
                     * point of an abort. */
                    os_arch_core_ipi_request_cb(0U);
                    wake_sent = true;
                }
                OS_ARCH_DSB();
                /* Woken by the owner's release event, by an abort-worthy interrupt, or
                 * spuriously (a stray event from elsewhere in the system); the loop condition
                 * and the work test above are the only things that decide which. With the mask
                 * held, none of the interrupt sources runs a handler here. */
                OS_ARCH_WFE();
            }
        }
        else
        {
            soc_sleep_abort = generation;
            OS_ARCH_DSB();
        }

        /* Back onto a live grid, exactly as the DEEP resume path does for core 0. */
        if (soc_tick_interval != 0U)
        {
            riscv_timer_set_mtimecmp(riscv_timer_get_mtime() + (uint64_t)soc_tick_interval);
        }
        irq_set_enabled(SIO_IRQ_MTIMECMP, true);
        OS_ARCH_DSB();

        /* Published only once this generation's state is whole again. Core 0 declines another
         * request while a cancelled or finished acknowledgement is still visible. */
        soc_sleep_ack = 0U;
        OS_ARCH_DMB();
    }

    os_arch_kernel_mask_restore(mask);
}
#endif

/******************************************************************************************************/
/**
 * @brief The sleep itself: drop off the PLL, stop it, and halt the core until POWMAN wakes it.
 *
 * Register for register the Arm package's sequence, and deliberately so - what stops the clocks on
 * this chip is the clock tree, not the core. The one line that is missing is SCR.SLEEPDEEP, which
 * has no counterpart and needs none: it told a Cortex-M what to do on WFI, and by the time this
 * WFI runs there is no PLL left to gate.
 *
 * It stops short of the deepest route - moving clk_ref onto LPOSC and stopping the crystal - for
 * the reason the Arm package gives: that is the variant where a mistake leaves the core with no
 * clock to execute the restore from, recoverable only through BOOTSEL.
 *
 * @return None.
 */
void os_arch_soc_sleep_cb(void)
{
    /* The kernel's pre-sleep board callback ran since prepare. Recheck autonomous peripheral
     * activity and incoming work immediately before touching a shared clock. */
    bool ready = soc_sleep_owner_held && !soc_sleep_work_pending_ex(false);

#if (OS_CONFIG_CORE_COUNT > 1U)
    OS_ARCH_DMB();
    ready = ready && (soc_sleep_ack == soc_sleep_request) && (soc_sleep_request != 0U) &&
            (soc_sleep_abort != soc_sleep_request);
#endif
    ready = ready && soc_deep_peripherals_ready() && soc_deep_sleep_allowed_cb();

    /* MISRA C:2012 Rule 15.5 - the declined path takes the else arm rather than returning from the
     * middle, so this function still has its single exit at the end. */
    if (!ready)
    {
        soc_sleep_swi_retire();
        OS_ARCH_DSB();
        OS_ARCH_IDLE();
    }
    else
    {
        uint32_t pll_cs   = pll_sys_hw->cs;
        uint32_t pll_fb   = pll_sys_hw->fbdiv_int;
        uint32_t pll_prim = pll_sys_hw->prim;
        uint32_t pll_pwr  = pll_sys_hw->pwr;
        uint32_t sys_ctrl = clocks_hw->clk[clk_sys].ctrl;
        uint32_t sys_div  = clocks_hw->clk[clk_sys].div;
        uint32_t sys_selected = clocks_hw->clk[clk_sys].selected;

        /* Off the PLL first, and glitchlessly: clk_sys back to clk_ref, which is still running from
         * whatever the application put it on. Only once nothing is fed from the PLL may it be
         * stopped - pulling it out from under a running clk_sys stops the core where it stands. */
        clocks_hw->clk[clk_sys].ctrl = sys_ctrl & ~CLOCKS_CLK_SYS_CTRL_SRC_BITS;

        while ((clocks_hw->clk[clk_sys].selected & 1U) == 0U)
        {
        }

        pll_sys_hw->pwr = PLL_PWR_BITS;   /* every block powered down */

#if (OS_CONFIG_TEST_ENABLE == 1U)
        /* The suite reports this: a deep build that always falls back to LIGHT is otherwise
         * indistinguishable from one that works. See os_test_deep_sleep_entries in ahura.h. */
        os_test_deep_sleep_entries++;
#endif
        /* Retire a scheduler request that may have been overtaken before halting, then sleep with
         * MSIE left open. With the rendezvous on h3.block/h3.unblock nothing manufactures MSIP
         * any more, so the retire is belt and braces rather than the load-bearing part it used to
         * be. Masking MSIE here instead would close the one channel a genuine cross-core wake
         * uses - the parked peer's abort and any remote kernel entry both arrive as MSIP - and a
         * real request then waits out the whole window, which is how an earlier revision hung the
         * suite. A request that arrives after the retire still cuts the sleep short, exactly as
         * the kernel documents. */
        soc_sleep_swi_retire();
        OS_ARCH_DSB();
        OS_ARCH_IDLE();
        OS_ARCH_ISB();

        /* Back up in the order it came down: the PLL has to be locked before anything is fed from
         * it. Restored from its own saved registers rather than recomputed, so it cannot disagree
         * with the clock tree the application configured. */
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
}

#endif /* OS_CONFIG_TICKLESS_DEEP_ENABLE */

#endif /* OS_CONFIG_TICKLESS_ENABLE */

/******************************************************************************************************/
/**
 * @brief Start the periodic tick.
 *
 * Called by the port from os_arch_tick_init(), on each core. The timer itself is shared by both
 * cores and only wants configuring once, but mtimecmp is core-local - the datasheet is explicit
 * that "each core gets a copy of this register, with the comparison result routed to its own
 * interrupt line" - so the comparator and the IRQ are armed per core.
 */
void os_arch_tick_init_cb(void)
{
    uint32_t clock_hz = os_arch_clock_hz_get();

    if ((clock_hz != 0U) && (OS_CONFIG_TICK_HZ != 0U))
    {
    if (get_core_num() == 0U)
    {
        /* Count clk_sys rather than the `ticks` block's 1 MHz reference. That makes the tick period
         * derive from the same number os_arch_clock_hz_get() reports and the kernel already uses for
         * its microsecond waits, instead of depending on how the ticks block happens to be set up -
         * and it is what SysTick does on the Arm side of this chip. */
        riscv_timer_set_fullspeed(true);
        riscv_timer_set_enabled(true);
    }

    soc_tick_interval = clock_hz / OS_CONFIG_TICK_HZ;

    /* A zero interval means a tick faster than the clock: nothing sane to program, so the
     * comparator and the enable below are skipped along with it. */
    if (soc_tick_interval != 0U)
    {
    riscv_timer_set_mtimecmp(riscv_timer_get_mtime() + (uint64_t)soc_tick_interval);

    /* The HANDLER is registered once; the comparator above and the enable below are per core.
     *
     * Those are not the same scope, and the difference is easy to miss because everything else in
     * this function is core-local. mtimecmp lives in SIO and the interrupt enable is a core CSR,
     * so both belong to whichever core is running. The handler CHAIN does not: unless
     * PICO_VTABLE_PER_CORE is set - and it defaults to 0 - multicore_launch_core1() hands core 1
     * core 0's own mtvec, so the two cores share one table. Registering from both would put
     * soc_tick_isr in that chain TWICE, and every timer interrupt would then push mtimecmp forward
     * by two intervals and count two ticks, halving the real tick rate on both cores.
     *
     * Shared rather than exclusive so a handler the application already installed on this line is
     * not silently displaced. */
    if (get_core_num() == 0U)
    {
        irq_add_shared_handler(SIO_IRQ_MTIMECMP, soc_tick_isr,
                               PICO_SHARED_IRQ_HANDLER_LOWEST_ORDER_PRIORITY);
    }

    irq_set_enabled(SIO_IRQ_MTIMECMP, true);
    }
    }
}

/*
 * ***********************************************************************************************************
 * Start-up and idle
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Per-core SoC start-up, called from inside os_init().
 *
 * Nothing to arm for the context-switch interrupt: SIO_RISCV_SOFTIRQ needs no claiming, and the
 * port enables mie.MSIE itself in os_arch_init(). The Arm package has to claim a doorbell and route
 * its IRQ here; this one genuinely has nothing to do.
 */
void os_arch_soc_init_cb(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    soc_core_id_verify();
#endif

    sio_hw->riscv_softirq = SOC_SOFTIRQ_CLR(get_core_num());
}

/******************************************************************************************************/
/**
 * @brief Idle the core until an interrupt arrives.
 *
 * WFI on core 0, and deliberately so: mtime keeps running through WFI because it lives in SIO
 * rather than in the core, so the reason the Arm package uses WFE everywhere - a gated clock would
 * stop that core's own tick - does not arise here.
 *
 * Core 1, under DEEP, waits in h3.block between its two park checks instead. A block wakes on
 * pending-and-enabled interrupts exactly as a WFI does, so the tick and every reschedule reach it
 * unchanged; what it adds is the unblock event, the channel core 0 uses for the park request and
 * the release - a wake that carries no interrupt state and so cannot be mistaken for a scheduler
 * request on either side.
 *
 * @return None.
 */
void os_arch_soc_idle_cb(void)
{
#if (OS_CONFIG_TICKLESS_ENABLE == 1U) &&     (OS_CONFIG_TICKLESS_DEEP_ENABLE == 1U) && (OS_CONFIG_CORE_COUNT > 1U)
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
        OS_ARCH_IDLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Report what this package knows about core 1's bring-up.
 *
 * Called by the kernel only after something has already gone wrong, and deliberately not at launch
 * time: os_arch_core_launch_cb() runs inside os_start(), where a USB console has not been opened by
 * the host yet and anything written is dropped unseen.
 *
 * The two answers split the search in half. Never reached means the launch itself failed - the
 * entry point, the stack or the vector table core 1 was handed. Reached means the launch, the trap
 * table and this package are fine, and the fault is in what follows: os_core_start(), the tick this
 * core arms for itself, or mie.MSIE.
 */
void os_arch_soc_diagnose_cb(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    if (soc_core_reached == 0xFFU)
    {
        printf("         [soc] core 1 NEVER reached its entry point - the launch itself failed.\r\n");
    }
    else
    {
        printf("         [soc] core %u DID reach its entry point, so the launch and the trap\r\n",
               (unsigned)soc_core_reached);
        printf("               table are fine; look at os_core_start() and this core's tick.\r\n");
    }

    (void)fflush(stdout);
#endif
}

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Core 1's entry point: clear this core's software-interrupt bit, then enter the scheduler.
 *
 * os_core_start(), NOT os_start(). os_start() is core 0's entry and does three things wrong from
 * here: it sets os_kernel_running again, it walks the launch loop - so this core would ask the SDK
 * to launch ITSELF - and it never calls os_arch_init(), which is what checks this core's trap
 * vector and sets mie.MSIE. Without that enable the context-switch interrupt is masked on this core
 * forever, so it never dispatches and every task pinned to it simply never runs.
 *
 * The softirq clear is the same per-core start-up os_arch_soc_init_cb() gave core 0 from os_init(),
 * which this core never runs: a bit left set before the core existed would otherwise present itself
 * as a context-switch request on the first instruction after the enable.
 */
static void soc_core1_entry(void)
{
    soc_core_reached = (uint8_t)get_core_num();

    soc_core_id_verify();

    sio_hw->riscv_softirq = SOC_SOFTIRQ_CLR(get_core_num());

    /* Does not return. */
    os_core_start();
}
#endif

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Called right before a suppressed idle window, with interrupts masked.
 *
 * Left empty, which selects a plain WFI: the core stalls and every clock keeps running, so nothing
 * needs saving here and the post-sleep hook has nothing to restore. That is also what makes the
 * window measurable, since mtimecmp keeps counting through it.
 *
 * The deeper modes on this chip stop the timers the kernel measures against, which is a different
 * feature and not one this package claims - see the tickless section of doc/porting.md.
 *
 * Weak, so an application that must quiesce something of its own - a UART with bytes still in its
 * FIFO, a sensor mid-conversion - replaces this one hook and leaves the rest alone.
 *
 * @return None.
 */
OS_WEAK void os_tickless_pre_sleep_cb(void)
{
}

/******************************************************************************************************/
/**
 * @brief Called right after the window closes, still masked and before the sleep is announced.
 *
 * @return None.
 */
OS_WEAK void os_tickless_post_sleep_cb(void)
{
}

#endif /* OS_CONFIG_TICKLESS_ENABLE */

/* SDK TIMER is shared, unlike the per-hart mcycle epochs. */
uint32_t os_arch_reference_clock_hz_cb(void)
{
    return 1000000U;
}

uint64_t os_arch_reference_clock_get_cb(void)
{
    return time_us_64();
}
