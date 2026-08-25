/**
 * @file os_kernel.c
 * @brief Kernel lifecycle core implementation.
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

#include "os_internal.h"

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TEST_ENABLE == 0U)
static os_err_t os_main_system_init(void);
static void      os_main_task_entry(void *context);
#endif

#if (OS_CONFIG_TEST_ENABLE == 1U)
static os_err_t os_test_system_init(void);
static void      os_test_task_entry(void *context);
#endif

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static __IO bool os_kernel_running = false;

/* Scheduler lock, per core. Nonzero means this core defers its own context switches with
 * interrupts left fully live; the pending flag remembers a switch that was swallowed while
 * it was held, so the outermost unlock can issue it. Not static: the scheduler reads both
 * on its hot paths in os_task.c (declared in os_internal.h), where a call would cost more
 * than the check itself. __IO because the tick and PendSV read them from ISR context. */
__IO uint32_t os_kernel_lock_count[OS_CONFIG_CORE_COUNT];
__IO bool     os_kernel_switch_pending[OS_CONFIG_CORE_COUNT];

#if (OS_CONFIG_TEST_ENABLE == 0U)
OS_TASK_DEFINE(tsk_main, OS_CONFIG_MAIN_TASK_STACK_SIZE);
#endif

#if (OS_CONFIG_TEST_ENABLE == 1U)
OS_TASK_DEFINE(tsk_test, OS_CONFIG_TEST_STACK_SIZE);
#endif

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize kernel subsystems. Call once before any other kernel API.
 *
 * @return None.
 */
/******************************************************************************************************/
/**
 * @brief Weak default for the SoC start-up hook: a target with no SoC package has nothing to do
 *        here, so this stays empty and costs one call.
 *
 * A package under soc/ replaces it with a strong definition - strong beats weak whatever
 * order the linker sees them in, which two weak definitions would not.
 */
OS_WEAK void os_arch_soc_init_cb(void)
{
}

/******************************************************************************************************/
/**
 * @brief Weak default for the SoC bring-up diagnosis hook: a target with no SoC package, or one
 *        with nothing to add, does nothing here.
 *
 * A package replaces it with a strong definition, exactly as with os_arch_soc_init_cb above.
 */
OS_WEAK void os_arch_soc_diagnose_cb(void)
{
}

/******************************************************************************************************/
/**
 * @brief Weak default for the idle wait: a plain WFI, which is correct on parts whose timers
 *        keep running through it.
 *
 * A SoC package replaces it where that is not true - see the declaration in ahura.h.
 */
OS_WEAK void os_arch_soc_idle_cb(void)
{
    OS_ARCH_IDLE();
}

/******************************************************************************************************/
void os_init(void)
{
    /* First, before os_arch_init() and long before os_tick_init(): a SoC package publishes the
     * CPU clock here, and the tick period is computed from it. */
    os_arch_soc_init_cb();

    os_arch_init();
    os_task_system_init();
    (void)os_task_idle_create();

    /* The kernel timer task, at OS_CONFIG_TIMER_PRIORITY: it runs both timer expiries and the
     * deferred work, which is why there is no second service task beside it.
     * Created as a system task, so the application cannot pause or delete it whatever priority it
     * is given. */
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    (void)os_timer_system_init();
#endif
    /* The log task sits at the opposite end from the timer task: lowest priority,
     * so draining the log never preempts application work. Created before the
     * main/test task so anything they log at startup already has a consumer. */
#if (OS_CONFIG_LOG_ENABLE == 1U)
    (void)os_log_system_init();
#endif
    /* The self-test suite takes priority over the default application task:
     * both otherwise run tsk_main-priority-range code from os_init(), and a
     * test build's job is to exercise the kernel in isolation, not race the
     * application's own task against it. Outside test builds, tsk_main is
     * created unconditionally. */
#if (OS_CONFIG_TEST_ENABLE == 0U)
    (void)os_main_system_init();
#endif
#if (OS_CONFIG_TEST_ENABLE == 1U)
    (void)os_test_system_init();
#endif

    os_tick_init();
}

/******************************************************************************************************/
/**
 * @brief Start the scheduler and switch to task context. Does not return.
 *
 * @return None.
 */
