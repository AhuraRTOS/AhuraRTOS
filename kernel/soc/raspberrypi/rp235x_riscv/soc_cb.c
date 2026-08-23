/**
 * @file soc_cb.c
 * @brief SoC-owned callbacks for the RP2350 / RP2354 running their Hazard3 RISC-V cores.
 *
 * The RISC-V sibling of ../rp235x_arm. Same silicon, different core, and almost everything the
 * kernel asks of a chip is answered differently here - which is why this is a separate package
 * rather than an #if inside that one. See ../../../../doc/soc.md.
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

/******************************************************************************************************/
/**
 * @brief Release the secondary core into the kernel.
 *
 * @param[in] core_id  Core to start; only core 1 can be started on this chip.
 */
void os_arch_core_launch_cb(uint32_t core_id)
{
    if (core_id == 1U)
    {
        multicore_launch_core1(os_start);
    }
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
    if (core_id == 0U)
    {
        return 0U;                                  /* core 0 keeps the linker's stack */
    }

    return (uint32_t)(uintptr_t)&soc_handler_stack[core_id - 1U][SOC_CONFIG_HANDLER_STACK_SIZE];
}

/******************************************************************************************************/
/**
 * @brief Limit of the trap stack for the given core.
 */
uint32_t os_arch_handler_stack_limit_cb(uint32_t core_id)
{
    if (core_id == 0U)
    {
        return 0U;
    }

    return (uint32_t)(uintptr_t)&soc_handler_stack[core_id - 1U][0];
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

    if ((clock_hz == 0U) || (OS_CONFIG_TICK_HZ == 0U))
    {
        return;
    }

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

    if (soc_tick_interval == 0U)
    {
        return;                                     /* tick faster than the clock: nothing sane to program */
    }

    riscv_timer_set_mtimecmp(riscv_timer_get_mtime() + (uint64_t)soc_tick_interval);

    /* Shared rather than exclusive: the timer IRQ is one line per core, and a handler already
     * installed by the application should not be silently displaced. */
    irq_add_shared_handler(SIO_IRQ_MTIMECMP, soc_tick_isr, PICO_SHARED_IRQ_HANDLER_LOWEST_ORDER_PRIORITY);
    irq_set_enabled(SIO_IRQ_MTIMECMP, true);
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
    __asm volatile("wfi");
}

/******************************************************************************************************/
/**
 * @brief Bring-up diagnostics hook.
 */
void os_arch_soc_diagnose_cb(void)
{
}
