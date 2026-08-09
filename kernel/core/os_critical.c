/**
 * @file os_critical.c
 * @brief Critical section module implementation (kernel interrupt mask based, nesting aware).
 *
 * The interrupt mask is the port's kernel mask: PRIMASK by default, or BASEPRI up to
 * OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY when that is nonzero (interrupts above the threshold
 * keep zero kernel latency but must never call a kernel API). On multi-core builds the outermost
 * enter also takes the global kernel spinlock, so a critical section excludes the other cores
 * too; nesting is per core, and both compile to nothing on single-core builds.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "os_internal.h"

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static os_arch_spinlock_t os_critical_kernel_lock = OS_ARCH_SPINLOCK_INIT;
static __IO uint32_t      os_critical_nesting_count[OS_CONFIG_CORE_COUNT];
static uint32_t           os_critical_saved_mask[OS_CONFIG_CORE_COUNT];

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Enter critical section: raise the kernel interrupt mask, lock out other cores,
 *        count nesting.
 *
 * Safe from task and interrupt context. Keep critical sections short:
 * kernel-maskable interrupts (including the kernel tick) are masked while
 * inside, and on multi-core builds the other cores spin at their own
 * outermost enter.
 *
 * @return None.
 */
void os_critical_enter(void)
{
    uint32_t mask_state = os_arch_kernel_mask_save();
    uint32_t core;

    /* In BASEPRI mode, trap kernel API calls from interrupts the kernel mask
     * cannot reach (above-threshold priorities); no-op in PRIMASK mode. */
    os_arch_isr_priority_check();

    core = os_arch_core_id_get();

    /* Remember the mask state of the outermost enter so exit can restore it
     * instead of blindly unmasking. */
    if (os_critical_nesting_count[core] == 0U)
    {
        os_arch_spinlock_acquire(&os_critical_kernel_lock);
        os_critical_saved_mask[core] = mask_state;
    }

    os_critical_nesting_count[core]++;
}

/******************************************************************************************************/
/**
 * @brief Exit critical section: release the lock and restore the kernel interrupt mask at
 *        the outermost level.
 *
 * @return None.
 */
void os_critical_exit(void)
{
    uint32_t core = os_arch_core_id_get();

    /* An exit without a matching enter means the pairing is broken somewhere:
     * whoever raised the mask is no longer tracked, so the next outermost exit
     * would restore a stale state. Returning quietly (as this still does with
     * assertions off) keeps the kernel consistent but hides the caller's bug. */
    OS_ASSERT(os_critical_nesting_count[core] != 0U);

    if (os_critical_nesting_count[core] != 0U)
    {
        os_critical_nesting_count[core]--;

        if (os_critical_nesting_count[core] == 0U)
        {
            os_arch_spinlock_release(&os_critical_kernel_lock);
            os_arch_kernel_mask_restore(os_critical_saved_mask[core]);
        }
    }
}

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Acquire the cross-core kernel spinlock only; caller has already raised its own
 *        local kernel mask directly (PendSV and tick-time scheduler walks, which must
 *        exclude the OTHER cores' os_critical_enter callers on the shared ready/delay
 *        lists and registries, but manage their local mask themselves rather than going
 *        through the nesting-counted os_critical_enter/exit pair).
 *
 * Never call this while already holding the lock via os_critical_enter, or while it will still be
 * held across a call that takes it itself (e.g. os_task_wake): it is not recursive, so the second
 * acquire on the same core spins forever. Compiles to nothing on single-core builds.
 *
 * @return None.
 */
void os_critical_multicore_lock(void)
{
    os_arch_spinlock_acquire(&os_critical_kernel_lock);
}

/******************************************************************************************************/
/**
 * @brief Release the cross-core kernel spinlock acquired by os_critical_multicore_lock.
 *
 * @return None.
 */
void os_critical_multicore_unlock(void)
{
    os_arch_spinlock_release(&os_critical_kernel_lock);
}
#endif /* OS_CONFIG_CORE_COUNT > 1U */
