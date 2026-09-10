/**
 * @file regression.c
 * @brief Execute message/log source with deterministic scheduler substitutions.
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */
/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "kernel/os_internal.h"
#include <string.h>

/* Include implementations so the log consumer can be exercised directly. The
 * scheduler and critical-section substitutions below inject specific schedules;
 * this harness does not test context switching or simultaneous CPU execution. */
#include "kernel/os_list.c"
#include "kernel/os_msg.c"
#include "kernel/os_log.c"

typedef struct
{
    os_list_node_t node;
    os_list_t *list;
    uint32_t needed;
    uint32_t timeout_ticks;
    bool signaled;
} test_waiter_t;

__IO uint32_t os_kernel_lock_count[OS_CONFIG_CORE_COUNT];
__IO bool os_kernel_switch_pending[OS_CONFIG_CORE_COUNT];
__IO bool os_kernel_running = true;

static test_waiter_t test_waiters[4];
static uint32_t test_current;
static uint32_t test_depth;
static uint32_t test_tick;
static uint32_t test_seed_tick;
static void (*test_switch_action)(void);
volatile uint32_t test_failure;
static os_msg_t *test_buffer;
static size_t test_second_size;
static os_err_t test_second_status;
static os_err_t test_seed_receive_status;
static uint8_t test_data[30];
static uint32_t test_report_count;
static bool test_inject_log_drop;

#define TEST_CHECK(condition)                               \
    do                                                      \
    {                                                       \
        if (!(condition) && (test_failure == 0U))           \
        {                                                   \
            test_failure = __LINE__;                        \
        }                                                   \
    } while (0)

/*
 * ***********************************************************************************************************
 * Function implementations
 * ***********************************************************************************************************
*/

void os_critical_enter(void)
{
    test_depth++;
}

void os_critical_exit(void)
{
    TEST_CHECK(test_depth > 0U);
    test_depth--;
    if ((test_depth == 0U) && (test_switch_action != NULL))
    {
        void (*action)(void) = test_switch_action;
        test_switch_action = NULL;
        action();
    }
}

uint32_t os_tick_get(void)
{
    return test_tick;
}

void os_task_wait_data_set(uint32_t data0, uint32_t data1)
{
    TEST_CHECK(test_depth > 0U);
    TEST_CHECK(data1 == 0U);
    test_waiters[test_current].needed = data0;
}

void os_task_wait_begin(os_list_t *waiters, uint32_t ticks)
{
    test_waiter_t *waiter = &test_waiters[test_current];
    TEST_CHECK(test_depth > 0U);
    TEST_CHECK(ticks != 0U);
    TEST_CHECK(waiter->list == NULL);
    waiter->list = waiters;
    waiter->timeout_ticks = ticks;
    waiter->signaled = false;
    os_list_push_back(waiters, &waiter->node);
}

void os_task_wait_end(void)
{
    test_waiter_t *waiter = &test_waiters[test_current];
    if (waiter->list != NULL)
    {
        /* An unsignaled resume models timeout expiry and removes the node. */
        os_list_remove(waiter->list, &waiter->node);
        waiter->list = NULL;
    }
    waiter->signaled = false;
}

bool os_task_wait_signaled(void)
{
    return test_waiters[test_current].signaled;
}

static void test_wake(test_waiter_t *waiter)
{
    os_list_remove(waiter->list, &waiter->node);
    waiter->list = NULL;
    waiter->signaled = true;
}

bool os_task_waiters_wake_one(os_list_t *waiters)
{
    bool woke = waiters->head != NULL;
    TEST_CHECK(test_depth > 0U);
    if (woke)
    {
        test_wake((test_waiter_t *)waiters->head);
    }
    return woke;
}

uint32_t os_task_waiters_wake_match(os_list_t *waiters, os_task_wait_match_fn match, void *context)
{
    os_list_node_t *node = waiters->head;
    uint32_t count = 0U;
    TEST_CHECK(test_depth > 0U);
    while (node != NULL)
    {
        test_waiter_t *waiter = (test_waiter_t *)node;
        os_list_node_t *next = node->next;
        uint32_t result = 99U;
        if (match(waiter->needed, 0U, context, &result))
        {
            TEST_CHECK(result == 0U);
            test_wake(waiter);
            count++;
        }
        node = next;
    }
    return count;
}

static void test_receive_seed(void)
{
    uint8_t output[30];
    size_t length;
    uint32_t saved = test_current;
    test_current = 2U;
    test_tick = test_seed_tick;
    test_seed_receive_status = os_msg_receive(test_buffer, output, sizeof(output), &length, OS_WAIT_NOTHING);
    test_current = saved;
}

static void test_start_second(void)
{
    test_current = 1U;
    test_switch_action = test_receive_seed;
    test_second_status = os_msg_send(test_buffer, test_data, test_second_size, OS_WAIT_FOREVER);
    test_current = 0U;
}

