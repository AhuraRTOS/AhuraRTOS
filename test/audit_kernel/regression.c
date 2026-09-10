/**
 * @file regression.c
 * @brief Executes real task, inheritance, mutex and kernel code against a deterministic port.
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */
/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "../../kernel/os_task.c"
#include "../../kernel/os_task_mutex.c"
#include "../../kernel/os_list.c"
#include "../../kernel/os_mutex.c"
#include "../../kernel/os_kernel.c"

volatile uint32_t audit_result;
static uint32_t audit_mask;
static uint32_t audit_core;
static uint32_t audit_nesting;
static uint32_t audit_saved_mask;
static uint32_t audit_migrate_on_mask;
static uint32_t audit_migrate_on_core;
static uint32_t audit_stack_calls;
static uint32_t audit_fail_stack_call;
static uint32_t audit_tick_init_calls;
static bool audit_expect_trap;
static bool audit_expect_start;

OS_TASK_DEFINE(audit_caller, 256U);
OS_TASK_DEFINE(audit_peer, 256U);
OS_TASK_DEFINE(audit_owner, 256U);
OS_TASK_DEFINE(audit_outer, 256U);
OS_TASK_DEFINE(audit_waiter, 256U);
OS_TASK_DEFINE(audit_second, 256U);
static os_mutex_t audit_mutex;
static os_mutex_t audit_outer_mutex;

__attribute__((noinline)) void audit_stop(void)
{
    __asm volatile("bkpt #0");
    for (;;) { }
}

#define AUDIT_CHECK(condition) do { if (!(condition)) { audit_result = __LINE__; audit_stop(); } } while (0)

/*
 * ***********************************************************************************************************
 * Function implementations
 * ***********************************************************************************************************
*/

static void audit_migrate(void)
{
    os_task_tcb_t *previous = os_task_current[0];
    os_task_current[0] = os_task_current[1];
    os_task_current[1] = previous;
    os_task_current[0]->running_core = 0U;
    os_task_current[1]->running_core = 1U;
    audit_core = 1U;
}

uint32_t os_arch_kernel_mask_save(void)
{
    uint32_t previous = audit_mask;
    if ((audit_mask == 0U) && (audit_migrate_on_mask != 0U))
    {
        audit_migrate_on_mask = 0U;
        audit_migrate();
    }
    audit_mask = 1U;
    return previous;
}

void os_arch_kernel_mask_restore(uint32_t mask)
{
    audit_mask = mask;
}

uint32_t os_arch_core_id_get(void)
{
    uint32_t result = audit_core;
    if ((audit_mask == 0U) && (audit_migrate_on_core != 0U))
    {
        audit_migrate_on_core = 0U;
        audit_migrate();
    }
    return result;
}

bool os_arch_in_isr(void)
{
    return false;
}
uint32_t os_arch_highest_bit_get(uint32_t bits)
{
    return 31U - (uint32_t)__builtin_clz(bits);
}
uint32_t os_arch_lowest_bit_get(uint32_t bits)
{
    return (uint32_t)__builtin_ctz(bits);
}
void os_arch_init(void)
{
     
}
void os_arch_tick_init(void)
{
     
}
void os_arch_core_ipi_request_cb(uint32_t core)
{
    (void)core;
}
void os_arch_core_launch_cb(uint32_t core)
{
    (void)core;
}
void os_main(void)
{
     
}
uint32_t os_tick_get(void)
{
    return 0U;
}
void os_tick_init(void)
{
    audit_tick_init_calls++;
}

void os_arch_config_fault_trap(void)
{
    AUDIT_CHECK(audit_expect_trap);
    AUDIT_CHECK(!os_kernel_running && !os_kernel_initialized);
    AUDIT_CHECK(audit_tick_init_calls == 0U);
    audit_result = 0U;
    audit_stop();
}

