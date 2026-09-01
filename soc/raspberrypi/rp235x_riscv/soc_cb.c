/**
 * @file soc_cb.c
 * @brief SoC-owned callbacks for the RP2350 / RP2354 running their Hazard3 RISC-V cores.
 *
 * The RISC-V sibling of ../rp235x_arm. Same silicon, different core, and almost everything the
 * kernel asks of a chip is answered differently here - which is why this is a separate package
 * rather than an #if inside that one. See doc/soc.md.
 *
 * WHAT REPLACES PENDSV
 *
 * SIO_RISCV_SOFTIRQ, at SIO offset 0x1a0. It drives mip.MSIP - the machine software interrupt,
 * trap cause 3 - and the RP2350 gives it one set bit and one clear bit PER CORE:
 *
 *     bit 0  CORE0_SET     bit 8  CORE0_CLR
 *     bit 1  CORE1_SET     bit 9  CORE1_CLR
 *
 * That per-core addressing is why this one register answers two of the kernel's questions at once.
 * Asking THIS core to reschedule and asking the OTHER core to reschedule are the same write with a
 * different bit, so os_arch_swi_request_cb() and os_arch_core_ipi_request_cb() are one line each.
 * The Arm package needs two separate mechanisms for the same job: PendSV for itself, a doorbell for
 * its sibling.
 *
 * The datasheet also settles the race for us: "It is safe for both cores to write to this register
 * on the same cycle. The set/clear effect is accumulated across both cores, and then applied. If a
 * flag is both set and cleared on the same cycle, only the set takes effect." Set winning over
 * clear is exactly the safe direction - a request that arrives while the handler is acknowledging
 * survives, so no wakeup is lost.
 *
 * WHY THE TICK IS AN EXTERNAL IRQ
 *
 * mtime/mtimecmp can reach the core two ways: straight in on mip.MTIP (trap cause 7), or as
 * SIO_IRQ_MTIMECMP, ordinary system IRQ 29. This package takes IRQ 29, for two reasons.
 *
 * The SDK recommends it - crt0_riscv.S says of MTIP that "this may be a better option, because it
 * plays nicely with interrupt preemption". And it is what makes os_arch_in_isr() correct: that
 * function reads Hazard3's meicontext, which accounts for external IRQs and knows nothing about
 * MTIP. A tick on cause 7 would run with the kernel believing it was in task context. See the "Trap
 * context" section of the RISC-V port header.
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
#include "hardware/riscv_platform_timer.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

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

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Tickless idle
 * ***********************************************************************************************************
 *
 * mtime free-runs at clk_sys and mtimecmp is an absolute deadline, so a suppressed window is one
 * write: put the next interrupt N tick periods out instead of one. The port cannot do it because
 * the privileged spec never says where these registers live; this package can.
*/

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
     * Otherwise no ceiling of this timer's own: mtime and mtimecmp are 64 bits, the deadline is
     * absolute, and ticks * soc_tick_interval cannot approach that width for any interval a
     * kernel tick could have. What bounds a window here is the kernel's own 32-bit tick count,
     * which is what this reports. The 24-bit answer this used to give came from the config, and
     * was SysTick's limit on a part that has no SysTick. */
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
 * WFI, not the WFE the Arm package uses. WFE is an Arm instruction with no RISC-V counterpart, and
 * the reason the Arm side needs it does not arise here: it avoids gating the clock SysTick counts,
 * while mtime keeps running through WFI because it lives in SIO rather than in the core.
 */
void os_arch_soc_idle_cb(void)
{
    OS_ARCH_IDLE();
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