static void test_reset(void)
{
    (void)memset(test_waiters, 0, sizeof(test_waiters));
    test_current = 0U;
    test_depth = 0U;
    test_tick = 0U;
    test_seed_tick = 0U;
    test_switch_action = NULL;
    test_second_status = OS_ERR_ERROR;
    test_seed_receive_status = OS_ERR_ERROR;
}

uint32_t test_two_senders(void)
{
    uint8_t storage[32];
    os_msg_t msg = OS_MSG_INITIALIZER(storage);
    test_reset();
    test_buffer = &msg;
    test_second_size = 1U;
    TEST_CHECK(os_msg_send(&msg, test_data, 30U, OS_WAIT_NOTHING) == OS_ERR_NONE);
    test_switch_action = test_start_second;
    TEST_CHECK(os_msg_send(&msg, test_data, 1U, OS_WAIT_FOREVER) == OS_ERR_NONE);
    TEST_CHECK(test_second_status == OS_ERR_NONE);
    TEST_CHECK(test_seed_receive_status == OS_ERR_NONE);
    TEST_CHECK(msg.count == 2U);
    TEST_CHECK(msg.used == 6U);
    TEST_CHECK(msg.send_waiters.head == NULL);
    TEST_CHECK(test_waiters[0].needed == 3U);
    TEST_CHECK(test_waiters[1].needed == 3U);
    return test_failure;
}

uint32_t test_eligible_sender(void)
{
    uint8_t storage[32];
    os_msg_t msg = OS_MSG_INITIALIZER(storage);
    test_reset();
    test_buffer = &msg;
    test_second_size = 1U;
    TEST_CHECK(os_msg_send(&msg, test_data, 2U, OS_WAIT_NOTHING) == OS_ERR_NONE);
    TEST_CHECK(os_msg_send(&msg, test_data, 26U, OS_WAIT_NOTHING) == OS_ERR_NONE);
    test_switch_action = test_start_second;
    TEST_CHECK(os_msg_send(&msg, test_data, 20U, 10U) == OS_ERR_TIMEOUT);
    TEST_CHECK(test_second_status == OS_ERR_NONE);
    TEST_CHECK(test_seed_receive_status == OS_ERR_NONE);
    TEST_CHECK(msg.used == 31U);
    TEST_CHECK(msg.count == 2U);
    TEST_CHECK(msg.send_waiters.head == NULL);
    return test_failure;
}

uint32_t test_competing_senders(void)
{
    uint8_t storage[32];
    os_msg_t msg = OS_MSG_INITIALIZER(storage);
    test_reset();
    test_buffer = &msg;
    test_second_size = 20U;
    test_seed_tick = 7U;
    TEST_CHECK(os_msg_send(&msg, test_data, 30U, OS_WAIT_NOTHING) == OS_ERR_NONE);
    test_switch_action = test_start_second;
    /* Both fit independently when woken. The second commits first; the first
     * must recheck and block again rather than corrupting the ring. */
    TEST_CHECK(os_msg_send(&msg, test_data, 20U, 10U) == OS_ERR_TIMEOUT);
    TEST_CHECK(test_second_status == OS_ERR_NONE);
    TEST_CHECK(msg.count == 1U);
    TEST_CHECK(msg.used == 22U);
    TEST_CHECK(msg.send_waiters.head == NULL);
    TEST_CHECK(test_waiters[0].timeout_ticks == 3U);
    return test_failure;
}

uint32_t test_maximum_sender_metadata(void)
{
    static uint8_t storage[OS_MSG_SPACE(OS_MSG_LENGTH_MAX)];
    static uint8_t payload[OS_MSG_LENGTH_MAX];
    os_msg_t msg = OS_MSG_INITIALIZER(storage);
    test_reset();
    TEST_CHECK(os_msg_send(&msg, payload, sizeof(payload), OS_WAIT_NOTHING) == OS_ERR_NONE);
    TEST_CHECK(os_msg_send(&msg, payload, sizeof(payload), 10U) == OS_ERR_TIMEOUT);
    TEST_CHECK(test_waiters[0].needed == 65537U);
    TEST_CHECK(msg.used == sizeof(storage));
    TEST_CHECK(msg.count == 1U);
    return test_failure;
}

uint32_t test_receivers(void)
{
    uint8_t storage[32];
    uint8_t output[30];
    size_t length;
    os_msg_t msg = OS_MSG_INITIALIZER(storage);
    test_reset();
    os_critical_enter();
    test_current = 1U;
    os_task_wait_begin(&msg.receive_waiters, OS_WAIT_FOREVER);
    test_current = 2U;
    os_task_wait_begin(&msg.receive_waiters, OS_WAIT_FOREVER);
    test_current = 0U;
    os_critical_exit();
    TEST_CHECK(os_msg_send(&msg, test_data, 5U, OS_WAIT_NOTHING) == OS_ERR_NONE);
    TEST_CHECK(test_waiters[1].signaled);
    TEST_CHECK(!test_waiters[2].signaled);
    test_current = 1U;
    TEST_CHECK(os_msg_receive(&msg, output, 1U, &length, OS_WAIT_FOREVER) == OS_ERR_INVALID_ARG);
    TEST_CHECK(length == 5U);
    TEST_CHECK(msg.count == 1U);
    TEST_CHECK(test_waiters[2].signaled);
    test_current = 2U;
    TEST_CHECK(os_msg_receive(&msg, output, sizeof(output), &length, OS_WAIT_FOREVER) == OS_ERR_NONE);
    TEST_CHECK(length == 5U);
    TEST_CHECK(msg.count == 0U);
    return test_failure;
}