void os_arch_start_first_task(void)
{
    AUDIT_CHECK(audit_expect_start);
    AUDIT_CHECK(os_kernel_running && os_kernel_initialized);
    AUDIT_CHECK(os_task_idle_is_created());
    AUDIT_CHECK(audit_tick_init_calls == 1U);
    audit_result = 0U;
    audit_stop();
}

uint32_t *os_arch_task_stack_initialize(uint8_t *stack, size_t bytes,
                                      void (*entry)(void *), void *context)
{
    (void)entry;
    (void)context;
    audit_stack_calls++;
    return (audit_stack_calls == audit_fail_stack_call) ? NULL :
           (uint32_t *)(void *)(stack + bytes - 64U);
}

void os_critical_enter(void)
{
    uint32_t previous = os_arch_kernel_mask_save();
    if (audit_nesting == 0U) { audit_saved_mask = previous; }
    audit_nesting++;
}

void os_critical_exit(void)
{
    AUDIT_CHECK(audit_nesting != 0U);
    audit_nesting--;
    if (audit_nesting == 0U) { os_arch_kernel_mask_restore(audit_saved_mask); }
}

void os_critical_multicore_lock(void)
{
    AUDIT_CHECK(audit_mask != 0U);
}
void os_critical_multicore_unlock(void)
{
    AUDIT_CHECK(audit_mask != 0U);
}

static void audit_entry(void *context)
{
    (void)context;
}

static os_task_tcb_t *audit_create(os_task_t *handle, uint32_t priority)
{
    os_task_config_t config = { audit_entry, NULL, priority, OS_TASK_CORE_ANY };
    AUDIT_CHECK(os_task_create(handle, &config) == OS_ERR_NONE);
    return os_task_find_by_id(handle->id);
}

static void audit_run(os_task_t *handle)
{
    os_task_tcb_t *tcb = os_task_find_by_id(handle->id);
    os_task_tcb_t *previous = os_task_current[audit_core];
    if ((previous != NULL) && (previous->state == OS_TASK_STATE_RUNNING))
    {
        previous->state = OS_TASK_STATE_SUSPENDED;
        previous->running_core = OS_CONFIG_CORE_COUNT;
    }
    os_task_current[audit_core] = tcb;
    tcb->state = OS_TASK_STATE_RUNNING;
    tcb->running_core = audit_core;
}

static void audit_block(os_task_t *handle, os_mutex_t *mutex, uint32_t ticks)
{
    audit_run(handle);
    os_critical_enter();
    os_task_mutex_priority_inherit(mutex->owner_id);
    os_task_mutex_blocked_on_set(mutex, ticks == OS_WAIT_FOREVER);
    os_task_wait_begin(&mutex->waiters, ticks);
    os_task_current[audit_core]->running_core = OS_CONFIG_CORE_COUNT;
    os_task_current[audit_core] = NULL;
    os_critical_exit();
}