void os_start(void)
{
    /* os_init() created it, and os_task_idle_create is idempotent - so a missing one here means
     * that creation failed and a retry would fail identically. Assert rather than re-create:
     * without an idle task the first switch restores through a NULL stack_ptr and hard faults. */
    OS_ASSERT(os_task_idle_is_created());

    os_kernel_running = true;

#if (OS_CONFIG_CORE_COUNT > 1U)
    /* Secondary cores start HERE, not in os_init(), and the difference is not cosmetic: a core
     * that enters os_core_start() early begins dispatching immediately, while core 0 is still
     * inside os_init() creating the idle task and the service tasks. It would pick from ready
     * lists that are half-built, against TCBs mid-initialisation, possibly before an idle task
     * exists at all - which is the very thing the assert above exists to catch on this core.
     *
     * By this line os_init() has returned, every kernel task exists, and os_kernel_running is
     * already true, so a secondary core sees a complete kernel the instant it looks. The launch
     * precedes os_arch_start_first_task() only because that call never returns.
     *
     * The SoC layer supplies the callback because booting a core is chip hardware with no
     * architectural form: the kernel can say WHEN, never HOW. */
    for (uint32_t core = 1U; core < OS_CONFIG_CORE_COUNT; core++)
    {
        os_arch_core_launch_cb(core);
    }
#endif

    os_arch_start_first_task();

    /* Never reached. */
    while (1)
    {
    }
}

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief Enter the scheduler on a secondary core. Does not return.
 *
 * Call from the secondary core after os_start() is running on core 0, once
 * the SoC layer has booted the core with a vector table routing PendSV
 * and SysTick to the kernel handlers. SHPR, SysTick, DWT and MSPLIM are all
 * banked per core, so the same architecture init runs here; the per-core
 * SysTick drives this core's preemption while core 0 owns the time base.
 *
 * @return None.
 */
void os_core_start(void)
{

    /* Same precondition as os_start(): without an idle task this core's first switch restores
     * through a NULL stack_ptr and hard faults. Cheap, and it turns a secondary core started too
     * early into a named assertion rather than a fault with no history. */
    OS_ASSERT(os_task_idle_is_created());

    /* Getting past this means the vector check inside os_arch_init() agreed that THIS core's
     * table routes the context switch - the single most likely thing to be wrong on a fresh SoC
     * port, and silent when it is. */
    os_arch_init();
    os_arch_tick_init();
    os_arch_start_first_task();

    /* Never reached. */
    while (1)
    {
    }
}

#endif /* OS_CONFIG_CORE_COUNT > 1U */

/******************************************************************************************************/
/**
 * @brief Return true once the scheduler has been started.
 *
 * @return bool  True when the scheduler is running.
 */
bool os_kernel_is_running(void)
{
    return os_kernel_running;
}

/******************************************************************************************************/
/**
 * @brief Defer context switches on the calling core, leaving interrupts enabled (nesting counted).
 *
 * The preemption barrier os_critical_enter is not: a critical section stops interrupts and so stops
 * everything, this stops only the scheduler. Interrupts keep running and keep waking tasks; they
 * just do not get the CPU until the outermost unlock, which then issues the switch it swallowed.
 * Use it against other TASKS - data an ISR or another core also touches still needs a critical
 * section, since this lock excludes neither.
 *
 * The calling task cannot block while it holds one: blocking primitives behave as if given
 * OS_WAIT_NOTHING, os_delay_ms busy-waits, and pause/delete of the CALLER return OS_ERR_BUSY.
 * Blocking means switching away, which is what the lock forbids. No-op from interrupt context.
 *
 * @return None.
 */
void os_kernel_lock(void)
{
    if (!os_arch_in_isr())
    {
        uint32_t mask_state = os_arch_kernel_mask_save();

        os_kernel_lock_count[os_arch_core_id_get()]++;
        os_arch_kernel_mask_restore(mask_state);
    }
}

/******************************************************************************************************/
/**
 * @brief Release one level of scheduler lock; at the outermost level, take any switch that was
 *        deferred while it was held.
 *
 * The deferred switch is issued after the count reaches zero, so the PendSV it pends finds the
 * scheduler open and really does switch. Unbalanced calls are an OS_ASSERT (with assertions off,
 * a call without a matching lock is ignored rather than wrapping the counter).
 *
 * @return None.
 */
