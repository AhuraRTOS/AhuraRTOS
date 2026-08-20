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
        return;
    }

    /* Non-blocking on purpose. A full FIFO already means an unhandled signal is waiting at the
     * other core, which is the same result this call wants, and blocking here would stall a
     * scheduler path with interrupts masked. */
    (void)multicore_fifo_push_timeout_us(SOC_IPI_TOKEN, 0);
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