void audit_migration(void)
{
    os_task_system_init();
    os_task_tcb_t *caller = audit_create(&audit_caller, 10U);
    os_task_tcb_t *peer = audit_create(&audit_peer, 12U);
    caller->state = peer->state = OS_TASK_STATE_RUNNING;
    caller->running_core = 0U;
    peer->running_core = 1U;
    os_task_current[0] = caller;
    os_task_current[1] = peer;

    audit_migrate_on_mask = 1U;
    AUDIT_CHECK(os_task_delete(&audit_peer) == OS_ERR_BUSY);
    AUDIT_CHECK(os_task_current[0] == peer && os_task_current[1] == caller);
    AUDIT_CHECK(peer->state == OS_TASK_STATE_RUNNING && peer->id == audit_peer.id);
    AUDIT_CHECK(os_task_pause(&audit_peer) == OS_ERR_BUSY);

    /* Return to core 0; a port hook migrates only if a core read occurs without a mask. */
    os_task_current[0] = caller;
    os_task_current[1] = peer;
    caller->running_core = audit_core = 0U;
    peer->running_core = 1U;
    caller->wait_signaled = true;
    caller->wait_result = 123U;
    peer->wait_signaled = false;
    peer->wait_result = 456U;
    audit_migrate_on_core = 1U;
    AUDIT_CHECK(os_task_current_id_get() == audit_caller.id);
    AUDIT_CHECK(os_task_wait_signaled());
    AUDIT_CHECK(os_task_wait_result_get() == 123U);
    AUDIT_CHECK(os_mutex_lock(&audit_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE);
    AUDIT_CHECK(audit_mutex.owner_id == audit_caller.id);
    AUDIT_CHECK(caller->owned_mutexes.head == &audit_mutex.owner_node);
    AUDIT_CHECK(os_mutex_unlock(&audit_mutex) == OS_ERR_NONE);
    audit_migrate_on_core = 0U;
    audit_migrate_on_mask = 1U;
    AUDIT_CHECK(os_task_pause(&audit_peer) == OS_ERR_BUSY);
    AUDIT_CHECK(peer->state == OS_TASK_STATE_RUNNING);
    audit_result = 0U;
    audit_stop();
}

void audit_inheritance(void)
{
    os_task_system_init();
    os_task_tcb_t *owner = audit_create(&audit_owner, 20U);
    os_task_tcb_t *outer = audit_create(&audit_outer, 8U);
    os_task_tcb_t *waiter = audit_create(&audit_waiter, 15U);
    os_task_tcb_t *second = audit_create(&audit_second, 12U);

    audit_run(&audit_owner);
    AUDIT_CHECK(os_mutex_lock(&audit_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE);
    audit_block(&audit_waiter, &audit_mutex, 3U);
    AUDIT_CHECK(owner->priority == 20U);
    AUDIT_CHECK(os_task_priority_set(&audit_owner, OS_TASK_PRIO_5) == OS_ERR_NONE);
    AUDIT_CHECK(owner->base_priority == 5U && owner->priority == 15U);

    audit_run(&audit_outer);
    AUDIT_CHECK(os_mutex_lock(&audit_outer_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE);
    audit_block(&audit_owner, &audit_outer_mutex, OS_WAIT_FOREVER);
    AUDIT_CHECK(outer->priority == 15U);
    AUDIT_CHECK(os_task_priority_set(&audit_waiter, OS_TASK_PRIO_25) == OS_ERR_NONE);
    AUDIT_CHECK(owner->priority == 25U && outer->priority == 25U);
    AUDIT_CHECK(os_task_priority_set(&audit_waiter, OS_TASK_PRIO_10) == OS_ERR_NONE);
    AUDIT_CHECK(owner->priority == 10U && outer->priority == 10U);

    audit_block(&audit_second, &audit_mutex, OS_WAIT_FOREVER);
    AUDIT_CHECK(owner->priority == 12U && outer->priority == 12U);
    AUDIT_CHECK(audit_mutex.waiters.head == &second->wait_node);
    AUDIT_CHECK(os_task_priority_set(&audit_waiter, OS_TASK_PRIO_30) == OS_ERR_NONE);
    AUDIT_CHECK(audit_mutex.waiters.head == &waiter->wait_node);
    AUDIT_CHECK(owner->priority == 30U && outer->priority == 30U);

    /* Revocation must happen in the timeout handler before the timed-out task is dispatched. */
    os_task_tick_update(3U);
    AUDIT_CHECK(waiter->state == OS_TASK_STATE_READY);
    AUDIT_CHECK(waiter->pi_owner_id == 0U && waiter->blocked_on_mutex == NULL);
    AUDIT_CHECK(owner->priority == 12U && outer->priority == 12U);
    AUDIT_CHECK(os_task_pause(&audit_second) == OS_ERR_NONE);
    AUDIT_CHECK(owner->priority == 5U && outer->priority == 8U);
    AUDIT_CHECK(second->pi_owner_id == 0U && second->blocked_on_mutex == NULL);
    AUDIT_CHECK(os_task_start(&audit_owner) == OS_ERR_NONE);
    AUDIT_CHECK(owner->blocked_on_mutex == NULL && owner->pi_owner_id == 0U);
    audit_result = 0U;
    audit_stop();
}

static void audit_owner_transition(bool reprioritize)
{
    os_task_system_init();
    os_task_tcb_t *owner = audit_create(&audit_owner, 5U);
    os_task_tcb_t *new_owner = audit_create(&audit_peer, 2U);
    os_task_tcb_t *highest = audit_create(&audit_caller, 25U);
    os_task_tcb_t *timed = audit_create(&audit_waiter, 20U);
    (void)audit_create(&audit_second, 15U);
    (void)audit_create(&audit_outer, 10U);

    audit_run(&audit_owner);
    AUDIT_CHECK(os_mutex_lock(&audit_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE);
    audit_block(&audit_caller, &audit_mutex, OS_WAIT_FOREVER);
    audit_block(&audit_waiter, &audit_mutex, 3U);
    audit_block(&audit_second, &audit_mutex, OS_WAIT_FOREVER);
    audit_block(&audit_outer, &audit_mutex, OS_WAIT_FOREVER);
    AUDIT_CHECK(owner->priority == 25U);
    audit_run(&audit_owner);
    AUDIT_CHECK(os_mutex_unlock(&audit_mutex) == OS_ERR_NONE);
    AUDIT_CHECK(highest->state == OS_TASK_STATE_READY && owner->priority == 5U);

    /* A peer core can acquire the free mutex before the signaled task retries. */
    audit_core = 1U;
    audit_run(&audit_peer);
    AUDIT_CHECK(os_mutex_lock(&audit_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE);
    if (reprioritize)
    {
        /* Exercise departure independently of acquisition: the existing base-change path
         * recomputes the owner even on versions missing the acquisition recompute. */
        AUDIT_CHECK(os_task_priority_set(&audit_peer, OS_TASK_PRIO_2) == OS_ERR_NONE);
    }
    AUDIT_CHECK(new_owner->priority == 20U);

    audit_core = 0U;
    os_task_tick_update(3U);
    AUDIT_CHECK(timed->state == OS_TASK_STATE_READY && timed->blocked_on_mutex == NULL);
    AUDIT_CHECK(new_owner->priority == 15U && owner->priority == 5U);
    AUDIT_CHECK(os_task_pause(&audit_second) == OS_ERR_NONE);
    AUDIT_CHECK(new_owner->priority == 10U);
    AUDIT_CHECK(os_task_delete(&audit_outer) == OS_ERR_NONE);
    AUDIT_CHECK(new_owner->priority == 2U && owner->priority == 5U);
    audit_result = 0U;
    audit_stop();
}

void audit_new_owner_inheritance(void)
{
    audit_owner_transition(false);
}
void audit_new_owner_departure(void)
{
    audit_owner_transition(true);
}

void audit_idle_partial(void)
{
    os_task_system_init();
    audit_fail_stack_call = 2U;
    AUDIT_CHECK(os_task_idle_create() == OS_ERR_ERROR);
    AUDIT_CHECK(!os_task_idle_is_created());
    audit_fail_stack_call = 0U;
    AUDIT_CHECK(os_task_idle_create() == OS_ERR_NONE);
    AUDIT_CHECK(os_task_idle_is_created());
    AUDIT_CHECK(audit_stack_calls == 3U);
    audit_result = 0U;
    audit_stop();
}

void audit_boot_success(void)
{
    audit_expect_start = true; os_init(); os_start();
}
void audit_boot_idle_failure(void)
{
    audit_expect_trap = true; audit_fail_stack_call = 1U; os_init(); audit_result = __LINE__; audit_stop();
}
void audit_boot_secondary_failure(void)
{
    audit_expect_trap = true; audit_fail_stack_call = 2U; os_init(); audit_result = __LINE__; audit_stop();
}
void audit_boot_main_failure(void)
{
    audit_expect_trap = true; audit_fail_stack_call = 3U; os_init(); audit_result = __LINE__; audit_stop();
}
void audit_start_before_init(void)
{
    audit_expect_trap = true; os_start(); audit_result = __LINE__; audit_stop();
}