void os_kernel_unlock(void)
{
    bool switch_due = false;

    if (!os_arch_in_isr())
    {
        uint32_t mask_state = os_arch_kernel_mask_save();
        uint32_t core       = os_arch_core_id_get();

        /* An unlock with no matching lock means the pairing is broken somewhere, exactly as in
         * os_critical_exit: decrementing anyway would wrap the counter and lock the scheduler for
         * ~4 billion nested unlocks. */
        OS_ASSERT(os_kernel_lock_count[core] != 0U);

        if (os_kernel_lock_count[core] != 0U)
        {
            os_kernel_lock_count[core]--;

            if (os_kernel_lock_count[core] == 0U)
            {
                switch_due                     = os_kernel_switch_pending[core];
                os_kernel_switch_pending[core] = false;
            }
        }

        os_arch_kernel_mask_restore(mask_state);
    }

    /* Outside the mask: the switch is due now, and pending it under the mask
     * would only delay it to the restore above. */
    if (switch_due && os_kernel_is_running())
    {
        OS_ARCH_CONTEXT_SWITCH_REQUEST();
    }
}

/******************************************************************************************************/
/**
 * @brief Whether the calling core currently has its scheduler locked (ISR-safe).
 *
 * @return bool  True while at least one os_kernel_lock is outstanding on this core.
 */
bool os_kernel_is_locked(void)
{
    return (os_kernel_lock_count[os_arch_core_id_get()] != 0U);
}

#if (OS_CONFIG_ASSERT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Report a failed OS_ASSERT and halt.
 *
 * The application hook runs first, while the failure's context is still intact, so it can print
 * or store the location. It then falls through to the same trap the boot-time configuration
 * checks use: interrupts masked, core parked, debugger stops here. There is deliberately no way
 * to continue - an assertion means an invariant the rest of the kernel relies on is already
 * broken, so running on would only corrupt more state before the eventual failure.
 *
 * @param[in] file  Source file of the failed check.
 * @param[in] line  Line number of the failed check.
 * @return None. Never returns.
 */
void os_assert_failed(const char *file, uint32_t line)
{
    os_assert_failed_cb(file, line);
    os_arch_config_fault_trap();

    /* os_arch_config_fault_trap never returns; the loop only convinces the
     * compiler of that when it is inlined as a plain call. */
    while (1)
    {
    }
}

#endif /* OS_CONFIG_ASSERT_ENABLE */

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TEST_ENABLE == 0U)
/******************************************************************************************************/
/**
 * @brief Create and start the default application task. Called from os_init().
 *
 * Not compiled in when OS_CONFIG_TEST_ENABLE is also 1: the self-test suite
 * runs instead of the application's own task in that build (see os_init()).
 *
 * @return os_err_t  Status code.
 */
static os_err_t os_main_system_init(void)
{
    os_err_t status;

    os_task_config_t config =
    {
        os_main_task_entry,
        NULL,
        OS_CONFIG_MAIN_TASK_PRIORITY,
        OS_TASK_CORE_ANY
    };

    status = os_task_create(&tsk_main, &config);

    /* Only start what was actually created; a failed create is reported as-is. */
    if (status == OS_ERR_NONE)
    {
        status = os_task_start(&tsk_main);
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Default application task entry: wraps os_main() so a return from it cleanly
 *        exits the task instead of falling off the end of an entry function.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_main_task_entry(void *context)
{
    (void)context;
    os_main();
}
#endif /* OS_CONFIG_TEST_ENABLE == 0U */

#if (OS_CONFIG_TEST_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Create and start the self-test task. Called from os_init().
 *
 * @return os_err_t  Status code.
 */
static os_err_t os_test_system_init(void)
{
    os_err_t status;

    os_task_config_t config =
    {
        os_test_task_entry,
        NULL,
        OS_CONFIG_TEST_PRIORITY,
#if (OS_CONFIG_CORE_COUNT > 1U)
        /* Pinned to core 0, matching the suite's own helpers (TEST_TASK_CONFIG in os_test.c).
         * The checks for priority inheritance, the timer stop race and the notification storm
         * all assume sender, waiter and service task share one core - with OS_TASK_CORE_ANY the
         * suite task migrates to core 1 and those checks fail on SMP even though the kernel
         * behaves as specified. */
        OS_TASK_CORE(0)
#else
        OS_TASK_CORE_ANY
#endif
    };

    status = os_task_create(&tsk_test, &config);

    /* Only start what was actually created; a failed create is reported as-is. */
    if (status == OS_ERR_NONE)
    {
        status = os_task_start(&tsk_test);
    }

    return status;
}

/******************************************************************************************************/
/**
 * @brief Self-test task entry: wraps os_test() so a return from it cleanly exits the
 *        task instead of falling off the end of an entry function.
 *
 * @param[in] context  Unused.
 * @return None.
 */
static void os_test_task_entry(void *context)
{
    (void)context;
    os_test();
}
#endif /* OS_CONFIG_TEST_ENABLE */