uint32_t test_remaining_messages(void)
{
    uint8_t storage[32];
    uint8_t output[30];
    size_t length;
    os_msg_t msg = OS_MSG_INITIALIZER(storage);
    test_reset();
    os_critical_enter();
    for (test_current = 0U; test_current < 3U; test_current++)
    {
        os_task_wait_begin(&msg.receive_waiters, OS_WAIT_FOREVER);
    }
    test_current = 3U;
    os_critical_exit();
    TEST_CHECK(os_msg_send(&msg, test_data, 1U, OS_WAIT_NOTHING) == OS_ERR_NONE);
    TEST_CHECK(os_msg_send(&msg, test_data, 2U, OS_WAIT_NOTHING) == OS_ERR_NONE);
    TEST_CHECK(!test_waiters[2].signaled);
    test_current = 0U;
    TEST_CHECK(os_msg_receive(&msg, output, sizeof(output), &length, OS_WAIT_FOREVER) == OS_ERR_NONE);
    TEST_CHECK(length == 1U);
    TEST_CHECK(test_waiters[2].signaled);
    test_current = 2U;
    TEST_CHECK(os_msg_receive(&msg, output, sizeof(output), &length, OS_WAIT_FOREVER) == OS_ERR_NONE);
    TEST_CHECK(length == 2U);
    TEST_CHECK(msg.count == 0U);
    return test_failure;
}

static bool test_contains(const uint8_t *data, size_t length, const char *text)
{
    size_t text_length = strlen(text);
    bool found = false;
    for (size_t i = 0U; (i + text_length) <= length; i++)
    {
        if (memcmp(&data[i], text, text_length) == 0)
        {
            found = true;
        }
    }
    return found;
}

void os_log_output_cb(const uint8_t *data, size_t length)
{
    static const char full[OS_CONFIG_LOG_BUFFER_SIZE - 1U] = {0};
    TEST_CHECK(test_depth == 0U);
    if (test_contains(data, length, "log lines dropped"))
    {
        test_report_count++;
        if (test_report_count == 1U)
        {
            TEST_CHECK(test_contains(data, length, "2 log lines dropped"));
            TEST_CHECK(os_log_dropped_get() == 2U);
            if (test_inject_log_drop)
            {
                os_log_queue(full, sizeof(full));
                os_log_queue("X", 1U);
            }
        }
        else
        {
            TEST_CHECK(test_report_count == 2U);
            TEST_CHECK(test_contains(data, length, "1 log lines dropped"));
        }
    }
}

os_err_t os_task_create_system(os_task_t *task, const os_task_config_t *config)
{
    (void)config;
    task->id = 1U;
    return OS_ERR_NONE;
}

os_err_t os_task_start(os_task_t *task)
{
    (void)task;
    return OS_ERR_NONE;
}

void *os_task_tcb_resolve(uint32_t id)
{
    (void)id;
    return NULL;
}

void os_task_wake_tcb(void *tcb)
{
    (void)tcb;
}

void os_task_sleep_ticks(uint32_t ticks)
{
    TEST_CHECK(ticks == OS_WAIT_FOREVER);
    TEST_CHECK(test_report_count == 2U);
    TEST_CHECK(os_log_dropped_get() == 3U);
    __asm volatile("bkpt #0");
}

uint32_t test_log_drain(void)
{
    static const char full[OS_CONFIG_LOG_BUFFER_SIZE - 1U] = {0};
    test_reset();
    test_report_count = 0U;
    test_inject_log_drop = true;
    TEST_CHECK(os_log_system_init() == OS_ERR_NONE);
    os_log_queue(full, sizeof(full));
    os_log_queue("X", 1U);
    os_log_queue("Y", 1U);
    TEST_CHECK(os_log_dropped_get() == 2U);
    os_log_task_entry(NULL);
    return test_failure;
}

uint32_t test_log_notice_full_ring(void)
{
    static const char full[OS_CONFIG_LOG_BUFFER_SIZE - 1U] = {0};
    test_reset();
    test_report_count = 0U;
    test_inject_log_drop = false;
    TEST_CHECK(os_log_system_init() == OS_ERR_NONE);
    os_log_queue(full, sizeof(full));
    os_log_queue("X", 1U);
    os_log_queue("Y", 1U);
    /* Models a producer filling the ring after the consumer took a drop-count
     * snapshot, before it formats/emits the notice. The notice must survive. */
    os_log_emit_dropped(2U);
    TEST_CHECK(test_report_count == 1U);
    TEST_CHECK(os_log_dropped_get() == 2U);
    return test_failure;
}
