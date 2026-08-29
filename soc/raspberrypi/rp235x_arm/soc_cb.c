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
#include "pico/multicore.h"

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
