/**
 * @file os_test.c
 * @brief Boot-time self-test suite for the Ahura RTOS kernel.
 *
 * Supplies os_test() (declared in ahura.h, defined nowhere else in the kernel; not named with the
 * "_cb" suffix - this is where the suite's own code runs, not a kernel query for platform
 * behavior): link this file's library (ahura_kernel/test, target "os_test") and, when
 * OS_CONFIG_TEST_ENABLE is 1, os_init() creates a task that calls this automatically - no explicit
 * call needed from the application. Runs once, exercises whichever OS_CONFIG_<FEATURE>_ENABLE
 * switches are on, and
 * prints a detailed PASS/FAIL log via printf, finishing with a pass/fail summary. Depends on
 * nothing but ahura.h - no board/HAL headers - so it runs on any arch/board the kernel supports;
 * printf's destination (typically a UART) is the linking application's concern.
 */

#include "ahura.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

/*
 * ***********************************************************************************************************
 * Test bookkeeping
 * ***********************************************************************************************************
*/

static uint32_t os_test_pass_count = 0U;
static uint32_t os_test_fail_count = 0U;

#define AHURA_TEST_CHECK(cond, fmt, ...) \
    do { \
        if (cond) { os_test_pass_count++; printf("  [PASS] " fmt "\r\n", ##__VA_ARGS__); } \
        else      { os_test_fail_count++; printf("  [FAIL] " fmt "  (os_test.c:%d)\r\n", ##__VA_ARGS__, __LINE__); } \
    } while (0)

/******************************************************************************************************/
static void test_print_section(const char *title)
{
    printf("\r\n--- %s ---\r\n", title);
}

/*
 * ***********************************************************************************************************
 * Shared kernel objects under test
 * ***********************************************************************************************************
*/

OS_TASK_DEFINE(worker, 512U);
OS_TASK_DEFINE(helper, 512U);
/* Two more concurrent task slots for the combined-scenario tests below, which run 3-4 tasks
 * at once (single-primitive tests above only ever run one helper at a time). */
OS_TASK_DEFINE(helper2, 512U);
OS_TASK_DEFINE(helper3, 512U);

static __IO uint32_t os_test_worker_counter    = 0U;
static __IO bool     os_test_worker_should_run = true;

/* Shared between test_priority_preemption() and test_cpu_usage(): a task that spins
 * incrementing this counter, without ever yielding/delaying, so it only runs on ticks
 * nothing higher-priority is ready for. */
static __IO uint32_t os_test_busy_counter    = 0U;
static __IO bool     os_test_busy_should_run = true;

/* test_scheduler_lock(): set by a task that outranks the test task, so the flag can only
 * turn true once the scheduler is actually allowed to switch. */
static __IO bool     os_test_sched_lock_ran  = false;
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
static os_semaphore_t os_test_sched_lock_sem;  /* left empty: a take would have to block */
#endif

#define TEST_BURST_ITERATIONS 200000UL

/*
 * Helper priorities, expressed relative to the test task rather than hardcoded.
 *
 * Several tests need a task that provably cannot run while the test task is runnable (LOW), or
 * one that provably preempts it (HIGH). Spelling those as literal 1 and 3 silently stopped
 * meaning that the moment OS_CONFIG_TEST_PRIORITY was not 2: a LOW helper written as 1 became a
 * PEER of a test task at priority 1, the two round-robined, and "the spinner never advanced"
 * failed by hundreds of microseconds of spinner time with nothing in the test looking wrong.
 *
 * OS_TASK_PRIO_1_LOWEST is the floor for user tasks, so a test task sitting on it has no room
 * underneath at all. test_priority_preemption checks that requirement rather than clamping:
 * quietly nudging the priorities would keep the suite green while no longer testing preemption.
 *
 * Checked at run time, not with #error: OS_TASK_PRIO_* are enum constants rather than macros, so
 * the preprocessor cannot see their values at all. It substitutes 0 for the unknown identifier and
 * compares that instead, which makes any #if arithmetic over them quietly meaningless - it fires
 * or stays silent for reasons unrelated to the configured priority.
 */
#define TEST_PRIO_LOW  (OS_CONFIG_TEST_PRIORITY - 1U)
#define TEST_PRIO_HIGH (OS_CONFIG_TEST_PRIORITY + 1U)

/*
 * Benchmarks are timed with the CPU cycle counter (os_arch_cycle_count_get), not the kernel
 * tick: the tick only resolves whole milliseconds, which is ~250000 cycles of quantization on a
 * fast core - far coarser than the calls being measured. Cycle resolution is 1 cycle.
 *
 * Each operation is measured ON ITS OWN, sampled many times, and the MINIMUM is kept. Anything
 * that perturbs a sample (the 1 kHz tick ISR landing mid-measurement, a flash/cache miss, a
 * pipeline stall) only ever ADDS cycles, so the minimum converges on the true uninterrupted
 * cost. The two cycle-counter reads have their own cost, measured the same way and subtracted.
 */
#define TEST_BENCH_SAMPLES         2000U
#define TEST_BENCH_HEAVY_SAMPLES   200U

/* Saturating subtract: an operation cheaper than the measurement overhead itself would
 * otherwise wrap to a huge unsigned value. */
#define TEST_BENCH_SUB(total, over) (((total) > (over)) ? ((total) - (over)) : 0U)

/* Sample one operation TEST_BENCH_SAMPLES times, keeping the cheapest run. The statement is
 * pasted inline (not called through a function pointer) so no call overhead is attributed to
 * it; the compiler cannot reorder it across the two counter reads because all three are
 * external calls into the kernel library. */
#define TEST_BENCH_MIN_CYCLES(best_out, samples, op_stmt)                        \
    do {                                                                         \
        uint32_t bench_best = UINT32_MAX;                                        \
        uint32_t bench_i;                                                        \
        for (bench_i = 0U; bench_i < (samples); bench_i++)                       \
        {                                                                        \
            uint32_t bench_c0 = os_arch_cycle_count_get();                       \
            op_stmt;                                                             \
            uint32_t bench_d = os_arch_cycle_count_get() - bench_c0;             \
            if (bench_d < bench_best) { bench_best = bench_d; }                  \
        }                                                                        \
        (best_out) = bench_best;                                                 \
    } while (0)

/* Dedicated benchmark objects, kept separate from the functional tests' shared ones so a
 * leftover count/item/waiter from an earlier section cannot skew a measurement. */
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_mutex_t     os_test_bench_mutex;
#endif
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
static os_semaphore_t os_test_bench_sem;
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
static uint32_t       os_test_bench_queue_buf[4];
OS_QUEUE_DEFINE_BUFFER(os_test_bench_queue, os_test_bench_queue_buf);
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
static os_event_t os_test_bench_event;
#endif

/* Shared between two equal-priority tasks in test_context_switch_timing(): each increments
 * this once per loop turn, then yields - so its total over a fixed window is (approximately)
 * the number of context switches that occurred. */
static __IO uint32_t os_test_switch_count      = 0U;
static __IO bool     os_test_switch_should_run = true;

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_mutex_t os_test_mutex;
#endif

/* test_spawn_helper drives the mutex, semaphore, queue and event sections, so it has to exist
 * whenever any one of them is compiled in. Guarding it on a single feature left the other three
 * calling an undeclared function. */
#define TEST_HELPER_NEEDED ((OS_CONFIG_SEMAPHORE_ENABLE == 1U) || (OS_CONFIG_MUTEX_ENABLE == 1U) || \
                            (OS_CONFIG_QUEUE_ENABLE == 1U)     || (OS_CONFIG_EVENT_ENABLE == 1U))

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
static os_semaphore_t os_test_bin_sem;
static os_semaphore_t os_test_count_sem;
#endif

/* Every use of this one - the give in test_helper_entry and the init/take in test_mutex - sits
 * behind both switches, so defining it on the semaphore switch alone left it unused whenever
 * mutexes were compiled out. */
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_semaphore_t os_test_sync_sem;   /* helper -> main "ready" signal */
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
/* Declared with its own array rather than by OS_QUEUE_DEFINE_STATIC, so the suite covers
 * OS_QUEUE_DEFINE_BUFFER too. Tests reset it with os_queue_cleanup(), which empties a queue
 * without touching storage it does not own. */
static uint32_t   os_test_queue_buf[3];
OS_QUEUE_DEFINE_BUFFER(os_test_queue, os_test_queue_buf);
#endif

#if (OS_CONFIG_EVENT_ENABLE == 1U)
static os_event_t os_test_event;
#endif

#if (OS_CONFIG_TIMER_ENABLE == 1U)
static os_timer_t        os_test_timer_oneshot;
static os_timer_t        os_test_timer_periodic;
static __IO uint32_t os_test_oneshot_fired  = 0U;
static __IO uint32_t os_test_periodic_fired = 0U;
#endif

#if (OS_CONFIG_WORK_ENABLE == 1U)
static __IO bool     os_test_work_ran       = false;
static __IO uint32_t os_test_work_run_count = 0U;
#endif

#if (OS_CONFIG_LOG_ENABLE == 1U)
/* Capture buffer for test_log(): this file supplies os_log_output_cb - the kernel declares it and
 * defines nothing - so the log task hands its bytes here instead of to a UART. Kept small on
 * purpose - only the most recent output needs inspecting. */
/* Must hold everything a single drain can deliver after the capture is cleared: a full ring, plus
 * the dropped-lines notice tsk_log emits once that ring empties.
 *
 * Sized from the ring rather than fixed, because getting this wrong does not look like a capture
 * problem. At 512 bytes the flood test filled the capture with "flood ..." lines and silently
 * discarded the notice that arrived after them, so three checks failed as though the kernel had
 * never emitted it. */
#define TEST_LOG_CAPTURE_SIZE (OS_CONFIG_LOG_BUFFER_SIZE + 128U)

static char              os_test_log_capture[TEST_LOG_CAPTURE_SIZE];
static __IO size_t   os_test_log_capture_len      = 0U;
static __IO uint32_t os_test_log_capture_lines    = 0U;
static __IO bool     os_test_log_capture_on       = false;
static __IO bool     os_test_log_capture_overflow = false;
#endif

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
static __IO os_status os_test_notify_wait_status;
static __IO uint32_t  os_test_notify_wait_value;
static __IO uint32_t  os_test_notify_wait_ticks;
static __IO os_status os_test_notify_second_status;
static uint32_t           os_test_notify_wait_timeout_ms; /* set by the test before starting the waiter */
#endif

typedef enum
{
    HELPER_NONE = 0,
    HELPER_MUTEX_HOLD,
    HELPER_SEM_GIVE_AFTER,
    HELPER_EVENT_SET_AFTER,
    HELPER_QUEUE_SEND_AFTER,

} helper_role_t;

typedef struct
{
    helper_role_t role;
    uint32_t      hold_ms;
    uint32_t      bits;
    uint32_t      value;

} helper_ctx_t;

#if TEST_HELPER_NEEDED
/* Only test_spawn_helper writes it and only test_helper_entry reads it, so it follows their guard
 * (an unused static is a -Werror build failure; the two typedefs above are harmless either way). */
static helper_ctx_t os_test_helper_ctx;
#endif

/*
 * ***********************************************************************************************************
 * Combined-scenario context types and objects (see "Integration / Combined Scenarios" below)
 * ***********************************************************************************************************
 *
 * Unlike the single-primitive tests above (one helper task, one role at a time via
 * os_test_helper_ctx), these run several DIFFERENT tasks concurrently, each with its own behavior -
 * so each gets its own context struct, passed through OS_TASK_CONFIG's context pointer instead
 * of the shared dispatch-by-role pattern.
*/

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
#define TEST_PIPELINE_ITEMS_PER_PRODUCER 6U
#define TEST_PIPELINE_TOTAL_ITEMS        (2U * TEST_PIPELINE_ITEMS_PER_PRODUCER)

typedef struct
{
    uint32_t base_value;
    uint32_t count;

} test_producer_ctx_t;

static test_producer_ctx_t os_test_producer_ctx[2];
static os_mutex_t          os_test_pipeline_mutex;
static __IO uint32_t   os_test_pipeline_total;
static __IO uint32_t   os_test_pipeline_processed;
#endif

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
typedef struct
{
    uint32_t priority_tag;

} test_prio_ctx_t;

static test_prio_ctx_t   os_test_prio_ctx[3];
static os_mutex_t        os_test_prio_mutex;
static __IO uint32_t os_test_prio_order[3];
static __IO uint32_t os_test_prio_order_count;
#endif

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_mutex_t        os_test_inherit_mutex;
static __IO bool     os_test_inherit_high_done;
static __IO uint32_t os_test_inherit_medium_counter;

/* Two mutexes held at once by the same owner, each with its own higher-priority waiter -
 * see test_mutex_multi_inheritance(). */
typedef struct
{
    os_mutex_t *mutex;
    uint32_t   tag;   /* OR'd into os_test_inherit2_done_mask once this waiter is granted the mutex */

} test_inherit2_ctx_t;

static test_inherit2_ctx_t os_test_inherit2_ctx[2];
static os_mutex_t          os_test_inherit2_mutex_a;
static os_mutex_t          os_test_inherit2_mutex_b;
static __IO uint32_t   os_test_inherit2_done_mask;
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_EVENT_ENABLE == 1U)
typedef struct
{
    uint32_t bit;
    uint32_t value;
    uint32_t work_ms;

} test_fanin_ctx_t;

static test_fanin_ctx_t os_test_fanin_ctx[3];
#endif

#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
    (OS_CONFIG_EVENT_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
/* Concurrent multi-primitive stress/soak (see "Stress/Soak" below): unlike every scenario
 * above, which runs a small fixed handful of tasks each doing ONE thing, this runs
 * OS_TEST_STRESS_WORKER_COUNT tasks at distinct priorities that each hit a mutex, a
 * deliberately under-provisioned semaphore and queue, an event, and the kernel heap -
 * all at once, repeatedly, for many iterations, then check hard invariants instead of just
 * "the call returned OK". Bump OS_TEST_STRESS_ITERATIONS for a longer soak run; the default
 * is sized to add at most a couple of seconds to a boot-time log, not to replace a real
 * multi-hour soak. */
#define OS_TEST_STRESS_WORKER_COUNT   4U
#define OS_TEST_STRESS_ITERATIONS     300U
#define OS_TEST_STRESS_SEM_MAX        2U    /* < worker count: forces real blocking/timeouts */
#define OS_TEST_STRESS_QUEUE_CAPACITY 3U    /* < worker count: forces real FULL/EMPTY paths  */

typedef struct
{
    uint32_t worker_id;
    uint32_t prng_state; /* xorshift32 stream, seeded distinctly per worker; never 0 */

} test_stress_ctx_t;

static test_stress_ctx_t os_test_stress_ctx[OS_TEST_STRESS_WORKER_COUNT];
static __IO uint32_t os_test_stress_done[OS_TEST_STRESS_WORKER_COUNT];        /* iterations completed   */
static __IO uint32_t os_test_stress_mutex_hits[OS_TEST_STRESS_WORKER_COUNT];  /* successful mutex locks */
static __IO bool     os_test_stress_corrupt[OS_TEST_STRESS_WORKER_COUNT];    /* heap/queue corruption seen */
static size_t            os_test_stress_watermark[OS_TEST_STRESS_WORKER_COUNT];  /* self-reported stack watermark */

static os_mutex_t        os_test_stress_mutex;
static __IO uint32_t os_test_stress_shared_counter; /* protected exclusively by os_test_stress_mutex */

static os_semaphore_t    os_test_stress_sem;
static os_event_t  os_test_stress_event;
static uint32_t          os_test_stress_queue_buf[OS_TEST_STRESS_QUEUE_CAPACITY];
OS_QUEUE_DEFINE_BUFFER(os_test_stress_queue, os_test_stress_queue_buf);
#endif

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static bool      test_wait_inactive(const os_task_t *task, uint32_t timeout_ms);
static void      test_worker_entry(void *context);
static void      test_self_pause_worker_entry(void *context);
#if TEST_HELPER_NEEDED
static void      test_helper_entry(void *context);
static os_status test_spawn_helper(helper_role_t role, uint32_t hold_ms, uint32_t bits, uint32_t value);
#endif

static void test_kernel_core(void);
static void test_delay(void);
static void test_critical_section(void);
static void test_task_lifecycle(void);
static void test_task_identity(void);
static void test_priority_preemption(void);
static void test_sched_lock_entry(void *context);
static void test_scheduler_lock(void);
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static void test_mutex(void);
#endif
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
static void test_semaphore(void);
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
static void test_queue(void);
static void test_queue_define_and_dynamic(void);
#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
static void test_atomic(void);
#endif
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
static void test_event_group(void);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void test_timer(void);
#endif
#if (OS_CONFIG_WORK_ENABLE == 1U)
static void test_work(void);
#endif
static void test_assert(void);
static void test_log(void);
#if (OS_CONFIG_LOG_ENABLE == 1U)
static bool test_log_capture_contains(const char *needle);
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
static void test_notify_wait_entry(void *context);
static void test_notify_unrelated_block_entry(void *context);
static void test_notify_discard_entry(void *context);
static void test_task_notify(void);
#endif
#if (OS_CONFIG_ALLOC_ENABLE == 1U)
static void test_alloc(void);
#endif
#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
static void test_stack_watermark(void);
#endif
#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
static void test_cpu_usage(void);
#endif
static void test_task_footprint(void);
static void test_context_switch_timing(void);
static void test_bench_row(const char *name, uint32_t cycles, uint32_t clock_hz);
static void test_benchmarks(void);
static void test_tickless_hooks(void);
static void test_tickless_sleep(void);
static void test_list(void);
static void test_unsupported_features(void);
#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
static void test_pipeline_producer_entry(void *context);
static void test_pipeline_consumer_entry(void *context);
static void test_pipeline(void);
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static void test_prio_waiter_entry(void *context);
static void test_mutex_priority_ordering(void);
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static void test_inherit_high_entry(void *context);
static void test_inherit_medium_entry(void *context);
static void test_mutex_priority_inheritance(void);
static void test_inherit2_waiter_entry(void *context);
static void test_mutex_multi_inheritance(void);
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_EVENT_ENABLE == 1U)
static void test_fanin_worker_entry(void *context);
static void test_event_queue_fanin(void);
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
    (OS_CONFIG_EVENT_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
static uint32_t  test_stress_prng_next(uint32_t *state);
static void      test_stress_worker_entry(void *context);
static void      test_stress_soak(void);
#endif
static void      test_stress_task_churn(void);
#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void      test_stress_timer_churn(void);
#endif

/*
 * ***********************************************************************************************************
 * Shared helpers
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Poll (bounded) until a task reports INACTIVE, i.e. it has fully self-terminated
 *        and its stack is free to reuse for the next helper.
 */
#if (OS_CONFIG_LOG_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief The log transport for a self-test build: captures what tsk_log hands over, into a buffer
 *        the log tests can then search (test_log_capture_contains).
 *
 * The suite has to define this itself - there is no way to test the log without seeing what the
 * kernel actually emitted - which means the APPLICATION must not also define it in a test build,
 * or the two collide at link time. template/os_cb.c guards its copy with
 * (OS_CONFIG_TEST_ENABLE == 0U) for exactly this reason; a project whose os_cb.c predates that
 * guard needs the same condition adding.
 *
 * Note this captures only. The suite's own PASS/FAIL report goes to printf, not through here.
 *
 * Runs on tsk_log, outside any critical section, exactly as a real transport would.
 */
void os_log_output_cb(const uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0U; i < length; i++)
    {
        char c = (char)data[i];

        if (os_test_log_capture_on)
        {
            if (os_test_log_capture_len < (TEST_LOG_CAPTURE_SIZE - 1U))
            {
                os_test_log_capture[os_test_log_capture_len] = c;
                os_test_log_capture_len++;
            }
            else
            {
                /* Remember that bytes were thrown away. Without this, a capture that is too small
                 * makes the kernel look like it never emitted what the test is searching for. */
                os_test_log_capture_overflow = true;
            }
        }

        if (c == '\n')
        {
            os_test_log_capture_lines++;
        }
    }

    os_test_log_capture[os_test_log_capture_len] = '\0';
}

/******************************************************************************************************/
/**
 * @brief Whether the captured log output contains the given text.
 */
static bool test_log_capture_contains(const char *needle)
{
    os_test_log_capture[TEST_LOG_CAPTURE_SIZE - 1U] = '\0';

    return (strstr((const char *)os_test_log_capture, needle) != NULL);
}
#endif /* OS_CONFIG_LOG_ENABLE */

/******************************************************************************************************/
/**
 * @brief Poll (bounded) until a task reports INACTIVE, i.e. it has fully self-terminated
 *        and its stack is free to reuse for the next helper.
 */
static bool test_wait_inactive(const os_task_t *task, uint32_t timeout_ms)
{
    uint32_t start = os_tick_get();

    while (os_task_state_get(task) != OS_TASK_STATE_INACTIVE)
    {
        if ((os_tick_get() - start) > OS_TICKS_FROM_MS(timeout_ms))
        {
            return false;
        }

        os_delay_ms(5U);
    }

    return true;
}

/******************************************************************************************************/
static void test_worker_entry(void *context)
{
    (void)context;

    while (os_test_worker_should_run)
    {
        os_test_worker_counter++;
        os_task_yield();
    }
}

/******************************************************************************************************/
/**
 * @brief Busy-spins incrementing os_test_busy_counter until told to stop - never yields or delays, so
 *        it only gets CPU time on ticks nothing higher-priority is ready for. Shared by
 *        test_priority_preemption() and test_cpu_usage().
 */
static void test_busy_spin_entry(void *context)
{
    (void)context;

    while (os_test_busy_should_run)
    {
        os_test_busy_counter++;
    }
}

/******************************************************************************************************/
/**
 * @brief Burns a fixed number of cycles then returns (self-exiting) - never yields, delays, or
 *        calls any blocking kernel API, so for its whole run nothing at an equal or lower
 *        priority can execute. Used by test_priority_preemption() to prove strict priority
 *        ordering, not just "eventually runs".
 */
static void test_burst_spin_entry(void *context)
{
    __IO uint32_t i;

    (void)context;

    for (i = 0U; i < TEST_BURST_ITERATIONS; i++)
    {
        /* Burn cycles; the loop body is intentionally empty. */
    }
}

/******************************************************************************************************/
/**
 * @brief Increments os_test_switch_count then immediately yields, in a loop, until told to stop.
 *        Run on two equal-priority tasks at once (see test_context_switch_timing()), they
 *        ping-pong the CPU between them - each turn is one context switch in, so the total
 *        count over a fixed window approximates how many switches occurred.
 */
static void test_switch_ping_entry(void *context)
{
    (void)context;

    while (os_test_switch_should_run)
    {
        os_test_switch_count++;
        os_task_yield();
    }
}

/******************************************************************************************************/
/**
 * @brief Worker body for the self-pause test: waits briefly, pauses itself (NULL means the
 *        calling task), then - once resumed by another task - proves it by setting a sentinel.
 */
static void test_self_pause_worker_entry(void *context)
{
    (void)context;

    os_delay_ms(20U);
    (void)os_task_pause(NULL);
    /* execution resumes here once another task calls os_task_start() on us */
    os_test_worker_counter = 42U;
}

#if TEST_HELPER_NEEDED
/******************************************************************************************************/
/**
 * @brief Generic helper task body: reads os_test_helper_ctx (set by test_spawn_helper before create)
 *        to decide what to do, then returns - the port auto-deletes the task on return.
 *
 * Guarded like test_spawn_helper, its only caller: with every one of mutex/semaphore/queue/event
 * compiled out, all four of its cases vanish and nothing references it, so an unguarded definition
 * is an unused function - which a -Werror build rejects.
 */
static void test_helper_entry(void *context)
{
    (void)context;

    switch (os_test_helper_ctx.role)
    {
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    case HELPER_MUTEX_HOLD:
        (void)os_mutex_lock(&os_test_mutex, OS_WAIT_FOREVER);
        (void)os_semaphore_give(&os_test_sync_sem);
        os_delay_ms(os_test_helper_ctx.hold_ms);
        (void)os_mutex_unlock(&os_test_mutex);
        break;
#endif

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    case HELPER_SEM_GIVE_AFTER:
        os_delay_ms(os_test_helper_ctx.hold_ms);
        (void)os_semaphore_give(&os_test_count_sem);
        break;
#endif

#if (OS_CONFIG_EVENT_ENABLE == 1U)
    case HELPER_EVENT_SET_AFTER:
        os_delay_ms(os_test_helper_ctx.hold_ms);
        (void)os_event_set_bits(&os_test_event, os_test_helper_ctx.bits);
        break;
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    case HELPER_QUEUE_SEND_AFTER:
        os_delay_ms(os_test_helper_ctx.hold_ms);
        (void)os_queue_send(&os_test_queue, &os_test_helper_ctx.value, OS_WAIT_FOREVER);
        break;
#endif

    default:
        break;
    }
}
#endif /* TEST_HELPER_NEEDED */

#if TEST_HELPER_NEEDED
/******************************************************************************************************/
static os_status test_spawn_helper(helper_role_t role, uint32_t hold_ms, uint32_t bits, uint32_t value)
{
    os_status status;

    os_test_helper_ctx.role    = role;
    os_test_helper_ctx.hold_ms = hold_ms;
    os_test_helper_ctx.bits    = bits;
    os_test_helper_ctx.value   = value;

    status = os_task_create(&helper, OS_TASK_CONFIG(test_helper_entry, NULL, 3U));
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return os_task_start(&helper);
}
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

/*
 * ***********************************************************************************************************
 * Kernel core: lifecycle, tick, delay, critical sections
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void test_kernel_core(void)
{
    uint32_t t0;
    uint32_t t1;

    test_print_section("Kernel / Tick");

    AHURA_TEST_CHECK(os_kernel_is_running(), "os_kernel_is_running() is true once a task is executing");

    t0 = os_tick_get();
    os_delay_ms(20U);
    t1 = os_tick_get();
    AHURA_TEST_CHECK((t1 - t0) >= OS_TICKS_FROM_MS(20U), "os_tick_get() advances with time (delta=%lu ticks)",
                      (unsigned long)(t1 - t0));
}

/******************************************************************************************************/
static void test_delay(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;

    test_print_section("Delay APIs");

    /* Capture status/t1 before any AHURA_TEST_CHECK() runs: its printf() on a match blocks on
     * a polled UART transmit (~3 ticks/line at 115200 baud) - checking status inline between
     * t0 and t1 would fold that print time into the measured delay. */
    t0    = os_tick_get();
    os_delay_ms(50U);
    t1    = os_tick_get();
    delta = t1 - t0;
    AHURA_TEST_CHECK((delta >= 50U) && (delta <= 65U), "os_delay_ms(50) elapsed %lu ticks (expected ~50)",
                      (unsigned long)delta);

    t0    = os_tick_get();
    os_delay_us(3000U);
    t1    = os_tick_get();
    delta = t1 - t0;
    AHURA_TEST_CHECK((delta >= 2U) && (delta <= 10U), "os_delay_us(3000) elapsed %lu ticks (expected ~3)",
                      (unsigned long)delta);

    t0    = os_tick_get();
    os_delay_s(1U);
    t1    = os_tick_get();
    delta = t1 - t0;
    AHURA_TEST_CHECK((delta >= 1000U) && (delta <= 1060U), "os_delay_s(1) elapsed %lu ticks (expected ~1000)",
                      (unsigned long)delta);
}

/******************************************************************************************************/
static void test_critical_section(void)
{
    test_print_section("Critical Sections");

    /* os_arch_kernel_mask_active reads PRIMASK or BASEPRI depending on
     * OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY, so the checks hold in both
     * kernel mask modes. */
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() == 0U, "the kernel mask is lowered before entering a critical section");

    os_critical_enter();
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() != 0U, "os_critical_enter() raises the kernel mask");

    os_critical_enter(); /* nested */
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() != 0U, "a nested os_critical_enter() keeps the kernel mask raised");

    os_critical_exit(); /* inner exit: outer level still held */
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() != 0U, "exiting the inner level keeps the kernel mask raised (nesting works)");

    os_critical_exit(); /* outer exit */
    AHURA_TEST_CHECK(os_arch_kernel_mask_active() == 0U, "the matching outer os_critical_exit() lowers the kernel mask");
}

/******************************************************************************************************/
static void test_task_lifecycle(void)
{
    os_task_config_t cfg;
    os_status        status;
    uint32_t         snapshot;

    test_print_section("Task Lifecycle");

    /* --- Reject invalid creation parameters (should not touch any handle). --- */
    cfg = *OS_TASK_CONFIG(test_worker_entry, NULL, 1U);

    cfg.priority = 0U;
    AHURA_TEST_CHECK(os_task_create(&helper, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects priority 0 (idle-reserved)");

    cfg.priority = OS_TASK_PRIO_MAX;
    AHURA_TEST_CHECK(os_task_create(&helper, &cfg) == OS_STATUS_INVALID_ARG,
                      "os_task_create() rejects priority %u (kernel-reserved)", (unsigned)OS_TASK_PRIO_MAX);

    cfg.priority = OS_TASK_PRIO_1_LOWEST;

    /* The stack travels with the handle now, not the config, so an unusable stack is an unusable
     * handle. These descriptors stand in for an OS_TASK_DEFINE that somehow got it wrong - which
     * the macro itself cannot, since it derives both fields from the array it just declared. */
    {
        static const os_task_storage_t storage_too_small =
            { "too_small", helper_STACK, OS_CONFIG_MIN_STACK_SIZE - 8U };
        static const os_task_storage_t storage_misaligned =
            { "misaligned", &helper_STACK[1], sizeof(helper_STACK) - 8U };

        os_task_t bad = { 0 };

        bad.storage = &storage_too_small;
        AHURA_TEST_CHECK(os_task_create(&bad, &cfg) == OS_STATUS_INVALID_ARG,
                          "os_task_create() rejects a stack smaller than OS_CONFIG_MIN_STACK_SIZE");

        bad.storage = &storage_misaligned;
        AHURA_TEST_CHECK(os_task_create(&bad, &cfg) == OS_STATUS_INVALID_ARG,
                          "os_task_create() rejects a misaligned stack pointer");

        bad.storage = NULL;
        AHURA_TEST_CHECK(os_task_create(&bad, &cfg) == OS_STATUS_INVALID_ARG,
                          "os_task_create() rejects a handle OS_TASK_DEFINE never set up");
    }

    /* --- Real worker: create / start / observe / pause / resume / delete. --- */
    os_test_worker_counter    = 0U;
    os_test_worker_should_run = true;

    status = os_task_create(&worker, OS_TASK_CONFIG(test_worker_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "os_task_create() creates the worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED,
                      "a created-but-not-started task reports SUSPENDED");

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "os_task_start() starts the worker task");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter > 0U, "worker task actually executed (counter=%lu)",
                      (unsigned long)os_test_worker_counter);
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_READY,
                      "a lower-priority runnable task reports READY while this task runs");

    AHURA_TEST_CHECK(os_task_pause(&worker) == OS_STATUS_OK, "os_task_pause() suspends the worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED, "paused task reports SUSPENDED");
    snapshot = os_test_worker_counter;
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter == snapshot, "counter is frozen while the worker is paused");

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "os_task_start() resumes a paused task");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter > snapshot, "counter resumes advancing after os_task_start()");

    AHURA_TEST_CHECK(os_task_delete(&worker) == OS_STATUS_OK, "os_task_delete() deletes the live worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_INACTIVE,
                      "a deleted task's handle reports INACTIVE");
    snapshot = os_test_worker_counter;
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter == snapshot, "counter is frozen after deletion (worker truly stopped)");

    /* --- NULL means "current task": the worker pauses itself; we resume it. --- */
    os_test_worker_counter = 0U;
    status = os_task_create(&worker, OS_TASK_CONFIG(test_self_pause_worker_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "worker task re-created for the self-pause test");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "os_task_start() starts it");

    os_delay_ms(40U); /* let it reach os_task_pause(NULL) */
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED,
                      "os_task_pause(NULL) suspends the calling task itself");

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK,
                      "os_task_start() resumes a task that paused itself");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter == 42U, "the resumed task continued executing past its self-pause point");

    /* test_self_pause_worker_entry() already returned above (auto-exiting via the arch port's
     * os_task_exit() trampoline) - no explicit os_task_delete() here, that would fail with
     * INVALID_ARG since the slot is already freed. Just confirm the self-exit completed. */
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "the resumed worker terminates cleanly on its own");
}

/*
 * ***********************************************************************************************************
 * Task identity (id allocation)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Proves task ids are true identities, not just slot indices: no two simultaneously live
 *        tasks ever share an id, and a deleted task's id is never handed to the task that reuses
 *        its slot. That second property is what stops a stale handle from silently addressing a
 *        different task - e.g. unlocking a mutex owned by whoever now occupies the slot.
 */
static void test_task_identity(void)
{
    uint32_t id_a;
    uint32_t id_b;
    uint32_t id_c;
    uint32_t stale_id;
    os_task_t stale_handle;

    test_print_section("Task Identity (id allocation)");

    os_test_worker_should_run = true;

    /* Three tasks alive at once: their ids must all differ. */
    AHURA_TEST_CHECK(os_task_create(&worker, OS_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_STATUS_OK,
                      "identity task A created");
    AHURA_TEST_CHECK(os_task_create(&helper, OS_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_STATUS_OK,
                      "identity task B created");
    AHURA_TEST_CHECK(os_task_create(&helper2, OS_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_STATUS_OK,
                      "identity task C created");

    id_a = worker.id;
    id_b = helper.id;
    id_c = helper2.id;

    AHURA_TEST_CHECK((id_a != 0U) && (id_b != 0U) && (id_c != 0U),
                      "every live task has a nonzero id (%lu, %lu, %lu)",
                      (unsigned long)id_a, (unsigned long)id_b, (unsigned long)id_c);
    AHURA_TEST_CHECK((id_a != id_b) && (id_b != id_c) && (id_a != id_c),
                      "no two simultaneously live tasks share an id (%lu, %lu, %lu)",
                      (unsigned long)id_a, (unsigned long)id_b, (unsigned long)id_c);

    /* Keep a copy of B's handle, then delete B so its table slot is recycled. */
    stale_handle = helper;
    stale_id     = helper.id;

    AHURA_TEST_CHECK(os_task_delete(&helper) == OS_STATUS_OK, "identity task B deleted, freeing its slot");
    AHURA_TEST_CHECK(os_task_state_get(&stale_handle) == OS_TASK_STATE_INACTIVE,
                      "a stale handle to the deleted task reports INACTIVE");

    /* The next task very likely lands in B's freed slot - but must not inherit B's id. */
    AHURA_TEST_CHECK(os_task_create(&helper3, OS_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_STATUS_OK,
                      "identity task D created into the freed slot");
    AHURA_TEST_CHECK(helper3.id != stale_id,
                      "the task reusing a freed slot gets a fresh id, not the deleted task's (%lu vs %lu)",
                      (unsigned long)helper3.id, (unsigned long)stale_id);
    AHURA_TEST_CHECK(os_task_state_get(&stale_handle) == OS_TASK_STATE_INACTIVE,
                      "the stale handle still resolves to nothing, not to the task that took the slot");

    os_test_worker_should_run = false;
    (void)os_task_delete(&worker);
    (void)os_task_delete(&helper2);
    (void)os_task_delete(&helper3);

    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_INACTIVE, "identity tasks cleaned up");
}

/*
 * ***********************************************************************************************************
 * Priority-based preemption
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Proves strict priority ordering, not just "eventually runs": a lower-priority task
 *        spinning without ever yielding is fully starved for as long as a higher-priority task
 *        is ready, and resumes the instant that higher-priority task is gone.
 */
static void test_priority_preemption(void)
{
    uint32_t  snapshot_before;
    uint32_t  snapshot_immediate;
    uint32_t  snapshot_after;
    os_status status;
    os_status start_status;

    test_print_section("Priority-Based Preemption");

    /* Everything below rests on the test task having room underneath it. Reported as its own
     * check so a misconfigured priority says exactly that, instead of surfacing as a spinner
     * that mysteriously kept counting. */
    AHURA_TEST_CHECK(OS_CONFIG_TEST_PRIORITY >= OS_TASK_PRIO_2,
                      "OS_CONFIG_TEST_PRIORITY (%u) leaves a usable priority below the test task",
                      (unsigned)OS_CONFIG_TEST_PRIORITY);
    AHURA_TEST_CHECK(TEST_PRIO_HIGH <= OS_TASK_PRIO_30_HIGHEST,
                      "OS_CONFIG_TEST_PRIORITY (%u) leaves a usable priority above the test task",
                      (unsigned)OS_CONFIG_TEST_PRIORITY);

    os_test_busy_counter    = 0U;
    os_test_busy_should_run = true;
    status = os_task_create(&worker, OS_TASK_CONFIG(test_busy_spin_entry, NULL, TEST_PRIO_LOW));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "low-priority spinner task created (priority %u)",
                      (unsigned)TEST_PRIO_LOW);
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "low-priority spinner started");

    os_delay_ms(20U);
    snapshot_before = os_test_busy_counter;
    AHURA_TEST_CHECK(snapshot_before > 0U,
                      "the low-priority spinner gets CPU time when nothing outranks it (count=%lu)",
                      (unsigned long)snapshot_before);

    /* A task at a strictly higher priority than both the spinner and this test task never
     * yields/delays for its whole burst - so the spinner cannot possibly run until it is gone. */
    status = os_task_create(&helper, OS_TASK_CONFIG(test_burst_spin_entry, NULL,
                                                            TEST_PRIO_HIGH));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "higher-priority burst task created (priority %u)",
                      (unsigned)TEST_PRIO_HIGH);

    /* Both snapshots are taken with NOTHING in between but the start and a busy-wait: an
     * AHURA_TEST_CHECK here would printf, and that polled UART write takes milliseconds during
     * which ticks fire and the scheduler runs, which is precisely the window this check is
     * supposed to prove is quiet. Statuses are recorded now and reported after the sampling.
     *
     * os_delay_us busy-waits and never yields, so those 100 us are real wall time in which the
     * spinner is READY and simply must not be picked - a stronger claim than sampling
     * instantly, which a lucky instant could pass by accident. */
    snapshot_before   = os_test_busy_counter;
    start_status      = os_task_start(&helper);
    os_delay_us(100U);
    snapshot_immediate = os_test_busy_counter;

    AHURA_TEST_CHECK(start_status == OS_STATUS_OK, "higher-priority burst task started");
    AHURA_TEST_CHECK(snapshot_immediate == snapshot_before,
                      "the spinner stayed frozen for 100 us while a higher-priority task ran "
                      "(count %lu -> %lu)",
                      (unsigned long)snapshot_before, (unsigned long)snapshot_immediate);

    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U),
                      "the higher-priority burst task ran to completion and self-terminated");

    os_delay_ms(10U);
    snapshot_after = os_test_busy_counter;
    AHURA_TEST_CHECK(snapshot_after > snapshot_before,
                      "the spinner resumes running once the higher-priority task is gone (count=%lu)",
                      (unsigned long)snapshot_after);

    os_test_busy_should_run = false;
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "low-priority spinner stops cleanly");
}

/*
 * ***********************************************************************************************************
 * Scheduler lock
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Runs at TEST_PRIO_HIGH, so it outranks the test task and would preempt it the instant it
 *        is started - unless the scheduler is locked.
 */
static void test_sched_lock_entry(void *context)
{
    (void)context;

    os_test_sched_lock_ran = true;
}

/******************************************************************************************************/
/**
 * @brief os_kernel_lock() defers preemption without masking interrupts.
 *
 * The claim under test is not "nothing ran" - a critical section would give that too - but that
 * nothing ran WHILE INTERRUPTS STAYED LIVE. The locked window is 200 us of real wall time
 * (os_delay_us busy-waits and never yields), so at a 1 kHz tick the tick interrupt fires inside it,
 * and with it every wake and round-robin decision the kernel makes. A task that outranks this one
 * is started inside that window and must still not get the CPU until the unlock.
 *
 * The two flags are sampled inside the window and reported afterwards on purpose: AHURA_TEST_CHECK
 * printfs, and that polled UART write takes milliseconds - it would dominate the window it is
 * supposed to be measuring.
 */
static void test_scheduler_lock(void)
{
    os_status status;
    bool      locked_flag;
    bool      ran_while_locked;
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    os_status take_status;
#endif

    test_print_section("Scheduler Lock");

    AHURA_TEST_CHECK(!os_kernel_is_locked(), "the scheduler starts out unlocked");

    os_test_sched_lock_ran = false;

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    AHURA_TEST_CHECK(os_semaphore_init(&os_test_sched_lock_sem, 0U, 1U) == OS_STATUS_OK,
                      "empty semaphore initialized (a take on it can only block)");
#endif

    status = os_task_create(&helper, OS_TASK_CONFIG(test_sched_lock_entry, NULL, TEST_PRIO_HIGH));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "higher-priority task created (priority %u)",
                      (unsigned)TEST_PRIO_HIGH);

    os_kernel_lock();

    locked_flag = os_kernel_is_locked();
    (void)os_task_start(&helper);
    os_delay_us(200U);
    ran_while_locked = os_test_sched_lock_ran;
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    take_status = os_semaphore_take(&os_test_sched_lock_sem, 10U);
#endif

    os_kernel_unlock();

    /* The unlock issues the switch it deferred, and the task that outranks this one takes the CPU
     * before the next line runs - so by here it has already finished. */
    AHURA_TEST_CHECK(locked_flag, "os_kernel_is_locked() reports the lock while held");
    AHURA_TEST_CHECK(!ran_while_locked,
                      "higher-priority task held back for 200 us of live-interrupt time");
    AHURA_TEST_CHECK(os_test_sched_lock_ran, "os_kernel_unlock() took the deferred switch at once");
    AHURA_TEST_CHECK(!os_kernel_is_locked(), "os_kernel_unlock() released the lock");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "the higher-priority task ran to completion");

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    /* Blocking under the lock would park a task the lock then refuses to switch away from, so
     * every blocking primitive degrades to its OS_WAIT_NOTHING behaviour instead - EMPTY here,
     * NOT the TIMEOUT a real 10 ms wait would have reported. */
    AHURA_TEST_CHECK(take_status == OS_STATUS_EMPTY,
                      "os_semaphore_take(10 ms) is non-blocking under the lock (status=%d)",
                      (int)take_status);
#endif

    /* Nesting: only the outermost unlock reopens the scheduler. */
    os_kernel_lock();
    os_kernel_lock();
    os_kernel_unlock();
    locked_flag = os_kernel_is_locked();
    os_kernel_unlock();

    AHURA_TEST_CHECK(locked_flag, "a nested unlock leaves the scheduler locked");
    AHURA_TEST_CHECK(!os_kernel_is_locked(), "the outermost unlock releases it");

    /* Self-suspension needs the switch the lock is holding back, so it is refused rather than
     * leaving this task running while the kernel marks it SUSPENDED. */
    os_kernel_lock();
    status = os_task_pause(NULL);
    os_kernel_unlock();

    AHURA_TEST_CHECK(status == OS_STATUS_BUSY, "os_task_pause(self) is refused under the lock (status=%d)",
                      (int)status);
}

/*
 * ***********************************************************************************************************
 * Mutex
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
static void test_mutex(void)
{
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    /* Only the contention part below uses these, and it needs a semaphore to know when the helper
     * has actually taken the mutex. Declaring them unconditionally left them unused whenever
     * semaphores were compiled out. */
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;
    os_status status;
#endif

    test_print_section("Mutex");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_mutex) == OS_STATUS_OK, "os_mutex_init() succeeds");
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_mutex) == OS_STATUS_ERROR, "unlocking a free mutex returns ERROR");

    AHURA_TEST_CHECK(os_mutex_lock(&os_test_mutex, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "os_mutex_lock() acquires a free mutex");
    AHURA_TEST_CHECK(os_mutex_try_lock(&os_test_mutex) == OS_STATUS_BUSY,
                      "re-locking from the owner fails BUSY (not recursive)");
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_mutex) == OS_STATUS_OK, "owner os_mutex_unlock() releases the mutex");

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    /* Contention: a helper task holds the mutex for 150 ms. */
    (void)os_semaphore_init(&os_test_sync_sem, 0U, 1U);
    AHURA_TEST_CHECK(test_spawn_helper(HELPER_MUTEX_HOLD, 150U, 0U, 0U) == OS_STATUS_OK,
                      "helper task spawned to hold the mutex");
    AHURA_TEST_CHECK(os_semaphore_take(&os_test_sync_sem, 200U) == OS_STATUS_OK, "helper signals once it holds the mutex");

    AHURA_TEST_CHECK(os_mutex_try_lock(&os_test_mutex) == OS_STATUS_BUSY,
                      "os_mutex_try_lock() fails while another task holds the mutex");
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_mutex) == OS_STATUS_NOT_OWNER,
                      "unlocking a mutex owned by another task returns NOT_OWNER");

    t0     = os_tick_get();
    status = os_mutex_lock(&os_test_mutex, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "blocking os_mutex_lock() succeeds once the holder releases it");
    AHURA_TEST_CHECK((delta >= 100U) && (delta <= 250U), "blocking lock woke ~when the holder unlocked (%lu ticks)",
                      (unsigned long)delta);

    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_mutex) == OS_STATUS_OK, "final os_mutex_unlock() releases the mutex");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "mutex-holder helper task terminated cleanly");
#endif
}
#endif /* OS_CONFIG_MUTEX_ENABLE */

/*
 * ***********************************************************************************************************
 * Semaphore
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
/******************************************************************************************************/
static void test_semaphore(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;
    os_status status;

    test_print_section("Semaphore");

    AHURA_TEST_CHECK(os_semaphore_init(&os_test_bin_sem, 0U, 1U) == OS_STATUS_OK,
                      "os_semaphore_init() creates a binary semaphore (0/1)");
    AHURA_TEST_CHECK(os_semaphore_take(&os_test_bin_sem, OS_WAIT_NOTHING) == OS_STATUS_EMPTY,
                      "take on an empty semaphore with OS_WAIT_NOTHING returns EMPTY");
    AHURA_TEST_CHECK(os_semaphore_give(&os_test_bin_sem) == OS_STATUS_OK, "os_semaphore_give() adds a token");
    AHURA_TEST_CHECK(os_semaphore_give(&os_test_bin_sem) == OS_STATUS_FULL, "giving beyond max_count returns FULL");
    AHURA_TEST_CHECK(os_semaphore_take(&os_test_bin_sem, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "take succeeds once a token is available");

    t0     = os_tick_get();
    status = os_semaphore_take(&os_test_bin_sem, 100U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_TIMEOUT, "take on an empty semaphore times out");
    AHURA_TEST_CHECK((delta >= 95U) && (delta <= 150U), "timeout elapsed ~100 ticks (%lu)", (unsigned long)delta);

    AHURA_TEST_CHECK(os_semaphore_init(&os_test_count_sem, 0U, 3U) == OS_STATUS_OK,
                      "os_semaphore_init() creates a counting semaphore (0/3)");
    AHURA_TEST_CHECK(test_spawn_helper(HELPER_SEM_GIVE_AFTER, 80U, 0U, 0U) == OS_STATUS_OK,
                      "helper spawned to give the counting semaphore after 80 ms");

    t0     = os_tick_get();
    status = os_semaphore_take(&os_test_count_sem, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "blocking take succeeds once the helper gives");
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "take woke ~when the helper gave (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "semaphore-giver helper task terminated cleanly");
}
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

/*
 * ***********************************************************************************************************
 * Queue
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
/******************************************************************************************************/
#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
/* Shared between the two contenders in test_atomic(): both hammer the same counters, one through
 * os_atomic_inc and one with a plain read-modify-write, so the two can be compared directly. */
#define TEST_ATOMIC_ITERATIONS 20000UL

static os_atomic_t      os_test_atomic_counter = OS_ATOMIC_INIT(0);
static __IO int32_t os_test_plain_counter  = 0;

/* Declared as os_atomic_t rather than a volatile int, which is what the header asks for: casting
 * some other type to os_atomic_t * to reach these calls is how a "volatile" counter quietly
 * becomes one the compiler is free to cache again. */
static os_atomic_t      os_test_atomic_done    = OS_ATOMIC_INIT(0);

/******************************************************************************************************/
static void test_atomic_hammer_entry(void *context)
{
    uint32_t i;

    (void)context;

    for (i = 0U; i < TEST_ATOMIC_ITERATIONS; i++)
    { 
        (void)os_atomic_inc(&os_test_atomic_counter);

        /* Deliberately NOT atomic, as the control case: load, add, store, with a preemption point
         * wide open in the middle of it. */
        os_test_plain_counter = os_test_plain_counter + 1;
    }

    (void)os_atomic_inc(&os_test_atomic_done);
}

/******************************************************************************************************/
/**
 * @brief Covers the os_atomic_* API: return values, bit operations, and that concurrent updates
 *        from two tasks actually survive.
 */
static void test_atomic(void)
{
    os_atomic_t value = OS_ATOMIC_INIT(0);
    uint32_t    waited;
    bool        ok;

    test_print_section("Atomics");

    /* Every read-modify-write returns the value held BEFORE the operation, so a return of 20 from
     * inc() means the word now reads 21. Checked as one group because a single wrong return value
     * here invalidates the whole convention, not just one call. */
    (void)os_atomic_set(&value, 10);
    ok  = (os_atomic_get(&value) == 10);
    ok &= (os_atomic_set(&value, 20) == 10);
    ok &= (os_atomic_add(&value, 5) == 20) && (os_atomic_get(&value) == 25);
    ok &= (os_atomic_sub(&value, 5) == 25) && (os_atomic_get(&value) == 20);
    ok &= (os_atomic_inc(&value)    == 20) && (os_atomic_get(&value) == 21);
    ok &= (os_atomic_dec(&value)    == 21) && (os_atomic_get(&value) == 20);
    ok &= (os_atomic_clear(&value)  == 20) && (os_atomic_get(&value) == 0);
    AHURA_TEST_CHECK(ok, "set/add/sub/inc/dec/clear all return the value from before the operation");

    (void)os_atomic_set(&value, 0x0F0F);
    ok  = (os_atomic_or(&value, 0xF000) == 0x0F0F) && (os_atomic_get(&value) == 0xFF0F);
    (void)os_atomic_set(&value, 0x0F0F);
    ok &= (os_atomic_and(&value, 0x00FF) == 0x0F0F) && (os_atomic_get(&value) == 0x000F);
    (void)os_atomic_set(&value, 0x0F0F);
    ok &= (os_atomic_xor(&value, 0xFFFF) == 0x0F0F) && (os_atomic_get(&value) == 0xF0F0);
    (void)os_atomic_set(&value, 0x0F0F);
    ok &= (os_atomic_nand(&value, 0x00FF) == 0x0F0F) && (os_atomic_get(&value) == (int32_t)~0x000F);
    AHURA_TEST_CHECK(ok, "or/and/xor/nand apply the right operation and return the previous value");

    (void)os_atomic_set(&value, 100);
    ok  = os_atomic_cas(&value, 100, 200) && (os_atomic_get(&value) == 200);
    ok &= !os_atomic_cas(&value, 100, 300) && (os_atomic_get(&value) == 200);
    AHURA_TEST_CHECK(ok, "os_atomic_cas() swaps on a match and leaves the word alone otherwise");

    (void)os_atomic_clear(&value);
    os_atomic_set_bit(&value, 3U);
    ok  = (os_atomic_get(&value) == 8) && os_atomic_test_bit(&value, 3U) &&
          !os_atomic_test_bit(&value, 4U);
    ok &= os_atomic_test_and_set_bit(&value, 3U);          /* was already set */
    ok &= !os_atomic_test_and_set_bit(&value, 4U) && os_atomic_test_bit(&value, 4U);
    ok &= os_atomic_test_and_clear_bit(&value, 4U) && !os_atomic_test_bit(&value, 4U);
    os_atomic_clear_bit(&value, 3U);
    ok &= (os_atomic_get(&value) == 0);
    os_atomic_set_bit_to(&value, 7U, true);
    ok &= os_atomic_test_bit(&value, 7U);
    os_atomic_set_bit_to(&value, 7U, false);
    ok &= !os_atomic_test_bit(&value, 7U);
    AHURA_TEST_CHECK(ok, "the bit operations set, clear and report previous state correctly");

    /* Bit 31 is a valid index and must not be mistaken for a sign problem. */
    (void)os_atomic_clear(&value);
    os_atomic_set_bit(&value, 31U);
    AHURA_TEST_CHECK(os_atomic_test_bit(&value, 31U) && (os_atomic_get(&value) == INT32_MIN),
                      "bit 31 works like any other and is the word's sign bit");

    /* --- The part that actually matters: concurrent updates --- */

    os_test_atomic_counter = OS_ATOMIC_INIT(0);
    os_test_plain_counter  = 0;
    os_test_atomic_done    = OS_ATOMIC_INIT(0);

    /* Two tasks at the same priority as each other: they round-robin every tick, so each is
     * preempted repeatedly mid-update. That is precisely the window a non-atomic
     * read-modify-write loses. */
    if ((os_task_create(&worker, OS_TASK_CONFIG(test_atomic_hammer_entry, NULL,
                                                        TEST_PRIO_HIGH)) == OS_STATUS_OK) &&
        (os_task_create(&helper, OS_TASK_CONFIG(test_atomic_hammer_entry, NULL,
                                                        TEST_PRIO_HIGH)) == OS_STATUS_OK))
    {
        (void)os_task_start(&worker);
        (void)os_task_start(&helper);

        for (waited = 0U; (waited < 4000U) &&
                          (os_atomic_get(&os_test_atomic_done) < 2); waited++)
        {
            os_delay_ms(1U);
        }

        AHURA_TEST_CHECK(os_atomic_get(&os_test_atomic_done) == 2,
                          "both contending tasks finished");
        AHURA_TEST_CHECK(os_atomic_get(&os_test_atomic_counter) == (int32_t)(2UL * TEST_ATOMIC_ITERATIONS),
                          "os_atomic_inc() lost nothing across %lu concurrent increments (got %ld)",
                          (unsigned long)(2UL * TEST_ATOMIC_ITERATIONS),
                          (long)os_atomic_get(&os_test_atomic_counter));

        /* Reported, not asserted: a plain read-modify-write is ALLOWED to come out correct if the
         * scheduler never lands between its load and its store. Asserting that it breaks would
         * make this test fail for the wrong reason on a machine where it happens to survive. */
        printf("  [INFO] the same loop without atomics reached %ld of %lu\r\n",
               (long)os_test_plain_counter, (unsigned long)(2UL * TEST_ATOMIC_ITERATIONS));

        (void)os_task_delete(&worker);
        (void)os_task_delete(&helper);
    }
    else
    {
        printf("  [SKIP] could not create the two contending tasks\r\n");
    }
}

#endif /* OS_CONFIG_ATOMIC_ENABLE */

/* Statically defined queue used by test_queue_define_and_dynamic(): the whole point of the macro
 * pair is that the geometry is stated once, here, and never repeated at the init call. */
typedef struct
{
    uint32_t id;
    uint8_t  payload[6];

} test_queue_item_t;

OS_QUEUE_DEFINE_STATIC(os_test_defined_queue, test_queue_item_t, 4);

/******************************************************************************************************/
/**
 * @brief Covers both ways of getting a queue: OS_QUEUE_DEFINE_STATIC static storage, and
 *        os_queue_init_dynamic heap storage, including that cleanup frees one and not the other.
 */
static void test_queue_define_and_dynamic(void)
{
    test_queue_item_t sent    = { 0 };
    test_queue_item_t got     = { 0 };
    os_queue_t        dynamic = { 0 };
    uint32_t          value   = 0U;
    size_t            heap_before;
    size_t            heap_after;
    os_status         status;

    test_print_section("Queue Definition (static macro and dynamic allocation)");

    /* --- OS_QUEUE_DEFINE_STATIC --- */

    AHURA_TEST_CHECK(sizeof(os_test_defined_queue_BUFFER) == (4U * sizeof(test_queue_item_t)),
                      "OS_QUEUE_DEFINE_STATIC() sized the buffer for 4 items of the declared type (%u bytes)",
                      (unsigned)sizeof(os_test_defined_queue_BUFFER));

    /* Nothing has been called on this queue: every field below was written by the macro at compile
     * time. The geometry has to match the declaration, since getting either wrong is exactly the
     * out-of-bounds bug that deriving it from the declaration exists to make impossible. */
    AHURA_TEST_CHECK(os_test_defined_queue.item_size == sizeof(test_queue_item_t),
                      "the item size comes from the declared type with no init call (%u bytes)",
                      (unsigned)os_test_defined_queue.item_size);
    AHURA_TEST_CHECK(os_test_defined_queue.capacity == 4U,
                      "the capacity comes from the declared count (%u)",
                      (unsigned)os_test_defined_queue.capacity);
    AHURA_TEST_CHECK(os_test_defined_queue.buffer == (uint8_t *)os_test_defined_queue_BUFFER,
                      "the queue points at the buffer the macro declared");
    AHURA_TEST_CHECK((os_test_defined_queue.count == 0U) && (os_test_defined_queue.head == 0U) &&
                      (os_test_defined_queue.tail == 0U) && !os_test_defined_queue.buffer_owned &&
                      (os_test_defined_queue.send_waiters.head == NULL) &&
                      (os_test_defined_queue.receive_waiters.head == NULL),
                      "and starts empty with empty waiter lists, owning nothing");

    sent.id         = 0xA5A5A5A5UL;
    sent.payload[0] = 0x11U;
    sent.payload[5] = 0x99U;

    AHURA_TEST_CHECK(os_queue_send(&os_test_defined_queue, &sent, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "a struct item goes into the statically defined queue, still with no init call");
    AHURA_TEST_CHECK(os_queue_receive(&os_test_defined_queue, &got, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "and comes back out");
    AHURA_TEST_CHECK((got.id == sent.id) && (got.payload[0] == 0x11U) && (got.payload[5] == 0x99U),
                      "the whole struct survived the round trip intact");

    /* --- os_queue_init_dynamic / os_queue_cleanup --- */

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    heap_before = os_mem_free_get();

    status = os_queue_init_dynamic(&dynamic, sizeof(uint32_t), 8U);
    AHURA_TEST_CHECK(status == OS_STATUS_OK,
                      "os_queue_init_dynamic() allocates an 8-slot uint32 queue");

    /* Ownership has to be true the moment the queue is usable, not a moment later: it is what
     * tells os_queue_cleanup the buffer came from the heap. A call that published the queue
     * before claiming ownership would leak that buffer to any cleanup landing in between. */
    AHURA_TEST_CHECK(dynamic.buffer_owned,
                      "an allocated queue owns its buffer as soon as it is usable");
    AHURA_TEST_CHECK(!os_test_defined_queue.buffer_owned,
                      "a statically defined queue never claims ownership of its buffer");

    heap_after = os_mem_free_get();
    AHURA_TEST_CHECK(heap_after < heap_before,
                      "creating it consumed kernel heap (%u -> %u bytes free)",
                      (unsigned)heap_before, (unsigned)heap_after);

    value = 0xDEADBEEFUL;
    AHURA_TEST_CHECK(os_queue_send(&dynamic, &value, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "the dynamic queue accepts an item");
    value = 0U;
    AHURA_TEST_CHECK(os_queue_receive(&dynamic, &value, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "the dynamic queue returns it");
    AHURA_TEST_CHECK(value == 0xDEADBEEFUL, "with the value intact (0x%08lX)", (unsigned long)value);

    AHURA_TEST_CHECK(os_queue_cleanup(&dynamic) == OS_STATUS_OK, "os_queue_cleanup() tears it down");
    AHURA_TEST_CHECK(os_mem_free_get() == heap_before,
                      "and returned every byte it took to the heap (%u bytes free)",
                      (unsigned)os_mem_free_get());
    AHURA_TEST_CHECK(dynamic.buffer == NULL, "the torn-down queue no longer points at freed memory");

    /* A zero or overflowing geometry must be refused rather than wrapped into a small allocation
     * that every later send would index past. */
    AHURA_TEST_CHECK(os_queue_init_dynamic(&dynamic, 0U, 4U) == OS_STATUS_INVALID_ARG,
                      "os_queue_init_dynamic() rejects a zero item size");
    AHURA_TEST_CHECK(os_queue_init_dynamic(&dynamic, 4U, 0U) == OS_STATUS_INVALID_ARG,
                      "os_queue_init_dynamic() rejects a zero capacity");
    AHURA_TEST_CHECK(os_queue_init_dynamic(&dynamic, SIZE_MAX / 2U, 4U) == OS_STATUS_INVALID_ARG,
                      "os_queue_init_dynamic() rejects a geometry whose byte count would overflow");

    /* A geometry that is valid but larger than the whole heap has to come back as NO_MEMORY, so a
     * caller can tell "ask for less" apart from "that request was nonsense". */
    AHURA_TEST_CHECK(os_queue_init_dynamic(&dynamic, 1U, OS_CONFIG_HEAP_SIZE * 2U) == OS_STATUS_NO_MEMORY,
                      "os_queue_init_dynamic() reports NO_MEMORY when the heap cannot cover the request");

    AHURA_TEST_CHECK(os_mem_free_get() == heap_before,
                      "and none of those rejections leaked heap");
#else
    (void)dynamic;
    (void)value;
    (void)heap_after;
    (void)status;
    printf("  [SKIP] os_queue_init_dynamic() requires OS_CONFIG_ALLOC_ENABLE=1\r\n");
#endif /* OS_CONFIG_ALLOC_ENABLE */

    /* --- os_queue_cleanup on static storage (no heap involved) --- */

    /* Tearing down a statically defined queue is allowed on every build, heapless included, and
     * must never hand the buffer the application declared to the kernel heap. Because there is
     * nothing to release, the queue keeps its storage and stays usable - the same promise the
     * macro makes at declaration: a static queue never needs an init call. */
    sent.id = 0x5A5A5A5AUL;
    (void)os_queue_send(&os_test_defined_queue, &sent, OS_WAIT_NOTHING);

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    heap_before = os_mem_free_get();
#else
    (void)heap_before;
#endif

    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_defined_queue) == OS_STATUS_OK,
                      "os_queue_cleanup() also accepts a statically defined queue");
    AHURA_TEST_CHECK(os_queue_count_get(&os_test_defined_queue) == 0U, "and empties it");
    AHURA_TEST_CHECK((os_test_defined_queue.buffer == (uint8_t *)os_test_defined_queue_BUFFER) &&
                      (os_test_defined_queue.item_size == sizeof(test_queue_item_t)) &&
                      (os_test_defined_queue.capacity == 4U),
                      "but keeps the storage it does not own, geometry intact");

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    AHURA_TEST_CHECK(os_mem_free_get() == heap_before,
                      "and did not free the static buffer into the kernel heap");
#endif

    /* Still usable with nothing called in between, which is the point of keeping the storage. */
    AHURA_TEST_CHECK(os_queue_send(&os_test_defined_queue, &sent, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "the queue works again straight after cleanup, with no init call");
    (void)os_queue_receive(&os_test_defined_queue, &got, OS_WAIT_NOTHING);
}

/******************************************************************************************************/
static void test_queue(void)
{
    uint32_t  items[3] = { 0 };
    uint32_t  value;
    uint32_t  i;
    bool      fifo_ok = true;
    os_status status;
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;

    test_print_section("Queue");

    /* No init call: OS_QUEUE_DEFINE_BUFFER initialized os_test_queue over os_test_queue_buf at compile time.
     * The geometry below is what the macro derived from the array, never a number passed by hand. */
    AHURA_TEST_CHECK((os_test_queue.buffer == (uint8_t *)os_test_queue_buf) &&
                      (os_test_queue.item_size == sizeof(os_test_queue_buf[0])) &&
                      (os_test_queue.capacity == (sizeof(os_test_queue_buf) / sizeof(os_test_queue_buf[0]))),
                      "OS_QUEUE_DEFINE_BUFFER() bound the queue to the declared array, geometry derived");
    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 0U, "a fresh queue reports 0 items");
    AHURA_TEST_CHECK(os_queue_free_get(&os_test_queue) == os_test_queue.capacity,
                      "a fresh queue reports its whole capacity free (%lu)",
                      (unsigned long)os_queue_free_get(&os_test_queue));
    AHURA_TEST_CHECK(os_queue_receive(&os_test_queue, &value, OS_WAIT_NOTHING) == OS_STATUS_EMPTY,
                      "receive on an empty queue with OS_WAIT_NOTHING returns EMPTY");

    for (i = 0U; i < 3U; i++)
    {
        AHURA_TEST_CHECK(os_queue_send(&os_test_queue, &i, OS_WAIT_NOTHING) == OS_STATUS_OK,
                          "send #%lu succeeds while the queue has room", (unsigned long)i);
    }
    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 3U, "queue count reports 3/3 full");
    AHURA_TEST_CHECK(os_queue_free_get(&os_test_queue) == 0U, "a full queue reports 0 free slots");

    value = 99U;
    AHURA_TEST_CHECK(os_queue_send(&os_test_queue, &value, OS_WAIT_NOTHING) == OS_STATUS_FULL,
                      "send on a full queue with OS_WAIT_NOTHING returns FULL");

    for (i = 0U; i < 3U; i++)
    {
        status = os_queue_receive(&os_test_queue, &items[i], OS_WAIT_NOTHING);
        if ((status != OS_STATUS_OK) || (items[i] != i))
        {
            fifo_ok = false;
        }
    }
    AHURA_TEST_CHECK(fifo_ok, "queue preserves FIFO order (got %lu,%lu,%lu)",
                      (unsigned long)items[0], (unsigned long)items[1], (unsigned long)items[2]);

    t0     = os_tick_get();
    status = os_queue_receive(&os_test_queue, &value, 100U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_STATUS_TIMEOUT, "receive on an empty queue times out");
    AHURA_TEST_CHECK((delta >= 95U) && (delta <= 150U), "timeout elapsed ~100 ticks (%lu)", (unsigned long)delta);

    AHURA_TEST_CHECK(test_spawn_helper(HELPER_QUEUE_SEND_AFTER, 80U, 0U, 42U) == OS_STATUS_OK,
                      "helper spawned to send item 42 after 80 ms");
    t0     = os_tick_get();
    status = os_queue_receive(&os_test_queue, &value, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK((status == OS_STATUS_OK) && (value == 42U),
                      "blocking receive gets the helper's item (value=%lu)", (unsigned long)value);
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "receive woke ~when the helper sent (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "queue-sender helper task terminated cleanly");
}
#endif /* OS_CONFIG_QUEUE_ENABLE */

/*
 * ***********************************************************************************************************
 * Event
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_EVENT_ENABLE == 1U)
/******************************************************************************************************/
static void test_event_group(void)
{
    uint32_t  matched;
    os_status status;
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;

    test_print_section("Events");

    AHURA_TEST_CHECK(os_event_init(&os_test_event) == OS_STATUS_OK, "os_event_init() succeeds");

    matched = 0xFFFFFFFFU;
    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x03U, false, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "wait-any on unset bits with OS_WAIT_NOTHING returns BUSY");
    AHURA_TEST_CHECK(matched == 0U, "matched_bits reports 0 when nothing matched");

    AHURA_TEST_CHECK(os_event_set_bits(&os_test_event, 0x01U) == OS_STATUS_OK,
                      "os_event_set_bits(0x01) succeeds");
    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x03U, false, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "wait-any matches once one of the requested bits is set");
    AHURA_TEST_CHECK(matched == 0x01U, "matched_bits reports the intersecting bits (0x%02lx)",
                      (unsigned long)matched);

    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x03U, true, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "wait-all is still BUSY while only some requested bits are set");

    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x01U, false, true, &matched, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "wait-any with clear_on_exit consumes the matched bit");
    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x01U, false, false, &matched, OS_WAIT_NOTHING) == OS_STATUS_BUSY,
                      "a consumed (atomically cleared) bit no longer matches");

    AHURA_TEST_CHECK(test_spawn_helper(HELPER_EVENT_SET_AFTER, 80U, 0x06U, 0U) == OS_STATUS_OK,
                      "helper spawned to set bits 0x06 after 80 ms");
    t0     = os_tick_get();
    status = os_event_wait_bits(&os_test_event, 0x06U, true, false, &matched, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK((status == OS_STATUS_OK) && (matched == 0x06U),
                      "wait-all matches once the helper sets both bits (matched=0x%02lx)", (unsigned long)matched);
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "wait woke ~when the helper set the bits (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "event-setter helper task terminated cleanly");
}
#endif /* OS_CONFIG_EVENT_ENABLE */

/*
 * ***********************************************************************************************************
 * Software timer
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
static void timer_oneshot_cb(void *context)
{
    (void)context;
    os_test_oneshot_fired++;
}

/******************************************************************************************************/
static void timer_periodic_cb(void *context)
{
    (void)context;
    os_test_periodic_fired++;
}

/******************************************************************************************************/
static void test_timer(void)
{
    uint32_t snapshot;

    test_print_section("Software Timer");

    os_test_oneshot_fired = 0U;
    AHURA_TEST_CHECK(os_timer_init(&os_test_timer_oneshot, OS_TICKS_FROM_MS(50U), OS_TIMER_MODE_ONE_SHOT, timer_oneshot_cb,
                                    NULL) == OS_STATUS_OK,
                      "os_timer_init() configures a one-shot timer (50 ms)");
    AHURA_TEST_CHECK(os_timer_start(&os_test_timer_oneshot) == OS_STATUS_OK, "os_timer_start() arms the one-shot timer");

    os_delay_ms(30U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "one-shot timer has not fired before its period elapses");
    os_delay_ms(50U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "one-shot timer fires exactly once (fired=%lu)",
                      (unsigned long)os_test_oneshot_fired);
    os_delay_ms(80U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "one-shot timer does not fire again on its own");

    os_test_periodic_fired = 0U;
    AHURA_TEST_CHECK(os_timer_init(&os_test_timer_periodic, OS_TICKS_FROM_MS(30U), OS_TIMER_MODE_PERIODIC,
                                    timer_periodic_cb, NULL) == OS_STATUS_OK,
                      "os_timer_init() configures a periodic timer (30 ms)");
    AHURA_TEST_CHECK(os_timer_start(&os_test_timer_periodic) == OS_STATUS_OK, "os_timer_start() arms the periodic timer");
    os_delay_ms(160U);
    AHURA_TEST_CHECK((os_test_periodic_fired >= 4U) && (os_test_periodic_fired <= 7U),
                      "periodic timer fires repeatedly (~5x expected in 160 ms, fired=%lu)",
                      (unsigned long)os_test_periodic_fired);

    AHURA_TEST_CHECK(os_timer_stop(&os_test_timer_periodic) == OS_STATUS_OK, "os_timer_stop() disarms the periodic timer");
    snapshot = os_test_periodic_fired;
    os_delay_ms(90U);
    AHURA_TEST_CHECK(os_test_periodic_fired == snapshot, "no further fires after os_timer_stop()");

    /* --- pause / resume, restart, delete --- */

    /* A 100 ms one-shot paused 40 ms in has ~60 ms left. Resuming must fire ~60 ms later, not
     * ~100 ms, which is what separates os_timer_start's resume from os_timer_restart's reload. */
    os_test_oneshot_fired = 0U;
    (void)os_timer_init(&os_test_timer_oneshot, OS_TICKS_FROM_MS(100U), OS_TIMER_MODE_ONE_SHOT,
                        timer_oneshot_cb, NULL);
    (void)os_timer_start(&os_test_timer_oneshot);
    os_delay_ms(40U);

    AHURA_TEST_CHECK(os_timer_pause(&os_test_timer_oneshot) == OS_STATUS_OK, "os_timer_pause() halts a running timer");
    os_delay_ms(150U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "a paused timer does not fire");

    (void)os_timer_start(&os_test_timer_oneshot);
    os_delay_ms(40U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "start() resumes the time left, not a full period");
    os_delay_ms(50U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "the resumed timer expires");
    AHURA_TEST_CHECK(os_timer_pause(&os_test_timer_oneshot) == OS_STATUS_ERROR, "pausing a stopped timer is an error");

    /* Restart 70 ms into a 100 ms period: the deadline moves out a whole period from now. */
    os_test_oneshot_fired = 0U;
    (void)os_timer_init(&os_test_timer_oneshot, OS_TICKS_FROM_MS(100U), OS_TIMER_MODE_ONE_SHOT,
                        timer_oneshot_cb, NULL);
    (void)os_timer_start(&os_test_timer_oneshot);
    os_delay_ms(70U);
    AHURA_TEST_CHECK(os_timer_restart(&os_test_timer_oneshot) == OS_STATUS_OK, "os_timer_restart() re-arms 70 ms in");
    os_delay_ms(50U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "restart moved the deadline");
    os_delay_ms(70U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "fires a full period after restart");

    /* Delete leaves the object needing os_timer_init before it can run again. */
    os_test_periodic_fired = 0U;
    (void)os_timer_init(&os_test_timer_periodic, OS_TICKS_FROM_MS(30U), OS_TIMER_MODE_PERIODIC,
                        timer_periodic_cb, NULL);
    (void)os_timer_start(&os_test_timer_periodic);
    os_delay_ms(50U);

    AHURA_TEST_CHECK(os_timer_delete(&os_test_timer_periodic) == OS_STATUS_OK, "os_timer_delete() tears it down");
    snapshot = os_test_periodic_fired;
    os_delay_ms(90U);
    AHURA_TEST_CHECK(os_test_periodic_fired == snapshot, "no further fires after os_timer_delete()");
    AHURA_TEST_CHECK(os_timer_start(&os_test_timer_periodic) == OS_STATUS_INVALID_ARG,
                      "a deleted timer is refused until re-init");
    (void)os_timer_init(&os_test_timer_periodic, OS_TICKS_FROM_MS(30U), OS_TIMER_MODE_PERIODIC,
                        timer_periodic_cb, NULL);
    (void)os_timer_stop(&os_test_timer_periodic);
}
#endif /* OS_CONFIG_TIMER_ENABLE */

/*
 * ***********************************************************************************************************
 * Work queue
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_WORK_ENABLE == 1U)
/* Payload copy check, sized to whatever the build allows so it exercises the largest submission
 * the configuration accepts, whatever that is. Every byte differs from its neighbours, so a short
 * memcpy, an off-by-one or a byte-swapped copy all show up rather than cancelling out. */
#define TEST_WORK_PAYLOAD_BYTES  ((OS_CONFIG_WORK_PAYLOAD_SIZE > 0U) ? OS_CONFIG_WORK_PAYLOAD_SIZE : 1U)
#define TEST_WORK_PAYLOAD_BYTE(i)  ((uint8_t)(0xA5U ^ (uint8_t)(i)))

static __IO bool os_test_work_payload_ok = false;

/******************************************************************************************************/
static void test_work_payload_handler(void *data, size_t len)
{
    const uint8_t *received = (const uint8_t *)data;
    bool          ok        = (data != NULL) && (len == OS_CONFIG_WORK_PAYLOAD_SIZE);
    size_t        index;

    for (index = 0U; ok && (index < len); index++)
    {
        if (received[index] != TEST_WORK_PAYLOAD_BYTE(index))
        {
            ok = false;
        }
    }

    os_test_work_payload_ok = ok;
}

/******************************************************************************************************/
static void work_handler(void *data, size_t len)
{
    (void)data;
    (void)len;
    os_test_work_ran = true;
    os_test_work_run_count++;
}

/******************************************************************************************************/
static void test_work(void)
{
    uint32_t snapshot;

    test_print_section("Work Queue");

    os_test_work_ran = false;
    AHURA_TEST_CHECK(os_work_submit(work_handler, NULL, 0U, 0U) == OS_STATUS_OK, "os_work_submit(delay=0) is accepted");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_work_ran, "zero-delay work runs almost immediately");

    os_test_work_run_count = 0U;
    AHURA_TEST_CHECK(os_work_submit(work_handler, NULL, 0U, 80U) == OS_STATUS_OK, "os_work_submit(delay=80ms) is accepted");
    os_delay_ms(30U);
    AHURA_TEST_CHECK(os_test_work_run_count == 0U, "delayed work has not run yet (30/80 ms)");
    os_delay_ms(80U);
    AHURA_TEST_CHECK(os_test_work_run_count == 1U, "delayed work ran once its delay elapsed");

    /* No handle means no rescheduling: two submissions are two calls. */
    os_test_work_run_count = 0U;
    (void)os_work_submit(work_handler, NULL, 0U, 40U);
    (void)os_work_submit(work_handler, NULL, 0U, 40U);
    os_delay_ms(120U);
    AHURA_TEST_CHECK(os_test_work_run_count == 2U, "the same handler submitted twice runs twice (ran=%lu)",
                      (unsigned long)os_test_work_run_count);

    AHURA_TEST_CHECK(os_work_submit(NULL, NULL, 0U, 0U) == OS_STATUS_INVALID_ARG, "a NULL handler is refused");
    AHURA_TEST_CHECK(os_work_submit(work_handler, NULL, 0U, OS_WAIT_FOREVER) == OS_STATUS_INVALID_ARG,
                      "OS_WAIT_FOREVER is refused as a delay");
    AHURA_TEST_CHECK(os_work_submit(work_handler, NULL, 4U, 0U) == OS_STATUS_INVALID_ARG,
                      "a nonzero len with NULL data is refused");
    AHURA_TEST_CHECK(os_work_submit(work_handler, "x", OS_CONFIG_WORK_PAYLOAD_SIZE + 1U, 0U) == OS_STATUS_INVALID_ARG,
                      "a payload past OS_CONFIG_WORK_PAYLOAD_SIZE is refused, not truncated");

    /* The payload is copied, so a buffer that dies before the handler runs is still fine. Sized to
     * the configured maximum, so this is also the widest copy the build can be asked for. */
    os_test_work_payload_ok = false;
    {
        uint8_t local[TEST_WORK_PAYLOAD_BYTES];
        size_t  index;

        for (index = 0U; index < OS_CONFIG_WORK_PAYLOAD_SIZE; index++)
        {
            local[index] = TEST_WORK_PAYLOAD_BYTE(index);
        }

        (void)os_work_submit(test_work_payload_handler, local, OS_CONFIG_WORK_PAYLOAD_SIZE, 40U);

        /* Clobbered after submit: the copy the kernel took must be unaffected. */
        for (index = 0U; index < OS_CONFIG_WORK_PAYLOAD_SIZE; index++)
        {
            local[index] = 0xFFU;
        }
    }
    os_delay_ms(100U);
    AHURA_TEST_CHECK(os_test_work_payload_ok,
                      "all %u payload bytes reached the handler intact from a buffer already out of scope",
                      (unsigned)OS_CONFIG_WORK_PAYLOAD_SIZE);

    /* Every slot filled, so the next submission has nowhere to go. Long delays keep them all
     * occupied while the registry is probed, then the wait lets them drain. */
    snapshot = os_test_work_run_count;
    {
        uint32_t filled = 0U;

        while (os_work_submit(work_handler, NULL, 0U, 60U) == OS_STATUS_OK)
        {
            filled++;
        }

        AHURA_TEST_CHECK(filled == OS_CONFIG_MAX_WORKS,
                          "the registry accepts exactly OS_CONFIG_MAX_WORKS submissions (%lu)",
                          (unsigned long)filled);
    }
    os_delay_ms(150U);
    AHURA_TEST_CHECK(os_test_work_run_count == (snapshot + OS_CONFIG_MAX_WORKS),
                      "and every one of them runs, freeing its slot");
}
#endif /* OS_CONFIG_WORK_ENABLE */

/*
 * ***********************************************************************************************************
 * Task notifications
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Calls os_notify_wait(os_test_notify_wait_timeout_ms, ...) and records the result, the
 *        delivered value, and the elapsed ticks - shared body for the give-before-wait,
 *        wait-then-give, and timeout cases below (each just sets the timeout and interleaves
 *        os_notify_give differently around starting this task).
 */
static void test_notify_wait_entry(void *context)
{
    uint32_t  start = os_tick_get();
    uint32_t  value = 0U;
    os_status status;

    (void)context;

    status = os_notify_wait(os_test_notify_wait_timeout_ms, &value);

    os_test_notify_wait_status = status;
    os_test_notify_wait_value  = value;
    os_test_notify_wait_ticks  = os_tick_get() - start;
}

/******************************************************************************************************/
/**
 * @brief Blocks in an unrelated os_delay_ms (not a notification wait), then does a
 *        non-blocking os_notify_wait - proves a give() that arrives during the delay
 *        neither cuts it short nor is lost.
 */
static void test_notify_unrelated_block_entry(void *context)
{
    uint32_t  value = 0U;
    os_status status;

    (void)context;

    os_delay_ms(80U);
    status = os_notify_wait(OS_WAIT_NOTHING, &value);

    os_test_notify_wait_status = status;
    os_test_notify_wait_value  = value;
}

/******************************************************************************************************/
/* Waits with value_out = NULL, then immediately re-checks the mailbox: the delivery must be
 * reported AND consumed, or the second wait would find it still full. */
/******************************************************************************************************/
static void test_notify_discard_entry(void *context)
{
    (void)context;

    os_test_notify_wait_status   = os_notify_wait(OS_WAIT_FOREVER, NULL);
    os_test_notify_second_status = os_notify_wait(OS_WAIT_NOTHING, NULL);
}

/******************************************************************************************************/
static void test_task_notify(void)
{
    os_status status;
    os_task_t stale_task;
    uint32_t  t0;
    uint32_t  t1;

    test_print_section("Task Notifications");

    AHURA_TEST_CHECK(os_notify_give(NULL, 1U) == OS_STATUS_INVALID_ARG,
                      "os_notify_give(NULL) is rejected");

    stale_task.id = 0xFFFFFFF0U;
    AHURA_TEST_CHECK(os_notify_give(&stale_task, 1U) == OS_STATUS_INVALID_ARG,
                      "os_notify_give() to a stale/unknown task id is rejected");

    /* Give-before-wait: the latched value must be delivered without blocking. */
    os_test_notify_wait_timeout_ms = 500U;
    status = os_task_create(&helper, OS_TASK_CONFIG(test_notify_wait_entry, NULL, 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "give-before-wait helper created");
    AHURA_TEST_CHECK(os_notify_give(&helper, 111U) == OS_STATUS_OK,
                      "os_notify_give() to a created-but-not-started task succeeds");
    t0 = os_tick_get();
    AHURA_TEST_CHECK(os_task_start(&helper) == OS_STATUS_OK, "give-before-wait helper started");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "give-before-wait helper finished");
    t1 = os_tick_get();
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_STATUS_OK, "the latched value was delivered without blocking");
    AHURA_TEST_CHECK(os_test_notify_wait_value == 111U, "the delivered value matches (got %lu)",
                      (unsigned long)os_test_notify_wait_value);
    AHURA_TEST_CHECK((t1 - t0) < OS_TICKS_FROM_MS(100U), "delivery was immediate (elapsed=%lu ticks)",
                      (unsigned long)(t1 - t0));

    /* Wait-then-give: blocks, then wakes promptly once given. */
    os_test_notify_wait_timeout_ms = 500U;
    status = os_task_create(&worker, OS_TASK_CONFIG(test_notify_wait_entry, NULL, 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "wait-then-give helper created");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "wait-then-give helper started");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_BLOCKED,
                      "wait-then-give helper is blocked in os_notify_wait");
    AHURA_TEST_CHECK(os_notify_give(&worker, 222U) == OS_STATUS_OK, "os_notify_give() wakes it");
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "wait-then-give helper finished");
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_STATUS_OK, "the wait reports delivery, not timeout");
    AHURA_TEST_CHECK(os_test_notify_wait_value == 222U, "the delivered value matches (got %lu)",
                      (unsigned long)os_test_notify_wait_value);
    AHURA_TEST_CHECK(os_test_notify_wait_ticks < OS_TICKS_FROM_MS(200U),
                      "the wake was prompt, well under the 500ms budget (elapsed=%lu ticks)",
                      (unsigned long)os_test_notify_wait_ticks);

    /* Timeout: nobody gives. */
    os_test_notify_wait_timeout_ms = 200U;
    status = os_task_create(&helper, OS_TASK_CONFIG(test_notify_wait_entry, NULL, 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "timeout-case helper created");
    AHURA_TEST_CHECK(os_task_start(&helper) == OS_STATUS_OK, "timeout-case helper started");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 400U), "timeout-case helper finished");
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_STATUS_TIMEOUT,
                      "os_notify_wait() times out when nobody gives");
    AHURA_TEST_CHECK(os_test_notify_wait_ticks >= OS_TICKS_FROM_MS(200U),
                      "the timeout waited its full budget (elapsed=%lu ticks)",
                      (unsigned long)os_test_notify_wait_ticks);

    /* give() during an unrelated block must not cut it short, and must not be lost. */
    status = os_task_create(&worker, OS_TASK_CONFIG(test_notify_unrelated_block_entry, NULL, 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "unrelated-block helper created");
    t0 = os_tick_get();
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "unrelated-block helper started (delaying 80ms)");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_notify_give(&worker, 333U) == OS_STATUS_OK,
                      "os_notify_give() during the unrelated delay succeeds");
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 300U), "unrelated-block helper finished");
    t1 = os_tick_get();
    AHURA_TEST_CHECK((t1 - t0) >= OS_TICKS_FROM_MS(75U),
                      "the unrelated delay was not cut short by the give() (elapsed=%lu ticks)",
                      (unsigned long)(t1 - t0));
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_STATUS_OK,
                      "the latched value was not lost - picked up by the later non-blocking wait");
    AHURA_TEST_CHECK(os_test_notify_wait_value == 333U, "the delivered value matches (got %lu)",
                      (unsigned long)os_test_notify_wait_value);

    /* value_out = NULL: wait for the signal, discard the value, still consume the delivery. */
    (void)os_task_create(&helper, OS_TASK_CONFIG(test_notify_discard_entry, NULL, 3U));
    (void)os_task_start(&helper);
    os_delay_ms(20U);
    (void)os_notify_give(&helper, 444U);
    (void)test_wait_inactive(&helper, 200U);
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_STATUS_OK, "notify_wait(NULL) reports the delivery");
    AHURA_TEST_CHECK(os_test_notify_second_status == OS_STATUS_EMPTY, "and still consumed it");
}
#endif /* OS_CONFIG_NOTIFY_ENABLE */

/*
 * ***********************************************************************************************************
 * Assertions and buffered logging
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Checks that a PASSING assertion is invisible: no halt, no side effect, and the
 *        expression is evaluated exactly once when assertions are compiled in.
 *
 * A FAILING assertion deliberately parks the core, so it cannot be exercised from inside a
 * running suite - that path is verified by inspection and on hardware with a debugger.
 */
static void test_assert(void)
{
    test_print_section("Assertions");

#if (OS_CONFIG_ASSERT_ENABLE == 1U)
    {
        __IO uint32_t evaluations = 0U;

        OS_ASSERT((evaluations++, true));
        AHURA_TEST_CHECK(evaluations == 1U,
                          "a passing OS_ASSERT evaluates its expression exactly once (got %lu)",
                          (unsigned long)evaluations);
    }

    OS_ASSERT(1 == 1);
    AHURA_TEST_CHECK(os_kernel_is_running(), "a passing OS_ASSERT does not disturb the kernel");
    printf("  [INFO] a FAILING assertion parks the core by design, so it is not exercised here\r\n");
#else
    {
        __IO uint32_t evaluations = 0U;

        OS_ASSERT((evaluations++, true));
        AHURA_TEST_CHECK(evaluations == 0U,
                          "with assertions off the expression is not evaluated at all (got %lu)",
                          (unsigned long)evaluations);
    }
#endif
}

#if (OS_CONFIG_LOG_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Exercises the log ring end to end: delivery through the output hook, formatting, the
 *        level filter, and the drop-and-count behavior when the buffer overruns.
 *
 * The suite installs its own os_log_output_cb (a strong definition overriding the kernel's weak
 * one), so the bytes the log task would have transmitted are captured here instead of going to
 * the UART. That also means this file's callback is the one the whole firmware uses.
 */
static void test_log(void)
{
    uint32_t dropped_before;
    uint32_t dropped_after;
    uint32_t i;

    test_print_section("Buffered Logging");

    /* Let tsk_log drain anything the kernel or earlier sections queued. */
    os_delay_ms(50U);

    os_test_log_capture_len   = 0U;
    os_test_log_capture_lines = 0U;
    os_test_log_capture_on    = true;

    OS_LOG_INFO("selftest marker %lu", 12345UL);
    os_delay_ms(50U);

    AHURA_TEST_CHECK(os_test_log_capture_lines > 0U, "a logged line reached os_log_output_cb (%lu lines)",
                      (unsigned long)os_test_log_capture_lines);
    AHURA_TEST_CHECK(test_log_capture_contains("selftest marker 12345"),
                      "the formatted text arrived intact");
    AHURA_TEST_CHECK(test_log_capture_contains("] I "), "the line carries its severity marker");

    /* Level filter: anything above OS_CONFIG_LOG_LEVEL must not even be
     * evaluated, let alone reach the buffer. */
    os_test_log_capture_len = 0U;
    {
        __IO uint32_t evaluated = 0U;

        OS_LOG_DEBUG("filtered %lu", (unsigned long)(evaluated++));
        os_delay_ms(20U);

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_DEBUG)
        AHURA_TEST_CHECK(evaluated == 1U, "OS_LOG_DEBUG is compiled in at this level and ran");
#else
        AHURA_TEST_CHECK(evaluated == 0U,
                          "OS_LOG_DEBUG above the configured level does not evaluate its arguments");
        AHURA_TEST_CHECK(!test_log_capture_contains("filtered"),
                          "and nothing from it reaches the output hook");
#endif
    }

    /* Overrun: burst far more than the ring can hold, with the drain task
     * starved (it runs below this one), so lines must be dropped whole. */
    dropped_before = os_log_dropped_get();

    for (i = 0U; i < 500U; i++)
    {
        OS_LOG_INFO("flood %lu 0123456789 0123456789 0123456789", (unsigned long)i);
    }

    dropped_after = os_log_dropped_get();
    AHURA_TEST_CHECK(dropped_after > dropped_before,
                      "a burst larger than the buffer drops lines instead of blocking (%lu dropped)",
                      (unsigned long)(dropped_after - dropped_before));

    /* Once drained, the kernel reports the loss and resumes normal service.
     *
     * The whole ring is delivered before the notice is, so the capture has to survive a full drain
     * plus the notice; TEST_LOG_CAPTURE_SIZE is sized from the ring for exactly that. The overflow
     * flag is cleared here so the check below reports a capture that was too small as itself,
     * rather than as the kernel failing to emit anything. */
    os_test_log_capture_len      = 0U;
    os_test_log_capture_overflow = false;
    os_delay_ms(400U);

    AHURA_TEST_CHECK(!os_test_log_capture_overflow,
                      "the test capture held the whole drain (%u bytes) without discarding any",
                      (unsigned)TEST_LOG_CAPTURE_SIZE);

    /* The notice is assembled by hand rather than through vsnprintf: running the formatter on the
     * log task's own stack overflowed it, so every part of the line below is the kernel's own
     * formatting and worth checking, not libc's. */
    AHURA_TEST_CHECK(test_log_capture_contains("dropped"),
                      "the dropped count is reported into the log itself");
    AHURA_TEST_CHECK(test_log_capture_contains("*** ") &&
                      test_log_capture_contains(" log lines dropped ***"),
                      "the hand-formatted notice carries both of its delimiters");
    AHURA_TEST_CHECK(test_log_capture_contains("] W "),
                      "the notice is emitted at warning severity");
    AHURA_TEST_CHECK(os_log_dropped_get() == 0U,
                      "the dropped counter is cleared once reported (now %lu)",
                      (unsigned long)os_log_dropped_get());

    os_test_log_capture_len = 0U;
    OS_LOG_INFO("logging still works after an overrun");
    os_delay_ms(50U);
    AHURA_TEST_CHECK(test_log_capture_contains("still works"), "logging recovers after an overrun");

    AHURA_TEST_CHECK(os_kernel_is_running(), "kernel state is intact after the log stress");

    os_test_log_capture_on = false;
}
#else
/******************************************************************************************************/
static void test_log(void)
{
    test_print_section("Buffered Logging");
    printf("  [SKIP] requires OS_CONFIG_LOG_ENABLE=1\r\n");
}
#endif /* OS_CONFIG_LOG_ENABLE */

/*
 * ***********************************************************************************************************
 * Kernel heap (os_mem_alloc / os_mem_free)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
/******************************************************************************************************/
static void test_alloc(void)
{
    size_t free0;
    size_t free1;
    size_t free2;
    size_t min_free;
    void   *p1;
    void   *p2;

    test_print_section("Kernel Heap (os_mem_alloc)");

    free0 = os_mem_free_get();
    AHURA_TEST_CHECK(free0 > 0U, "heap reports free bytes at start (%lu)", (unsigned long)free0);

    p1 = os_mem_alloc(128U);
    AHURA_TEST_CHECK(p1 != NULL, "os_mem_alloc(128) succeeds");
    free1 = os_mem_free_get();
    AHURA_TEST_CHECK(free1 < free0, "free bytes decreased after alloc (%lu -> %lu)", (unsigned long)free0,
                      (unsigned long)free1);

    p2 = os_mem_alloc(64U);
    AHURA_TEST_CHECK(p2 != NULL, "a second os_mem_alloc(64) succeeds");
    AHURA_TEST_CHECK(p1 != p2, "two live allocations return distinct blocks");

    os_mem_free(p1);
    os_mem_free(p2);
    free2 = os_mem_free_get();
    AHURA_TEST_CHECK(free2 == free0, "freeing both blocks restores the original free-byte count (coalescing works)");

    min_free = os_mem_watermark_get();
    AHURA_TEST_CHECK(min_free <= free0, "watermark min-free (%lu) never exceeds the current free count (%lu)",
                      (unsigned long)min_free, (unsigned long)free0);

    AHURA_TEST_CHECK(os_mem_alloc((size_t)OS_CONFIG_HEAP_SIZE * 2U) == NULL,
                      "an allocation larger than the whole heap fails cleanly");

    os_mem_free(NULL); /* must not crash */
    AHURA_TEST_CHECK(true, "os_mem_free(NULL) is a safe no-op");
}
#endif /* OS_CONFIG_ALLOC_ENABLE */

/*
 * ***********************************************************************************************************
 * Stack watermark
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
static void test_stack_watermark(void)
{
    size_t min_free;

    test_print_section("Stack Watermark");

    AHURA_TEST_CHECK(os_task_stack_watermark_get(NULL, &min_free) == OS_STATUS_OK,
                      "os_task_stack_watermark_get(NULL) reports the calling task");
    AHURA_TEST_CHECK(min_free < OS_CONFIG_TEST_STACK_SIZE,
                      "watermark (%lu) is less than the full stack (%lu bytes)",
                      (unsigned long)min_free, (unsigned long)OS_CONFIG_TEST_STACK_SIZE);

    AHURA_TEST_CHECK(os_task_stack_watermark_get(NULL, NULL) == OS_STATUS_INVALID_ARG,
                      "a NULL output pointer is rejected");

#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
    /* The guard word the overflow check reads on every switch-out. Detection itself cannot be
     * tested here - tripping it parks the core by design - so this only proves the guard is in
     * place and that a healthy task has not disturbed it. */
    AHURA_TEST_CHECK(*(const uint32_t *)(const void *)worker_STACK == 0xA5A5A5A5UL,
                      "the stack guard word is intact at the bottom of an idle task's stack");
#endif
}
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */

/*
 * ***********************************************************************************************************
 * CPU usage
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
/******************************************************************************************************/
static void test_cpu_usage(void)
{
    uint32_t  idle_usage;
    uint32_t  busy_usage;
    os_status status;

    test_print_section("CPU Usage");

    /* Idle baseline: this task is the only thing besides tsk_main (mostly asleep) that could
     * run, and it spends the whole window blocked in os_delay_ms(), so usage should be low. */
    (void)os_cpu_usage_get(); /* reset the sampling window */
    os_delay_ms(300U);
    idle_usage = os_cpu_usage_get();
    AHURA_TEST_CHECK(idle_usage <= 20U, "usage stays low while nothing is busy (%lu%%)",
                      (unsigned long)idle_usage);

    /* Busy load: a lower-priority task spins without yielding for the whole window, so it runs
     * on every tick this task would otherwise be idle for (it is itself blocked in
     * os_delay_ms() below, and outranks the spinner, so the spinner only gets what idle would
     * have gotten). */
    os_test_busy_counter    = 0U;
    os_test_busy_should_run = true;
    status = os_task_create(&worker, OS_TASK_CONFIG(test_busy_spin_entry, NULL, TEST_PRIO_LOW));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "busy worker task created to load the CPU (priority 1)");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "busy worker task started");

    (void)os_cpu_usage_get(); /* reset the sampling window right before the load starts */
    os_delay_ms(300U);
    busy_usage = os_cpu_usage_get();
    AHURA_TEST_CHECK(busy_usage >= 90U, "usage rises sharply under a busy lower-priority task (%lu%%)",
                      (unsigned long)busy_usage);
    AHURA_TEST_CHECK(os_test_busy_counter > 0U, "the busy worker actually made progress (count=%lu)",
                      (unsigned long)os_test_busy_counter);

    os_test_busy_should_run = false;
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "busy worker task stops cleanly");
}
#endif /* OS_CONFIG_CPU_USAGE_ENABLE */

/*
 * ***********************************************************************************************************
 * Integration / Combined Scenarios: several primitives at once, driven by several concurrent
 * tasks - the single-primitive tests above each involve at most one helper task; these prove
 * the primitives compose correctly under real multi-task contention, not just in isolation.
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Sends ctx->count items (ctx->base_value .. +count-1) into the shared pipeline queue,
 *        blocking whenever it is full - one of two producers running concurrently.
 */
static void test_pipeline_producer_entry(void *context)
{
    const test_producer_ctx_t *ctx = (const test_producer_ctx_t *)context;
    uint32_t                  i;

    for (i = 0U; i < ctx->count; i++)
    {
        uint32_t value = ctx->base_value + i;

        (void)os_queue_send(&os_test_queue, &value, OS_WAIT_FOREVER);
    }
}

/******************************************************************************************************/
/**
 * @brief Drains the shared pipeline queue, accumulating into a mutex-protected running total -
 *        one of two consumers running concurrently, so the mutex is under real contention:
 *        if it ever failed to serialize the read-modify-write, the total would come out wrong.
 *        Stops once the known total item count has been processed (by either consumer), or
 *        after a receive timeout (the other consumer got the last item).
 */
static void test_pipeline_consumer_entry(void *context)
{
    (void)context;

    for (;;)
    {
        uint32_t  value;
        os_status status;
        bool      done;

        status = os_queue_receive(&os_test_queue, &value, 300U);
        if (status != OS_STATUS_OK)
        {
            break;
        }

        (void)os_mutex_lock(&os_test_pipeline_mutex, OS_WAIT_FOREVER);
        os_test_pipeline_total     += value;
        os_test_pipeline_processed += 1U;
        done                  = (os_test_pipeline_processed >= TEST_PIPELINE_TOTAL_ITEMS);
        (void)os_mutex_unlock(&os_test_pipeline_mutex);

        if (done)
        {
            break;
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Two producers and two consumers share a queue (capacity 3, far smaller than the 12
 *        items produced, so both directions really block) and a mutex-protected accumulator.
 *        The pass criterion is an exact sum: any lost mutex update or dropped/duplicated queue
 *        item would show up as a wrong total, not just "some items arrived".
 */
static void test_pipeline(void)
{
    os_status status;
    uint32_t  expected_total = 0U;
    uint32_t  i;

    test_print_section("Combined: Queue + Mutex, 2 producers + 2 consumers");

    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_queue) == OS_STATUS_OK,
                      "pipeline queue emptied and reused (capacity %u, %u items will be produced)",
                      (unsigned)os_test_queue.capacity, (unsigned)TEST_PIPELINE_TOTAL_ITEMS);
    AHURA_TEST_CHECK(os_mutex_init(&os_test_pipeline_mutex) == OS_STATUS_OK, "pipeline mutex initialized");

    os_test_pipeline_total     = 0U;
    os_test_pipeline_processed = 0U;

    os_test_producer_ctx[0].base_value = 0U;
    os_test_producer_ctx[0].count      = TEST_PIPELINE_ITEMS_PER_PRODUCER;
    os_test_producer_ctx[1].base_value = 100U;
    os_test_producer_ctx[1].count      = TEST_PIPELINE_ITEMS_PER_PRODUCER;

    for (i = 0U; i < TEST_PIPELINE_ITEMS_PER_PRODUCER; i++)
    {
        expected_total += (os_test_producer_ctx[0].base_value + i);
        expected_total += (os_test_producer_ctx[1].base_value + i);
    }

    /* Consumers at a higher priority than producers so they drain the small queue promptly,
     * keeping both producers genuinely blocking on a full queue rather than racing ahead. */
    status = os_task_create(&helper2, OS_TASK_CONFIG(test_pipeline_consumer_entry, NULL, 4U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "consumer task 1 created (priority 4)");
    status = os_task_create(&helper3, OS_TASK_CONFIG(test_pipeline_consumer_entry, NULL, 4U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "consumer task 2 created (priority 4)");
    status = os_task_create(&worker, OS_TASK_CONFIG(test_pipeline_producer_entry, &os_test_producer_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "producer task 1 created (priority 3, values 0-5)");
    status = os_task_create(&helper, OS_TASK_CONFIG(test_pipeline_producer_entry, &os_test_producer_ctx[1], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "producer task 2 created (priority 3, values 100-105)");

    (void)os_task_start(&helper2);
    (void)os_task_start(&helper3);
    (void)os_task_start(&worker);
    (void)os_task_start(&helper);

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 1000U), "producer 1 finished sending its items");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 1000U), "producer 2 finished sending its items");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 1000U), "consumer 1 drained and stopped");
    AHURA_TEST_CHECK(test_wait_inactive(&helper3, 1000U), "consumer 2 drained and stopped");

    AHURA_TEST_CHECK(os_test_pipeline_processed == TEST_PIPELINE_TOTAL_ITEMS,
                      "both consumers together processed all %u items (processed=%lu)",
                      (unsigned)TEST_PIPELINE_TOTAL_ITEMS, (unsigned long)os_test_pipeline_processed);
    AHURA_TEST_CHECK(os_test_pipeline_total == expected_total,
                      "mutex-protected total is exact under two-consumer contention (got=%lu expected=%lu)",
                      (unsigned long)os_test_pipeline_total, (unsigned long)expected_total);
}
#endif /* OS_CONFIG_QUEUE_ENABLE && OS_CONFIG_MUTEX_ENABLE */

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Locks os_test_prio_mutex (blocking until granted), records ctx->priority_tag as the next
 *        entry in the shared wake-order log, then unlocks and exits.
 */
static void test_prio_waiter_entry(void *context)
{
    const test_prio_ctx_t *ctx = (const test_prio_ctx_t *)context;

    (void)os_mutex_lock(&os_test_prio_mutex, OS_WAIT_FOREVER);
    os_test_prio_order[os_test_prio_order_count] = ctx->priority_tag;
    os_test_prio_order_count++;
    (void)os_mutex_unlock(&os_test_prio_mutex);
}

/******************************************************************************************************/
/**
 * @brief Three tasks at three different priorities (started low-to-high, to rule out arrival
 *        order) all block on a mutex this test task holds; releasing it must wake them
 *        highest-priority-first, not creation/arrival order - proving the mutex waiter list is
 *        genuinely priority-ordered under contention from more than one waiter (the
 *        single-waiter test_mutex() above cannot distinguish priority order from FIFO order).
 */
static void test_mutex_priority_ordering(void)
{
    os_status status;

    test_print_section("Combined: Mutex + Priority, ordered contention across 3 tasks");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_prio_mutex) == OS_STATUS_OK, "priority-contention mutex initialized");
    AHURA_TEST_CHECK(os_mutex_lock(&os_test_prio_mutex, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "test task takes the mutex first, so all 3 waiters below must block");

    os_test_prio_order_count         = 0U;
    os_test_prio_ctx[0].priority_tag = 4U;
    os_test_prio_ctx[1].priority_tag = 5U;
    os_test_prio_ctx[2].priority_tag = 6U;

    status = os_task_create(&worker, OS_TASK_CONFIG(test_prio_waiter_entry, &os_test_prio_ctx[0], 4U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "low-priority waiter created (priority 4)");
    status = os_task_create(&helper, OS_TASK_CONFIG(test_prio_waiter_entry, &os_test_prio_ctx[1], 5U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "medium-priority waiter created (priority 5)");
    status = os_task_create(&helper2, OS_TASK_CONFIG(test_prio_waiter_entry, &os_test_prio_ctx[2], 6U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "high-priority waiter created (priority 6)");

    /* Start low first, high last: if the wake order below still comes out high-to-low, that
     * proves it is driven by priority, not by creation/start order. */
    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_task_start(&helper2);

    os_delay_ms(30U); /* let all 3 reach os_mutex_lock() and join the waiter list */

    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_prio_mutex) == OS_STATUS_OK,
                      "test task releases the mutex with all 3 tasks queued");

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 300U), "low-priority waiter finished");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 300U), "medium-priority waiter finished");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 300U), "high-priority waiter finished");

    AHURA_TEST_CHECK(os_test_prio_order_count == 3U, "all 3 waiters recorded their turn (count=%lu)",
                      (unsigned long)os_test_prio_order_count);
    AHURA_TEST_CHECK((os_test_prio_order[0] == 6U) && (os_test_prio_order[1] == 5U) && (os_test_prio_order[2] == 4U),
                      "mutex was granted highest-priority-first, not arrival order (got %lu,%lu,%lu)",
                      (unsigned long)os_test_prio_order[0], (unsigned long)os_test_prio_order[1],
                      (unsigned long)os_test_prio_order[2]);
}
#endif /* OS_CONFIG_MUTEX_ENABLE */

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Blocks on os_test_inherit_mutex (held by the test task), which boosts the test task's
 *        effective priority; once granted, records completion and releases it.
 */
static void test_inherit_high_entry(void *context)
{
    (void)context;

    (void)os_mutex_lock(&os_test_inherit_mutex, OS_WAIT_FOREVER);
    os_test_inherit_high_done = true;
    (void)os_mutex_unlock(&os_test_inherit_mutex);
}

/******************************************************************************************************/
/**
 * @brief Burns a fixed number of cycles incrementing os_test_inherit_medium_counter then returns -
 *        same shape as test_burst_spin_entry, but exposes its progress through a shared counter
 *        so test_mutex_priority_inheritance() can prove it got zero CPU time while boosted.
 */
static void test_inherit_medium_entry(void *context)
{
    __IO uint32_t i;

    (void)context;

    for (i = 0U; i < TEST_BURST_ITERATIONS; i++)
    {
        os_test_inherit_medium_counter++;
    }
}

/******************************************************************************************************/
/**
 * @brief Proves single-level mutex priority inheritance closes the classic priority-inversion
 *        window: while this test task - boosted to the blocked high-priority waiter's priority -
 *        holds the mutex, an unrelated medium-priority task must get zero CPU time, and only
 *        runs once the mutex is released and the boost drops back to base priority.
 */
static void test_mutex_priority_inheritance(void)
{
    os_status status;

    test_print_section("Combined: Mutex Priority Inheritance");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_inherit_mutex) == OS_STATUS_OK, "priority-inheritance mutex initialized");
    AHURA_TEST_CHECK(os_mutex_lock(&os_test_inherit_mutex, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "test task takes the mutex first (at its own priority %u)",
                      (unsigned)OS_CONFIG_TEST_PRIORITY);

    os_test_inherit_high_done      = false;
    os_test_inherit_medium_counter = 0U;

    /* Higher priority than this test task: preempts immediately, finds the mutex locked, and
     * boosts this test task's effective priority before blocking - synchronously, inside this
     * os_task_start() call, so the test task resumes already boosted. */
    status = os_task_create(&helper, OS_TASK_CONFIG(test_inherit_high_entry, NULL,
                                                            OS_CONFIG_TEST_PRIORITY + 2U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "high-priority waiter created (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 2U));
    AHURA_TEST_CHECK(os_task_start(&helper) == OS_STATUS_OK, "high-priority waiter started");

    AHURA_TEST_CHECK(!os_test_inherit_high_done,
                      "the high-priority waiter blocked on the held mutex instead of finishing");

    /* A medium-priority task, created and started while this test task is (boosted) running: it
     * must not get any CPU time yet - proving the boost, not just "it'll run eventually". */
    status = os_task_create(&worker, OS_TASK_CONFIG(test_inherit_medium_entry, NULL,
                                                             OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "medium-priority task created (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "medium-priority task started");

    AHURA_TEST_CHECK(os_test_inherit_medium_counter == 0U,
                      "medium-priority task got zero CPU time while the boosted owner held the mutex (count=%lu)",
                      (unsigned long)os_test_inherit_medium_counter);

    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_inherit_mutex) == OS_STATUS_OK,
                      "test task releases the mutex, dropping its boost back to base priority");

    AHURA_TEST_CHECK(test_wait_inactive(&helper, 300U), "high-priority waiter finished");
    AHURA_TEST_CHECK(os_test_inherit_high_done, "high-priority waiter actually acquired the mutex");

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 300U), "medium-priority task finished");
    AHURA_TEST_CHECK(os_test_inherit_medium_counter == TEST_BURST_ITERATIONS,
                      "medium-priority task ran to completion once nothing outranked it any more (count=%lu)",
                      (unsigned long)os_test_inherit_medium_counter);
}

/******************************************************************************************************/
/**
 * @brief Blocks on the mutex named by its context, records its tag once granted, releases it.
 *        Two of these run at different priorities against two different mutexes held by the same
 *        owner - see test_mutex_multi_inheritance().
 */
static void test_inherit2_waiter_entry(void *context)
{
    const test_inherit2_ctx_t *ctx = (const test_inherit2_ctx_t *)context;

    (void)os_mutex_lock(ctx->mutex, OS_WAIT_FOREVER);
    os_test_inherit2_done_mask |= ctx->tag;
    (void)os_mutex_unlock(ctx->mutex);
}

/******************************************************************************************************/
/**
 * @brief The case a single-mutex inheritance test cannot reach: ONE task holding TWO contended
 *        mutexes at once.
 *
 * The test task holds mutex A and mutex B. Waiter HIGH (+2) blocks on A, then waiter HIGHER (+3)
 * blocks on B, boosting the owner twice. Releasing B must drop the owner only to +2 - the boost
 * mutex A's waiter is still owed - NOT all the way back to base. That distinction is the whole
 * point of recomputing against every still-held mutex, and a "just revert to base_priority on
 * unlock" implementation passes the single-mutex test while failing here: the medium task (+1)
 * would get CPU time it must not have while A is still held and contended.
 */
static void test_mutex_multi_inheritance(void)
{
    os_status status;

    test_print_section("Combined: Mutex Priority Inheritance across TWO held mutexes");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_inherit2_mutex_a) == OS_STATUS_OK, "mutex A initialized");
    AHURA_TEST_CHECK(os_mutex_init(&os_test_inherit2_mutex_b) == OS_STATUS_OK, "mutex B initialized");

    AHURA_TEST_CHECK(os_mutex_lock(&os_test_inherit2_mutex_a, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "test task takes mutex A (at its own priority %u)", (unsigned)OS_CONFIG_TEST_PRIORITY);
    AHURA_TEST_CHECK(os_mutex_lock(&os_test_inherit2_mutex_b, OS_WAIT_NOTHING) == OS_STATUS_OK,
                      "test task takes mutex B as well - two mutexes held at once");

    os_test_inherit2_done_mask     = 0U;
    os_test_inherit_medium_counter = 0U;

    os_test_inherit2_ctx[0].mutex = &os_test_inherit2_mutex_a;
    os_test_inherit2_ctx[0].tag   = 1U;
    os_test_inherit2_ctx[1].mutex = &os_test_inherit2_mutex_b;
    os_test_inherit2_ctx[1].tag   = 2U;

    /* HIGH blocks on A: boosts the owner to +2 (synchronously, inside os_task_start). */
    status = os_task_create(&helper, OS_TASK_CONFIG(test_inherit2_waiter_entry, &os_test_inherit2_ctx[0],
                                                            OS_CONFIG_TEST_PRIORITY + 2U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "waiter HIGH created for mutex A (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 2U));
    AHURA_TEST_CHECK(os_task_start(&helper) == OS_STATUS_OK, "waiter HIGH started");

    /* HIGHER blocks on B: boosts the owner again, to +3. */
    status = os_task_create(&helper2, OS_TASK_CONFIG(test_inherit2_waiter_entry, &os_test_inherit2_ctx[1],
                                                             OS_CONFIG_TEST_PRIORITY + 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "waiter HIGHER created for mutex B (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 3U));
    AHURA_TEST_CHECK(os_task_start(&helper2) == OS_STATUS_OK, "waiter HIGHER started");

    AHURA_TEST_CHECK(os_test_inherit2_done_mask == 0U,
                      "both waiters blocked on the held mutexes instead of finishing (mask=%lu)",
                      (unsigned long)os_test_inherit2_done_mask);

    /* Medium (+1) must stay starved for as long as ANY boost is in effect. */
    status = os_task_create(&worker, OS_TASK_CONFIG(test_inherit_medium_entry, NULL,
                                                             OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "medium-priority task created (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_STATUS_OK, "medium-priority task started");
    AHURA_TEST_CHECK(os_test_inherit_medium_counter == 0U,
                      "medium task got no CPU while the owner is boosted to +3 (count=%lu)",
                      (unsigned long)os_test_inherit_medium_counter);

    /* Release B only. HIGHER wakes, takes B and finishes; the owner must settle at +2 (still
     * owed to A's waiter), so medium STILL must not run. */
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_inherit2_mutex_b) == OS_STATUS_OK, "test task releases mutex B");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 300U), "waiter HIGHER finished after B was released");
    AHURA_TEST_CHECK((os_test_inherit2_done_mask & 2U) != 0U, "waiter HIGHER actually acquired mutex B");

    AHURA_TEST_CHECK((os_test_inherit2_done_mask & 1U) == 0U,
                      "waiter HIGH is still blocked - mutex A was never released");
    AHURA_TEST_CHECK(os_test_inherit_medium_counter == 0U,
                      "THE KEY CHECK: releasing B kept the boost A's waiter is still owed, so the "
                      "medium task still got zero CPU (count=%lu)",
                      (unsigned long)os_test_inherit_medium_counter);

    /* Release A: no held mutex left, so the owner finally drops to base and medium is free. */
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_inherit2_mutex_a) == OS_STATUS_OK,
                      "test task releases mutex A, dropping the last boost to base priority");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 300U), "waiter HIGH finished after A was released");
    AHURA_TEST_CHECK((os_test_inherit2_done_mask & 1U) != 0U, "waiter HIGH actually acquired mutex A");

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 300U), "medium-priority task finished");
    AHURA_TEST_CHECK(os_test_inherit_medium_counter == TEST_BURST_ITERATIONS,
                      "medium task ran to completion once every boost was released (count=%lu)",
                      (unsigned long)os_test_inherit_medium_counter);
}
#endif /* OS_CONFIG_MUTEX_ENABLE */

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_EVENT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Waits ctx->work_ms (staggered per task so completion order is not predictable), sends
 *        ctx->value into the shared queue, then sets ctx->bit in the shared event - one
 *        of three independent workers in a fan-out/fan-in pattern.
 */
static void test_fanin_worker_entry(void *context)
{
    const test_fanin_ctx_t *ctx = (const test_fanin_ctx_t *)context;

    os_delay_ms(ctx->work_ms);
    (void)os_queue_send(&os_test_queue, &ctx->value, OS_WAIT_FOREVER);
    (void)os_event_set_bits(&os_test_event, ctx->bit);
}

/******************************************************************************************************/
/**
 * @brief Three tasks each do "work" for a different duration, then deliver a queue item and set
 *        their own event bit. The test task wait-alls on all 3 bits (proving the event
 *        correctly rendezvous-es 3 independent, differently-timed setters) then drains the
 *        queue and checks the exact multiset of values arrived - order-independent, since which
 *        worker finishes first is not deterministic.
 */
static void test_event_queue_fanin(void)
{
    uint32_t  matched;
    os_status status;
    uint32_t  received[3] = { 0 };
    uint32_t  i;
    uint32_t  sum          = 0U;
    uint32_t  expected_sum;
    bool      saw[3]       = { false, false, false };

    test_print_section("Combined: Events + Queue, fan-out/fan-in across 3 tasks");

    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_queue) == OS_STATUS_OK,
                      "fan-in queue emptied and reused (capacity %u, one slot per worker)",
                      (unsigned)os_test_queue.capacity);
    AHURA_TEST_CHECK(os_event_init(&os_test_event) == OS_STATUS_OK, "fan-in event initialized");

    os_test_fanin_ctx[0].bit = 0x01U; os_test_fanin_ctx[0].value = 10U; os_test_fanin_ctx[0].work_ms = 60U;
    os_test_fanin_ctx[1].bit = 0x02U; os_test_fanin_ctx[1].value = 20U; os_test_fanin_ctx[1].work_ms = 20U;
    os_test_fanin_ctx[2].bit = 0x04U; os_test_fanin_ctx[2].value = 30U; os_test_fanin_ctx[2].work_ms = 40U;
    expected_sum = os_test_fanin_ctx[0].value + os_test_fanin_ctx[1].value + os_test_fanin_ctx[2].value;

    status = os_task_create(&worker, OS_TASK_CONFIG(test_fanin_worker_entry, &os_test_fanin_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "fan-in worker 1 created (bit 0x01, 60 ms work)");
    status = os_task_create(&helper, OS_TASK_CONFIG(test_fanin_worker_entry, &os_test_fanin_ctx[1], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "fan-in worker 2 created (bit 0x02, 20 ms work)");
    status = os_task_create(&helper2, OS_TASK_CONFIG(test_fanin_worker_entry, &os_test_fanin_ctx[2], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "fan-in worker 3 created (bit 0x04, 40 ms work)");

    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_task_start(&helper2);

    status = os_event_wait_bits(&os_test_event, 0x07U, true, false, &matched, 500U);
    AHURA_TEST_CHECK((status == OS_STATUS_OK) && (matched == 0x07U),
                      "wait-all sees all 3 workers' bits despite different finish times (matched=0x%02lx)",
                      (unsigned long)matched);

    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 3U, "queue holds exactly the 3 workers' results");

    for (i = 0U; i < 3U; i++)
    {
        AHURA_TEST_CHECK(os_queue_receive(&os_test_queue, &received[i], OS_WAIT_NOTHING) == OS_STATUS_OK,
                          "received fan-in result #%lu", (unsigned long)i);
        sum += received[i];

        if (received[i] == 10U) { saw[0] = true; }
        if (received[i] == 20U) { saw[1] = true; }
        if (received[i] == 30U) { saw[2] = true; }
    }

    AHURA_TEST_CHECK(sum == expected_sum, "the 3 delivered values sum correctly (got=%lu expected=%lu)",
                      (unsigned long)sum, (unsigned long)expected_sum);
    AHURA_TEST_CHECK(saw[0] && saw[1] && saw[2],
                      "all 3 distinct worker values arrived exactly once each, in any order");

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 300U), "fan-in worker 1 terminated cleanly");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 300U), "fan-in worker 2 terminated cleanly");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 300U), "fan-in worker 3 terminated cleanly");
}
#endif /* OS_CONFIG_QUEUE_ENABLE && OS_CONFIG_EVENT_ENABLE */

#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
    (OS_CONFIG_EVENT_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
/*
 * ***********************************************************************************************************
 * Stress/Soak: several tasks contend on every primitive at once (see the type/object block near
 * the top of this file for OS_TEST_STRESS_* and the rationale)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Small, fast xorshift32 PRNG - just enough spread to pick different operations and
 *        sizes per worker per iteration; not meant to be statistically strong.
 */
static uint32_t test_stress_prng_next(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;

    return x;
}

/******************************************************************************************************/
/**
 * @brief One stress worker: OS_TEST_STRESS_ITERATIONS times, pick one of 5 operations at
 *        random and do it. Every operation either self-verifies (pattern-filled heap memory
 *        read back unchanged, a received queue item decodes to a plausible sender/sequence) or
 *        feeds a counter the parent checks after every worker has finished (successful mutex
 *        locks vs. the shared counter they protect).
 */
static void test_stress_worker_entry(void *context)
{
    test_stress_ctx_t *ctx = (test_stress_ctx_t *)context;
    uint32_t           iteration;

    for (iteration = 0U; iteration < OS_TEST_STRESS_ITERATIONS; iteration++)
    {
        uint32_t pick = test_stress_prng_next(&ctx->prng_state) % 5U;

        switch (pick)
        {
            case 0U: /* mutex: protected read-modify-write; os_test_stress_shared_counter must end up
                      * exactly equal to the total successful locks across every worker below, or
                      * the lock let two tasks in at once and a lost update reveals it. */
            {
                if (os_mutex_lock(&os_test_stress_mutex, 20U) == OS_STATUS_OK)
                {
                    uint32_t before = os_test_stress_shared_counter;

                    os_task_yield(); /* widen the window: a broken lock would let another worker in here */
                    os_test_stress_shared_counter = before + 1U;
                    (void)os_mutex_unlock(&os_test_stress_mutex);
                    os_test_stress_mutex_hits[ctx->worker_id]++;
                }
                break;
            }

            case 1U: /* semaphore: take then give back, so the run is self-balancing */
            {
                if (os_semaphore_take(&os_test_stress_sem, 5U) == OS_STATUS_OK)
                {
                    os_delay_ms(test_stress_prng_next(&ctx->prng_state) % 3U);
                    (void)os_semaphore_give(&os_test_stress_sem);
                }
                break;
            }

            case 2U: /* queue: send and receive both, so the queue is self-draining; a received
                      * tag that does not decode to a real sender/sequence means corruption. */
            {
                uint32_t tag = (ctx->worker_id << 16) | (iteration & 0xFFFFU);
                uint32_t received;

                if ((test_stress_prng_next(&ctx->prng_state) & 1U) != 0U)
                {
                    (void)os_queue_send(&os_test_stress_queue, &tag, 5U);
                }
                else if (os_queue_receive(&os_test_stress_queue, &received, 5U) == OS_STATUS_OK)
                {
                    uint32_t sender = received >> 16;
                    uint32_t seq    = received & 0xFFFFU;

                    if ((sender >= OS_TEST_STRESS_WORKER_COUNT) || (seq >= OS_TEST_STRESS_ITERATIONS))
                    {
                        os_test_stress_corrupt[ctx->worker_id] = true;
                    }
                }
                break;
            }

            case 3U: /* event: set a couple of bits, then a short bounded wait - mainly
                      * here to add concurrent set/wait/clear-on-exit pressure on top of the rest. */
            {
                uint32_t bit     = 1UL << (test_stress_prng_next(&ctx->prng_state) % 4U);
                uint32_t matched = 0U;

                (void)os_event_set_bits(&os_test_stress_event, bit);
                (void)os_event_wait_bits(&os_test_stress_event, bit, false, true, &matched, 2U);
                break;
            }

            case 4U: /* kernel heap: alloc, pattern-fill, verify, free - catches corruption/overlap */
            default:
            {
                size_t  size = 1U + (test_stress_prng_next(&ctx->prng_state) % 64U);
                uint8_t *mem = (uint8_t *)os_mem_alloc(size);

                if (mem != NULL)
                {
                    uint8_t pattern = (uint8_t)(ctx->worker_id + iteration);
                    size_t  i;

                    for (i = 0U; i < size; i++) { mem[i] = pattern; }
                    os_task_yield(); /* widen the window for a racy allocator to let it overlap */
                    for (i = 0U; i < size; i++)
                    {
                        if (mem[i] != pattern) { os_test_stress_corrupt[ctx->worker_id] = true; }
                    }
                    os_mem_free(mem);
                }
                break;
            }
        }
    }

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    (void)os_task_stack_watermark_get(NULL, &os_test_stress_watermark[ctx->worker_id]);
#endif

    os_test_stress_done[ctx->worker_id] = iteration;
}

/******************************************************************************************************/
/**
 * @brief Concurrent multi-primitive stress/soak: OS_TEST_STRESS_WORKER_COUNT tasks at distinct
 *        priorities hit a mutex, an under-provisioned semaphore and queue, an event, and
 *        the kernel heap simultaneously and repeatedly, then the results are checked against
 *        hard invariants (exact mutex-protected counter, exact semaphore token reconciliation,
 *        no heap leak, no pattern/queue corruption) rather than just "the call returned OK".
 *        Unlike every test above, several DIFFERENT primitives are under contention from
 *        several tasks at once for many iterations, so this is the closest thing in the suite
 *        to actually shaking out a wakeup-ordering or allocator race instead of only ever
 *        exercising the one deterministic interleaving a scripted single-shot test happens to
 *        produce on a given boot.
 */
static void test_stress_soak(void)
{
    size_t    heap_before;
    size_t    heap_after;
    uint32_t  total_iterations = 0U;
    uint32_t  total_mutex_hits = 0U;
    uint32_t  drained_tokens   = 0U;
    uint32_t  leftover_items   = 0U;
    bool      any_corruption   = false;
    uint32_t  dummy;
    uint32_t  i;
    os_status status;

    test_print_section("Stress/Soak: 4 tasks contend on mutex+semaphore+queue+event+heap at once");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_stress_mutex) == OS_STATUS_OK, "stress mutex initialized");
    AHURA_TEST_CHECK(os_semaphore_init(&os_test_stress_sem, OS_TEST_STRESS_SEM_MAX, OS_TEST_STRESS_SEM_MAX) == OS_STATUS_OK,
                      "stress semaphore initialized (max=%u, deliberately < %u workers)",
                      (unsigned)OS_TEST_STRESS_SEM_MAX, (unsigned)OS_TEST_STRESS_WORKER_COUNT);
    AHURA_TEST_CHECK(os_event_init(&os_test_stress_event) == OS_STATUS_OK, "stress event initialized");
    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_stress_queue) == OS_STATUS_OK,
                      "stress queue emptied and reused (capacity=%u, deliberately < %u workers)",
                      (unsigned)os_test_stress_queue.capacity, (unsigned)OS_TEST_STRESS_WORKER_COUNT);

    os_test_stress_shared_counter = 0U;
    heap_before = os_mem_free_get();

    for (i = 0U; i < OS_TEST_STRESS_WORKER_COUNT; i++)
    {
        os_test_stress_done[i]       = 0U;
        os_test_stress_corrupt[i]    = false;
        os_test_stress_mutex_hits[i] = 0U;
        os_test_stress_watermark[i]  = 0U;
        os_test_stress_ctx[i].worker_id  = i;
        os_test_stress_ctx[i].prng_state = 0x9E3779B9U ^ (i * 0x2545F491U) ^ (os_tick_get() | 1U);
    }

    status = os_task_create(&worker, OS_TASK_CONFIG(test_stress_worker_entry, &os_test_stress_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "stress worker 0 created (priority 3)");
    status = os_task_create(&helper, OS_TASK_CONFIG(test_stress_worker_entry, &os_test_stress_ctx[1], 4U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "stress worker 1 created (priority 4)");
    status = os_task_create(&helper2, OS_TASK_CONFIG(test_stress_worker_entry, &os_test_stress_ctx[2], 5U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "stress worker 2 created (priority 5)");
    status = os_task_create(&helper3, OS_TASK_CONFIG(test_stress_worker_entry, &os_test_stress_ctx[3], 6U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "stress worker 3 created (priority 6)");

    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_task_start(&helper2);
    (void)os_task_start(&helper3);

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 15000U), "stress worker 0 terminated cleanly (no deadlock/hang)");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 15000U), "stress worker 1 terminated cleanly (no deadlock/hang)");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 15000U), "stress worker 2 terminated cleanly (no deadlock/hang)");
    AHURA_TEST_CHECK(test_wait_inactive(&helper3, 15000U), "stress worker 3 terminated cleanly (no deadlock/hang)");

    for (i = 0U; i < OS_TEST_STRESS_WORKER_COUNT; i++)
    {
        total_iterations += os_test_stress_done[i];
        total_mutex_hits += os_test_stress_mutex_hits[i];
        any_corruption    = any_corruption || os_test_stress_corrupt[i];
    }

    AHURA_TEST_CHECK(total_iterations == (OS_TEST_STRESS_WORKER_COUNT * OS_TEST_STRESS_ITERATIONS),
                      "all workers completed every iteration (%lu of %lu total)",
                      (unsigned long)total_iterations, (unsigned long)(OS_TEST_STRESS_WORKER_COUNT * OS_TEST_STRESS_ITERATIONS));

    AHURA_TEST_CHECK(!any_corruption, "no worker observed corrupted heap memory or a malformed queue item");

    AHURA_TEST_CHECK(os_test_stress_shared_counter == total_mutex_hits,
                      "mutex gave exclusive access every time (counter=%lu, successful locks=%lu - a mismatch would mean two tasks were inside at once)",
                      (unsigned long)os_test_stress_shared_counter, (unsigned long)total_mutex_hits);

    while (os_semaphore_take(&os_test_stress_sem, OS_WAIT_NOTHING) == OS_STATUS_OK)
    {
        drained_tokens++;
    }
    AHURA_TEST_CHECK(drained_tokens == OS_TEST_STRESS_SEM_MAX,
                      "every semaphore token was given back exactly once (drained %lu of %lu)",
                      (unsigned long)drained_tokens, (unsigned long)OS_TEST_STRESS_SEM_MAX);

    while (os_queue_receive(&os_test_stress_queue, &dummy, OS_WAIT_NOTHING) == OS_STATUS_OK)
    {
        uint32_t sender = dummy >> 16;
        uint32_t seq    = dummy & 0xFFFFU;

        if ((sender >= OS_TEST_STRESS_WORKER_COUNT) || (seq >= OS_TEST_STRESS_ITERATIONS))
        {
            any_corruption = true;
        }

        leftover_items++;
    }
    AHURA_TEST_CHECK(!any_corruption, "every leftover queue item (if any: %lu) still decoded to a valid sender/sequence",
                      (unsigned long)leftover_items);

    AHURA_TEST_CHECK(os_mutex_try_lock(&os_test_stress_mutex) == OS_STATUS_OK, "stress mutex ended unlocked");
    (void)os_mutex_unlock(&os_test_stress_mutex);

    heap_after = os_mem_free_get();
    AHURA_TEST_CHECK(heap_after == heap_before,
                      "kernel heap has no leak after the alloc/free churn (before=%lu after=%lu bytes free)",
                      (unsigned long)heap_before, (unsigned long)heap_after);

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    for (i = 0U; i < OS_TEST_STRESS_WORKER_COUNT; i++)
    {
        printf("  [INFO] stress worker %lu peak stack usage watermark: %lu bytes free at minimum\r\n",
               (unsigned long)i, (unsigned long)os_test_stress_watermark[i]);
    }
#endif

    printf("  [INFO] stress run: %u workers x %u iterations = %lu total operations\r\n",
           (unsigned)OS_TEST_STRESS_WORKER_COUNT, (unsigned)OS_TEST_STRESS_ITERATIONS, (unsigned long)total_iterations);
}
#endif /* OS_CONFIG_MUTEX_ENABLE && OS_CONFIG_SEMAPHORE_ENABLE && OS_CONFIG_QUEUE_ENABLE && OS_CONFIG_EVENT_ENABLE && OS_CONFIG_ALLOC_ENABLE */

/*
 * ***********************************************************************************************************
 * Additional targeted churn/stress tests: unlike test_stress_soak() above (several DIFFERENT
 * primitives contended by several concurrent tasks), each of these hammers ONE subsystem's
 * create/destroy or alloc/free path back-to-back, many times, in a tight loop from a single task.
 * The single-primitive tests earlier in this file only exercise create/delete or alloc/free a
 * handful of times each - not nearly enough repetition to shake out a slot-reuse bug, a list-
 * corruption bug, or a leak that only shows up after hundreds of cycles.
 * ***********************************************************************************************************
*/

#define OS_TEST_CHURN_ITERATIONS 500U

static __IO uint32_t os_test_churn_counter = 0U;

/******************************************************************************************************/
static void test_churn_worker_entry(void *context)
{
    (void)context;
    os_test_churn_counter++;
    /* returns immediately - self-exits via the arch port's os_task_exit() trampoline, freeing the
     * slot for the next iteration's os_task_create() as fast as the port allows. */
}

/******************************************************************************************************/
/**
 * @brief Creates, starts, and waits for a task to self-exit, back-to-back OS_TEST_CHURN_ITERATIONS
 *        times on the same slot - a create/run/exit/slot-reuse cycle the earlier lifecycle test
 *        only exercises a handful of times. Catches slot-reuse bugs (stale state left over from
 *        the previous occupant) or ready-list corruption that only show up under repeated churn.
 */
static void test_stress_task_churn(void)
{
    uint32_t  i;
    bool      all_created  = true;
    bool      all_started  = true;
    bool      all_finished = true;
    os_status status;

    test_print_section("Stress: rapid task create/start/exit churn");

    os_test_churn_counter = 0U;

    for (i = 0U; i < OS_TEST_CHURN_ITERATIONS; i++)
    {
        status = os_task_create(&worker, OS_TASK_CONFIG(test_churn_worker_entry, NULL, 1U));
        if (status != OS_STATUS_OK)
        {
            all_created = false;
            break;
        }

        status = os_task_start(&worker);
        if (status != OS_STATUS_OK)
        {
            all_started = false;
            break;
        }

        if (!test_wait_inactive(&worker, 100U))
        {
            all_finished = false;
            break;
        }
    }

    AHURA_TEST_CHECK(all_created, "task slot creates cleanly on every one of %u churn cycles",
                      (unsigned)OS_TEST_CHURN_ITERATIONS);
    AHURA_TEST_CHECK(all_started, "task starts cleanly on every churn cycle");
    AHURA_TEST_CHECK(all_finished, "task self-exits and frees its slot on every churn cycle (no leak/hang)");
    AHURA_TEST_CHECK(os_test_churn_counter == OS_TEST_CHURN_ITERATIONS,
                      "each cycle's task body ran exactly once (counter=%lu of %lu)",
                      (unsigned long)os_test_churn_counter, (unsigned long)OS_TEST_CHURN_ITERATIONS);

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    {
        size_t min_free;

        if (os_task_stack_watermark_get(&worker, &min_free) == OS_STATUS_OK)
        {
            AHURA_TEST_CHECK(min_free <= sizeof(worker_STACK),
                              "repeated slot reuse leaves a sane stack watermark (%lu / %lu bytes free)",
                              (unsigned long)min_free, (unsigned long)sizeof(worker_STACK));
        }
    }
#endif
}

#if (OS_CONFIG_TIMER_ENABLE == 1U)
#define OS_TEST_TIMER_CHURN_ITERATIONS 500U

static __IO uint32_t os_test_churn_timer_fired = 0U;

/******************************************************************************************************/
static void test_churn_timer_cb(void *context)
{
    (void)context;
    os_test_churn_timer_fired++;
}

/******************************************************************************************************/
/**
 * @brief Hammers os_timer_init()/os_timer_start()/os_timer_stop() on the same timer object back-
 *        to-back, many times, always stopping it long before its (long) period could elapse -
 *        purely to shake out add/remove bugs in the timer list under rapid churn. Finishes with
 *        one real run to prove the timer list is still healthy afterward, not just that the API
 *        calls returned OK.
 */
static void test_stress_timer_churn(void)
{
    uint32_t i;
    bool     all_ok = true;

    test_print_section("Stress: rapid timer init/start/stop churn");

    os_test_churn_timer_fired = 0U;

    for (i = 0U; i < OS_TEST_TIMER_CHURN_ITERATIONS; i++)
    {
        if (os_timer_init(&os_test_timer_oneshot, OS_TICKS_FROM_MS(1000U), OS_TIMER_MODE_ONE_SHOT, test_churn_timer_cb,
                           NULL) != OS_STATUS_OK)
        {
            all_ok = false;
            break;
        }

        if (os_timer_start(&os_test_timer_oneshot) != OS_STATUS_OK)
        {
            all_ok = false;
            break;
        }

        if (os_timer_stop(&os_test_timer_oneshot) != OS_STATUS_OK)
        {
            all_ok = false;
            break;
        }
    }

    AHURA_TEST_CHECK(all_ok, "timer init/start/stop succeeds on every one of %u rapid churn cycles",
                      (unsigned)OS_TEST_TIMER_CHURN_ITERATIONS);
    AHURA_TEST_CHECK(os_test_churn_timer_fired == 0U, "none of the stopped-before-expiry timers fired (fired=%lu)",
                      (unsigned long)os_test_churn_timer_fired);

    AHURA_TEST_CHECK(os_timer_init(&os_test_timer_oneshot, OS_TICKS_FROM_MS(30U), OS_TIMER_MODE_ONE_SHOT,
                                    test_churn_timer_cb, NULL) == OS_STATUS_OK,
                      "timer re-armed for a real run after the churn");
    AHURA_TEST_CHECK(os_timer_start(&os_test_timer_oneshot) == OS_STATUS_OK, "timer starts normally after the churn");
    os_delay_ms(60U);
    AHURA_TEST_CHECK(os_test_churn_timer_fired == 1U, "the post-churn timer still fires correctly (fired=%lu)",
                      (unsigned long)os_test_churn_timer_fired);
}
#endif /* OS_CONFIG_TIMER_ENABLE */

/*
 * ***********************************************************************************************************
 * Extended per-subsystem stress tests - OS_TEST_STRESS_EXTENDED
 * ***********************************************************************************************************
 *
 * These cost roughly 15 KB of flash, most of it the .rodata for their PASS/FAIL messages, which is
 * more than an unoptimized build of this project has left over: -O0 already sits at ~97% of the
 * STM32H503's 128 KB, so linking them there overflows. They are therefore compiled in whenever the
 * build is optimized at all (__OPTIMIZE__, i.e. any -O above -O0) and left out otherwise, with a
 * SKIP line naming the reason at run time.
 *
 * Keyed on the optimization level rather than on a hand-set switch because that is the thing that
 * actually decides whether they fit, and because a stress test is close to meaningless at -O0
 * anyway: every timing margin and every contention window is distorted by unoptimized code, so a
 * -O0 run would report numbers that say nothing about the firmware anyone ships. Define
 * OS_TEST_STRESS_EXTENDED explicitly to override in either direction - to 0 to reclaim the flash
 * in an optimized build, or to 1 at -O0 on a part with room to spare.
*/

#ifndef OS_TEST_STRESS_EXTENDED
#if defined(__OPTIMIZE__)
#define OS_TEST_STRESS_EXTENDED 1U
#else
#define OS_TEST_STRESS_EXTENDED 0U
#endif
#endif

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/* Enough timers to fill the registry, plus one more that must therefore be refused.
 *
 * Declared out here, ahead of the OS_TEST_STRESS_EXTENDED block below, because BOTH the extended
 * timer-flood stress test (inside that block, so compiled out in unoptimized builds) and
 * test_regressions() (which runs in every build) fill the registry with them. Declaring them
 * inside the block made the whole suite fail to compile at -O0 - precisely the build a board
 * bring-up uses. */
static os_timer_t os_test_tflood[OS_CONFIG_MAX_TIMERS];
static os_timer_t os_test_tflood_extra;
#endif

#if (OS_TEST_STRESS_EXTENDED == 1U)

/*
 * These sit between the two kinds of stress test above. test_stress_soak() contends several
 * DIFFERENT primitives from several tasks at once; the churn tests cycle ONE create/destroy path
 * repeatedly from a single task. Each test below drives one subsystem at high volume AND checks an
 * invariant strong enough to fail on a lost wakeup, a dropped or duplicated item, a leaked
 * registry slot, or a heap block handed out twice - failures a low-repetition functional check
 * cannot see, because the one interleaving it happens to produce is usually the easy one.
 *
 * Every count here is exact, not approximate: a test that only asserts "roughly the right number
 * of things happened" cannot distinguish a real dropped wakeup from scheduling jitter, so it would
 * have to be written loose enough to pass through the very bug it exists to catch. Where the
 * hardware genuinely cannot be pinned down (timer fire counts over a wall-clock window), the
 * tolerance is stated and bounded rather than left open.
 *
 * The multi-worker tests below start and join their tasks through os_test_stress_tasks[] rather than
 * repeating four near-identical lines each: one format string covers every worker, which keeps
 * per-worker failure attribution while costing a fraction of the .rodata that matters on a part
 * this close to full (see OS_TEST_STRESS_EXTENDED below).
*/

/* The two helpers below drive the queue-producer, event-bit-storm and mutex-convoy tests, so they
 * have to exist whenever ANY of those is compiled in: guarding them on a single feature would leave
 * the others calling an undeclared function, and leaving them unguarded breaks an
 * all-features-off build on -Wunused-function. Same reasoning as TEST_HELPER_NEEDED above. */
#define TEST_STRESS_WORKERS_NEEDED                                        \
    ((OS_CONFIG_MUTEX_ENABLE == 1U) || (OS_CONFIG_EVENT_ENABLE == 1U) ||  \
     ((OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)))

#if TEST_STRESS_WORKERS_NEEDED

/* The four concurrent task slots the multi-worker stress tests share, in priority order. */
static os_task_t *const os_test_stress_tasks[4] = { &worker, &helper, &helper2, &helper3 };

/******************************************************************************************************/
/**
 * @brief Start `count` of the shared task slots on the same entry point, each with its own context
 *        and a distinct priority (3, 4, 5, ...), and report how many actually started.
 */
static uint32_t test_stress_start_workers(os_task_entry_t entry, void *contexts, size_t context_size,
                                          uint32_t count)
{
    uint32_t started = 0U;
    uint32_t i;

    for (i = 0U; i < count; i++)
    {
        /* Each slot carries its own stack and name, from the OS_TASK_DEFINE that declared it, so
         * the config here is purely behaviour and the same one works for every slot in the array.
         * This used to need a switch mapping the index back to the matching *_STACK symbol. */
        os_task_config_t config;

        config.entry         = entry;
        config.context       = (void *)((uint8_t *)contexts + (i * context_size));
        config.priority      = 3U + i;
        config.core_affinity = OS_TASK_CORE_ANY;

        if (os_task_create(os_test_stress_tasks[i], &config) != OS_STATUS_OK) { break; }
        if (os_task_start(os_test_stress_tasks[i]) != OS_STATUS_OK)           { break; }

        started++;
    }

    return started;
}

/******************************************************************************************************/
/**
 * @brief Join `count` shared task slots, checking each one individually so a hang is attributed to
 *        the worker that hung rather than to the group.
 */
static void test_stress_join_workers(uint32_t count, uint32_t timeout_ms)
{
    uint32_t i;

    for (i = 0U; i < count; i++)
    {
        AHURA_TEST_CHECK(test_wait_inactive(os_test_stress_tasks[i], timeout_ms),
                          "stress worker %lu terminated cleanly (no deadlock/hang)", (unsigned long)i);
    }
}
#endif /* TEST_STRESS_WORKERS_NEEDED */

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)

#define OS_TEST_QCHURN_ITERATIONS 200U

/******************************************************************************************************/
/**
 * @brief Creates, uses and deletes a HEAP-allocated queue back-to-back, with a different geometry
 *        every cycle. test_queue_define_and_dynamic() proves one create/delete pair is correct;
 *        this proves the pair stays correct 200 times running, which is what actually catches a
 *        per-cycle leak or an allocator that mis-splits a reused hole.
 */
static void test_stress_queue_dynamic_churn(void)
{
    size_t   heap_before = os_mem_free_get();
    size_t   worst_free  = heap_before;
    uint32_t completed   = 0U;
    bool     all_ok      = true;
    bool     data_ok     = true;
    uint32_t i;

    test_print_section("Stress: dynamic queue alloc/use/cleanup churn");

    for (i = 0U; i < OS_TEST_QCHURN_ITERATIONS; i++)
    {
        os_queue_t q         = { 0 };
        size_t     capacity  = 1U + (i % 8U);
        size_t     item_size = sizeof(uint32_t) * (1U + (i % 3U));
        uint32_t   sent[3];
        uint32_t   got[3]    = { 0U, 0U, 0U };
        size_t     free_now;

        /* The geometry deliberately changes every cycle. A fixed size would hand back the same
         * hole each time and never exercise splitting or coalescing against differently sized
         * neighbours - the case where an off-by-one in the allocator actually shows up. */
        if (os_queue_init_dynamic(&q, item_size, capacity) != OS_STATUS_OK)
        {
            all_ok = false;
            break;
        }

        free_now = os_mem_free_get();
        if (free_now < worst_free) { worst_free = free_now; }

        sent[0] = 0xA5A50000UL | (i & 0xFFFFU);
        sent[1] = i;
        sent[2] = ~i;

        if (os_queue_send(&q, sent, OS_WAIT_NOTHING) != OS_STATUS_OK)
        {
            all_ok = false;
        }
        else if (os_queue_receive(&q, got, OS_WAIT_NOTHING) != OS_STATUS_OK)
        {
            all_ok = false;
        }
        else
        {
            /* Only the words the item is actually wide enough to carry are compared: the rest of
             * got[] was never written, so checking them would fail on a correct kernel. */
            if (got[0] != sent[0])                                                 { data_ok = false; }
            if ((item_size > sizeof(uint32_t)) && (got[1] != sent[1]))              { data_ok = false; }
            if ((item_size > (2U * sizeof(uint32_t))) && (got[2] != sent[2]))       { data_ok = false; }
        }

        if (os_queue_cleanup(&q) != OS_STATUS_OK) { all_ok = false; }

        if (!all_ok) { break; }

        completed++;
    }

    AHURA_TEST_CHECK(all_ok && (completed == OS_TEST_QCHURN_ITERATIONS),
                      "create/send/receive/delete succeeded on all %u cycles (%lu completed)",
                      (unsigned)OS_TEST_QCHURN_ITERATIONS, (unsigned long)completed);
    AHURA_TEST_CHECK(data_ok, "every cycle's item came back with its full payload intact");
    AHURA_TEST_CHECK(worst_free < heap_before,
                      "the buffers really came off the heap (low water %lu vs %lu bytes free)",
                      (unsigned long)worst_free, (unsigned long)heap_before);
    AHURA_TEST_CHECK(os_mem_free_get() == heap_before,
                      "%u create/delete cycles leaked nothing (%lu bytes free, unchanged)",
                      (unsigned)OS_TEST_QCHURN_ITERATIONS, (unsigned long)os_mem_free_get());
}

#define OS_TEST_QPROD_COUNT 3U
#define OS_TEST_QPROD_ITEMS 32U   /* 32 so one uint32_t mask tracks a producer's whole run */

typedef struct
{
    uint32_t id;

} test_qprod_ctx_t;

static test_qprod_ctx_t os_test_qprod_ctx[OS_TEST_QPROD_COUNT];
OS_QUEUE_DEFINE_DYNAMIC(os_test_qprod_queue);
static __IO uint32_t    os_test_qprod_sent[OS_TEST_QPROD_COUNT];

/******************************************************************************************************/
static void test_qprod_entry(void *context)
{
    test_qprod_ctx_t *ctx = (test_qprod_ctx_t *)context;
    uint32_t          seq;

    for (seq = 0U; seq < OS_TEST_QPROD_ITEMS; seq++)
    {
        uint32_t tag = (ctx->id << 16) | seq;

        if (os_queue_send(&os_test_qprod_queue, &tag, 500U) == OS_STATUS_OK)
        {
            os_test_qprod_sent[ctx->id]++;
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Three producers hammer a heap-allocated queue whose capacity is smaller than the producer
 *        count, so nearly every send goes through the blocking path. The consumer then accounts for
 *        every (producer, sequence) pair individually: a lost send-waiter wakeup shows up as a
 *        missing bit, and a double delivery as an already-set one. The existing pipeline test
 *        covers a STATIC queue with a handful of items; this covers the dynamic one at volume.
 */
static void test_stress_queue_dynamic_concurrent(void)
{
    uint32_t seen[OS_TEST_QPROD_COUNT];
    uint32_t received   = 0U;
    uint32_t total_sent = 0U;
    uint32_t expected   = OS_TEST_QPROD_COUNT * OS_TEST_QPROD_ITEMS;
    bool     duplicate  = false;
    bool     malformed  = false;
    bool     all_seen   = true;
    size_t   heap_before;
    uint32_t i;

    test_print_section("Stress: 3 producers on a heap-allocated queue, exact item accounting");

    heap_before = os_mem_free_get();

    AHURA_TEST_CHECK(os_queue_init_dynamic(&os_test_qprod_queue, sizeof(uint32_t), 2U) == OS_STATUS_OK,
                      "dynamic queue created (capacity 2, deliberately < %u producers)",
                      (unsigned)OS_TEST_QPROD_COUNT);

    for (i = 0U; i < OS_TEST_QPROD_COUNT; i++)
    {
        os_test_qprod_ctx[i].id = i;
        os_test_qprod_sent[i]   = 0U;
        seen[i]           = 0U;
    }

    AHURA_TEST_CHECK(test_stress_start_workers(test_qprod_entry, os_test_qprod_ctx, sizeof(os_test_qprod_ctx[0]),
                                               OS_TEST_QPROD_COUNT) == OS_TEST_QPROD_COUNT,
                      "all %u producers created and started", (unsigned)OS_TEST_QPROD_COUNT);

    /* This task sits below every producer, so it only gets the CPU once they are all blocked on a
     * full queue - which is exactly the interleaving a missed send-waiter wakeup would deadlock. */
    while (received < expected)
    {
        uint32_t tag;
        uint32_t producer;
        uint32_t seq;

        if (os_queue_receive(&os_test_qprod_queue, &tag, 500U) != OS_STATUS_OK) { break; }

        producer = tag >> 16;
        seq      = tag & 0xFFFFU;

        if ((producer >= OS_TEST_QPROD_COUNT) || (seq >= OS_TEST_QPROD_ITEMS))
        {
            malformed = true;
        }
        else if ((seen[producer] & (1UL << seq)) != 0U)
        {
            duplicate = true;
        }
        else
        {
            seen[producer] |= (1UL << seq);
        }

        received++;
    }

    test_stress_join_workers(OS_TEST_QPROD_COUNT, 2000U);

    for (i = 0U; i < OS_TEST_QPROD_COUNT; i++)
    {
        total_sent += os_test_qprod_sent[i];
        if (seen[i] != 0xFFFFFFFFUL) { all_seen = false; }
    }

    AHURA_TEST_CHECK(total_sent == expected, "every producer placed all its items (%lu of %lu)",
                      (unsigned long)total_sent, (unsigned long)expected);
    AHURA_TEST_CHECK(received == expected, "the consumer took exactly as many as were sent (%lu of %lu)",
                      (unsigned long)received, (unsigned long)expected);
    AHURA_TEST_CHECK(!malformed, "no delivered item decoded to an impossible producer/sequence");
    AHURA_TEST_CHECK(!duplicate, "no item was delivered twice");
    AHURA_TEST_CHECK(all_seen, "each producer's %u sequence numbers arrived exactly once each",
                      (unsigned)OS_TEST_QPROD_ITEMS);

    AHURA_TEST_CHECK(os_queue_count_get(&os_test_qprod_queue) == 0U, "the queue ended empty");
    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_qprod_queue) == OS_STATUS_OK, "the dynamic queue tears down cleanly");
    AHURA_TEST_CHECK(os_mem_free_get() == heap_before, "and returned its buffer to the heap");
}
#endif /* OS_CONFIG_QUEUE_ENABLE && OS_CONFIG_ALLOC_ENABLE */

#if (OS_CONFIG_ALLOC_ENABLE == 1U)

#define OS_TEST_FRAG_BLOCKS 24U
#define OS_TEST_FRAG_SIZE   32U

/******************************************************************************************************/
/**
 * @brief Fragments the heap deliberately, then checks the three things test_alloc() cannot: that
 *        freeing a block never disturbs a live neighbour, that adjacent holes really do coalesce
 *        back into one usable run, and that the heap recovers exactly after being driven to
 *        exhaustion. A first-fit allocator with a coalescing bug passes a handful of alloc/free
 *        calls easily and only misbehaves once the free list has holes on both sides of a block.
 */
static void test_stress_heap_fragmentation(void)
{
    void     *blocks[OS_TEST_FRAG_BLOCKS];
    size_t   heap_before = os_mem_free_get();
    uint32_t allocated   = 0U;
    bool     pattern_ok  = true;
    void     *big;
    uint32_t i;

    test_print_section("Stress: heap fragmentation, coalescing and exhaustion recovery");

    for (i = 0U; i < OS_TEST_FRAG_BLOCKS; i++)
    {
        blocks[i] = os_mem_alloc(OS_TEST_FRAG_SIZE);
        if (blocks[i] == NULL) { break; }

        memset(blocks[i], (int)(0x40U + i), OS_TEST_FRAG_SIZE);
        allocated++;
    }

    AHURA_TEST_CHECK(allocated == OS_TEST_FRAG_BLOCKS,
                      "%u blocks of %u bytes allocated (%lu succeeded)",
                      (unsigned)OS_TEST_FRAG_BLOCKS, (unsigned)OS_TEST_FRAG_SIZE, (unsigned long)allocated);

    /* Free every other block, leaving the heap checkerboarded. The survivors are the real test: an
     * allocator that merged a freed hole into a LIVE neighbour corrupts them right here, and a
     * status-code-only check would sail straight past it. */
    for (i = 1U; i < allocated; i += 2U)
    {
        os_mem_free(blocks[i]);
        blocks[i] = NULL;
    }

    for (i = 0U; i < allocated; i += 2U)
    {
        const uint8_t *bytes = (const uint8_t *)blocks[i];
        uint32_t       j;

        for (j = 0U; j < OS_TEST_FRAG_SIZE; j++)
        {
            if (bytes[j] != (uint8_t)(0x40U + i)) { pattern_ok = false; }
        }
    }

    AHURA_TEST_CHECK(pattern_ok, "every surviving block kept its contents through the interleaved frees");

    for (i = 0U; i < allocated; i += 2U)
    {
        os_mem_free(blocks[i]);
        blocks[i] = NULL;
    }

    AHURA_TEST_CHECK(os_mem_free_get() == heap_before,
                      "freeing all of it restored the heap exactly (%lu bytes free)",
                      (unsigned long)os_mem_free_get());

    /* Coalescing proof: this is many times larger than any single block just freed, so it can only
     * be satisfied if the neighbouring holes were merged back into one contiguous run. */
    big = os_mem_alloc((OS_TEST_FRAG_BLOCKS * OS_TEST_FRAG_SIZE) / 2U);
    AHURA_TEST_CHECK(big != NULL,
                      "a single %u-byte block still fits afterwards, so the holes coalesced",
                      (unsigned)((OS_TEST_FRAG_BLOCKS * OS_TEST_FRAG_SIZE) / 2U));
    os_mem_free(big);

    /* Exhaustion and recovery: take large blocks until the heap refuses, then give them all back
     * and confirm not one byte went missing along the way. */
    allocated = 0U;
    for (i = 0U; i < OS_TEST_FRAG_BLOCKS; i++)
    {
        blocks[i] = os_mem_alloc(OS_CONFIG_HEAP_SIZE / 8U);
        if (blocks[i] == NULL) { break; }

        allocated++;
    }

    AHURA_TEST_CHECK(os_mem_alloc(OS_CONFIG_HEAP_SIZE) == NULL,
                      "a request past the remaining heap returns NULL, not a short block");

    for (i = 0U; i < allocated; i++) { os_mem_free(blocks[i]); }

    AHURA_TEST_CHECK(os_mem_free_get() == heap_before,
                      "the heap recovers fully after exhaustion (%lu bytes free)",
                      (unsigned long)os_mem_free_get());

    printf("  [INFO] heap held %lu blocks of %lu bytes before refusing; all-time low %lu bytes free\r\n",
           (unsigned long)allocated, (unsigned long)(OS_CONFIG_HEAP_SIZE / 8U),
           (unsigned long)os_mem_watermark_get());
}
#endif /* OS_CONFIG_ALLOC_ENABLE */

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)

#define OS_TEST_PINGPONG_ROUNDS 1000U

static os_semaphore_t os_test_pp_ping;
static os_semaphore_t os_test_pp_pong;
static __IO uint32_t  os_test_pp_partner_rounds = 0U;

/******************************************************************************************************/
static void test_pp_entry(void *context)
{
    (void)context;

    while (os_test_pp_partner_rounds < OS_TEST_PINGPONG_ROUNDS)
    {
        if (os_semaphore_take(&os_test_pp_ping, 500U) != OS_STATUS_OK) { break; }

        os_test_pp_partner_rounds++;
        (void)os_semaphore_give(&os_test_pp_pong);
    }
}

/******************************************************************************************************/
/**
 * @brief Two tasks hand a token back and forth through a pair of binary semaphores, 1000 round
 *        trips - 2000 blocking handoffs. Both semaphores start empty and the partner runs above
 *        this task, so every single take genuinely blocks and every give genuinely wakes a waiter:
 *        there is no already-available token to paper over a lost wakeup. One dropped wake stalls
 *        the loop instead of quietly reducing a count.
 */
static void test_stress_semaphore_pingpong(void)
{
    uint32_t completed = 0U;
    uint32_t elapsed;
    uint32_t t0;
    uint32_t i;

    test_print_section("Stress: binary-semaphore ping-pong handoffs");

    os_test_pp_partner_rounds = 0U;

    AHURA_TEST_CHECK(os_semaphore_init(&os_test_pp_ping, 0U, 1U) == OS_STATUS_OK, "ping semaphore initialized (binary, empty)");
    AHURA_TEST_CHECK(os_semaphore_init(&os_test_pp_pong, 0U, 1U) == OS_STATUS_OK, "pong semaphore initialized (binary, empty)");

    if (os_task_create(&worker, OS_TASK_CONFIG(test_pp_entry, NULL, TEST_PRIO_HIGH)) != OS_STATUS_OK)
    {
        printf("  [SKIP] could not create the ping-pong partner task\r\n");
        return;
    }
    (void)os_task_start(&worker);

    t0 = os_tick_get();

    for (i = 0U; i < OS_TEST_PINGPONG_ROUNDS; i++)
    {
        if (os_semaphore_give(&os_test_pp_ping) != OS_STATUS_OK)       { break; }
        if (os_semaphore_take(&os_test_pp_pong, 500U) != OS_STATUS_OK) { break; }

        completed++;
    }

    elapsed = os_tick_get() - t0;

    AHURA_TEST_CHECK(completed == OS_TEST_PINGPONG_ROUNDS,
                      "all %u round trips completed, none stalled on a lost wakeup (%lu)",
                      (unsigned)OS_TEST_PINGPONG_ROUNDS, (unsigned long)completed);
    AHURA_TEST_CHECK(os_test_pp_partner_rounds == OS_TEST_PINGPONG_ROUNDS,
                      "the partner counted exactly the same number of tokens (%lu)",
                      (unsigned long)os_test_pp_partner_rounds);
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 1000U), "the partner task terminated cleanly");
    AHURA_TEST_CHECK(os_semaphore_take(&os_test_pp_ping, OS_WAIT_NOTHING) == OS_STATUS_EMPTY,
                      "no stray ping token was left behind");
    AHURA_TEST_CHECK(os_semaphore_take(&os_test_pp_pong, OS_WAIT_NOTHING) == OS_STATUS_EMPTY,
                      "no stray pong token was left behind");

    printf("  [INFO] %lu blocking handoffs in %lu ms\r\n",
           (unsigned long)(2UL * OS_TEST_PINGPONG_ROUNDS), (unsigned long)elapsed);
}
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)

#define OS_TEST_NOTIFY_STORM_COUNT 1000U

static __IO uint32_t os_test_ns_received = 0U;
static __IO uint32_t os_test_ns_last     = 0U;
static __IO bool     os_test_ns_order_ok = true;
static __IO bool     os_test_ns_run      = true;

/******************************************************************************************************/
static void test_ns_entry(void *context)
{
    (void)context;

    while (os_test_ns_run)
    {
        uint32_t value = 0U;

        if (os_notify_wait(50U, &value) == OS_STATUS_OK)
        {
            /* Values are sent 1..N in order, so the next one must be exactly one past the count
             * already taken. Anything else means a notification was lost, delivered twice, or the
             * mailbox handed back a stale value. */
            if (value != (os_test_ns_received + 1U)) { os_test_ns_order_ok = false; }

            os_test_ns_received++;
            os_test_ns_last = value;
        }
    }
}

/******************************************************************************************************/
/**
 * @brief 1000 notifications delivered to a waiter running ABOVE the sender, so it preempts on
 *        every give and consumes each value before the next is written. That is what makes exact
 *        1:1 accounting meaningful for a mailbox whose documented behaviour is last-write-wins:
 *        under this interleaving no overwrite is legitimate, so a missing or repeated value is
 *        unambiguously a lost or duplicated wakeup rather than the overwrite semantics working.
 */
static void test_stress_notify_storm(void)
{
    uint32_t delivered = 0U;
    uint32_t i;

    test_print_section("Stress: task-notification storm");

    os_test_ns_received = 0U;
    os_test_ns_last     = 0U;
    os_test_ns_order_ok = true;
    os_test_ns_run      = true;

    if (os_task_create(&worker, OS_TASK_CONFIG(test_ns_entry, NULL, TEST_PRIO_HIGH)) != OS_STATUS_OK)
    {
        printf("  [SKIP] could not create the notification waiter task\r\n");
        return;
    }
    (void)os_task_start(&worker);

    for (i = 1U; i <= OS_TEST_NOTIFY_STORM_COUNT; i++)
    {
        if (os_notify_give(&worker, i) != OS_STATUS_OK) { break; }

        delivered++;
    }

    os_test_ns_run = false;

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 1000U), "the waiter task terminated cleanly");
    AHURA_TEST_CHECK(delivered == OS_TEST_NOTIFY_STORM_COUNT,
                      "all %u notifications were accepted (%lu)",
                      (unsigned)OS_TEST_NOTIFY_STORM_COUNT, (unsigned long)delivered);
    AHURA_TEST_CHECK(os_test_ns_received == OS_TEST_NOTIFY_STORM_COUNT,
                      "the waiter consumed every one exactly once (%lu of %u)",
                      (unsigned long)os_test_ns_received, (unsigned)OS_TEST_NOTIFY_STORM_COUNT);
    AHURA_TEST_CHECK(os_test_ns_order_ok, "every value arrived in order, none lost or repeated");
    AHURA_TEST_CHECK(os_test_ns_last == OS_TEST_NOTIFY_STORM_COUNT,
                      "the last value received is the last one sent (%lu)", (unsigned long)os_test_ns_last);
}
#endif /* OS_CONFIG_NOTIFY_ENABLE */

#if (OS_CONFIG_EVENT_ENABLE == 1U)

#define OS_TEST_EBS_WORKERS 4U
#define OS_TEST_EBS_ITERS   250U

typedef struct
{
    uint32_t id;
    uint32_t bit;

} test_ebs_ctx_t;

static test_ebs_ctx_t   os_test_ebs_ctx[OS_TEST_EBS_WORKERS];
static os_event_t os_test_ebs_event;
static __IO uint32_t    os_test_ebs_matched[OS_TEST_EBS_WORKERS];

/******************************************************************************************************/
static void test_ebs_entry(void *context)
{
    test_ebs_ctx_t *ctx = (test_ebs_ctx_t *)context;
    uint32_t        i;

    for (i = 0U; i < OS_TEST_EBS_ITERS; i++)
    {
        uint32_t matched = 0U;

        (void)os_event_set_bits(&os_test_ebs_event, ctx->bit);

        if (os_event_wait_bits(&os_test_ebs_event, ctx->bit, false, true, &matched, 100U) == OS_STATUS_OK)
        {
            if ((matched & ctx->bit) != 0U) { os_test_ebs_matched[ctx->id]++; }
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Four tasks each own one bit of the same event and pound set / wait / clear-on-exit
 *        on it concurrently, 250 iterations apiece. Because a worker only ever touches its OWN bit
 *        and consumes it in the same iteration it set it, the group must end with all four bits
 *        clear - a bit left standing means one set was matched without being cleared, or cleared
 *        without matching. That end-state invariant is what a single-task functional check cannot
 *        provide: it needs concurrent set/wait/clear traffic on one group to be worth anything.
 */
static void test_stress_event_bit_storm(void)
{
    uint32_t all_bits = (1UL << OS_TEST_EBS_WORKERS) - 1UL;
    uint32_t leftover = 0U;
    uint32_t total    = 0U;
    uint32_t i;

    test_print_section("Stress: 4 tasks set/wait/clear their own event bit concurrently");

    AHURA_TEST_CHECK(os_event_init(&os_test_ebs_event) == OS_STATUS_OK, "bit-storm event initialized");

    for (i = 0U; i < OS_TEST_EBS_WORKERS; i++)
    {
        os_test_ebs_ctx[i].id  = i;
        os_test_ebs_ctx[i].bit = 1UL << i;
        os_test_ebs_matched[i] = 0U;
    }

    AHURA_TEST_CHECK(test_stress_start_workers(test_ebs_entry, os_test_ebs_ctx, sizeof(os_test_ebs_ctx[0]),
                                               OS_TEST_EBS_WORKERS) == OS_TEST_EBS_WORKERS,
                      "all %u bit-storm workers created and started", (unsigned)OS_TEST_EBS_WORKERS);

    test_stress_join_workers(OS_TEST_EBS_WORKERS, 5000U);

    for (i = 0U; i < OS_TEST_EBS_WORKERS; i++) { total += os_test_ebs_matched[i]; }

    AHURA_TEST_CHECK(total == (OS_TEST_EBS_WORKERS * OS_TEST_EBS_ITERS),
                      "every set was matched by its owner's wait (%lu of %lu)",
                      (unsigned long)total, (unsigned long)(OS_TEST_EBS_WORKERS * OS_TEST_EBS_ITERS));

    (void)os_event_wait_bits(&os_test_ebs_event, all_bits, false, false, &leftover, OS_WAIT_NOTHING);
    AHURA_TEST_CHECK((leftover & all_bits) == 0U,
                      "no worker's bit was left standing at the end (flags=0x%02lX)",
                      (unsigned long)(leftover & all_bits));
}
#endif /* OS_CONFIG_EVENT_ENABLE */

#if (OS_CONFIG_WORK_ENABLE == 1U)

#define OS_TEST_WFLOOD_ITEMS  (2U * OS_CONFIG_MAX_WORKS)
#define OS_TEST_WFLOOD_ROUNDS 20U

static __IO uint32_t os_test_wflood_ran = 0U;

/******************************************************************************************************/
static void test_wflood_handler(void *data, size_t len)
{
    (void)data;
    (void)len;
    os_test_wflood_ran++;
}

/******************************************************************************************************/
/**
 * @brief Oversubscribes the work registry with twice as many submissions as it has slots, so the
 *        FULL path is exercised for real rather than hypothesized, and reconciles both outcomes
 *        exactly: executed + refused. A registry that wrote past its array, or leaked a slot when a
 *        handler finished, cannot make these totals balance. Then it churns the registry 20 more
 *        times to prove slots are reused cleanly.
 */
static void test_stress_work_flood(void)
{
    uint32_t accepted = 0U;
    uint32_t refused  = 0U;
    uint32_t round;
    uint32_t i;

    test_print_section("Stress: work registry oversubscribed and flooded");

    os_test_wflood_ran = 0U;

    /* The delay is long enough that nothing can run during the submit loop, which is what makes
     * the accepted/refused split deterministic: exactly OS_CONFIG_MAX_WORKS slots exist. */
    for (i = 0U; i < OS_TEST_WFLOOD_ITEMS; i++)
    {
        os_status status = os_work_submit(test_wflood_handler, NULL, 0U, 60U);

        if (status == OS_STATUS_OK)        { accepted++; }
        else if (status == OS_STATUS_FULL) { refused++; }
    }

    AHURA_TEST_CHECK(accepted == OS_CONFIG_MAX_WORKS,
                      "the registry took exactly its %u slots and no more (%lu accepted)",
                      (unsigned)OS_CONFIG_MAX_WORKS, (unsigned long)accepted);
    AHURA_TEST_CHECK(refused == (OS_TEST_WFLOOD_ITEMS - OS_CONFIG_MAX_WORKS),
                      "every submission past capacity was refused with FULL (%lu)", (unsigned long)refused);

    os_delay_ms(120U);

    AHURA_TEST_CHECK(os_test_wflood_ran == accepted,
                      "every accepted submission ran exactly once (%lu of %lu)",
                      (unsigned long)os_test_wflood_ran, (unsigned long)accepted);

    /* Registry churn: fill it, let it drain, repeat. Each round has to release and reuse its slots
     * cleanly or the total below cannot come out even. */
    os_test_wflood_ran = 0U;
    accepted     = 0U;

    for (round = 0U; round < OS_TEST_WFLOOD_ROUNDS; round++)
    {
        for (i = 0U; i < OS_TEST_WFLOOD_ITEMS; i++)
        {
            if (os_work_submit(test_wflood_handler, NULL, 0U, 0U) == OS_STATUS_OK) { accepted++; }
        }

        for (i = 0U; (i < 50U) && (os_test_wflood_ran < accepted); i++)
        {
            os_delay_ms(1U);
        }
    }

    AHURA_TEST_CHECK(os_test_wflood_ran == accepted,
                      "every accepted item across %u churn rounds ran exactly once (%lu of %lu)",
                      (unsigned)OS_TEST_WFLOOD_ROUNDS, (unsigned long)os_test_wflood_ran, (unsigned long)accepted);
}
#endif /* OS_CONFIG_WORK_ENABLE */

#if (OS_CONFIG_TIMER_ENABLE == 1U)

#define OS_TEST_TFLOOD_WINDOW 200U

/* os_test_tflood[] and os_test_tflood_extra are declared further down, outside
 * this OS_TEST_STRESS_EXTENDED block: test_regressions() fills the timer
 * registry with them too, and that test always runs. */
static __IO uint32_t os_test_tflood_fired[OS_CONFIG_MAX_TIMERS];

/******************************************************************************************************/
static void test_tflood_cb(void *context)
{
    uint32_t index = (uint32_t)(uintptr_t)context;

    if (index < OS_CONFIG_MAX_TIMERS) { os_test_tflood_fired[index]++; }
}

/******************************************************************************************************/
/**
 * @brief Arms every timer slot periodically at once, each at a different period, and lets them all
 *        run together for a fixed window. test_timer() runs one timer at a time; this checks the
 *        registry under a full load of concurrent expiries - that each timer keeps its own period
 *        rather than inheriting a neighbour's, that one past capacity is refused, and that a
 *        stopped timer really stops instead of firing once more from a stale registry entry.
 *
 * Fire counts are the one thing here that cannot be exact: the callbacks run on the timer task and
 * the window is measured with os_delay_ms, so a boundary expiry may land on either side. The
 * tolerance is bounded at +/-2 rather than left open, which is still far tighter than the error a
 * wrong period would produce.
 */
static void test_stress_timer_flood(void)
{
    uint32_t snapshot[OS_CONFIG_MAX_TIMERS];
    uint32_t started      = 0U;
    bool     all_stopped  = true;
    bool     counts_ok    = true;
    bool     still_firing = false;
    uint32_t i;

    test_print_section("Stress: every timer slot armed periodically at once");

    for (i = 0U; i < OS_CONFIG_MAX_TIMERS; i++)
    {
        uint32_t period_ms = 10U + (i * 5U);

        os_test_tflood_fired[i] = 0U;

        (void)os_timer_init(&os_test_tflood[i], OS_TICKS_FROM_MS(period_ms), OS_TIMER_MODE_PERIODIC,
                             test_tflood_cb, (void *)(uintptr_t)i);

        if (os_timer_start(&os_test_tflood[i]) == OS_STATUS_OK) { started++; }
    }

    AHURA_TEST_CHECK(started == OS_CONFIG_MAX_TIMERS,
                      "all %u timer slots armed periodically (%lu started)",
                      (unsigned)OS_CONFIG_MAX_TIMERS, (unsigned long)started);

    (void)os_timer_init(&os_test_tflood_extra, OS_TICKS_FROM_MS(10U), OS_TIMER_MODE_PERIODIC,
                         test_tflood_cb, (void *)(uintptr_t)OS_CONFIG_MAX_TIMERS);
    AHURA_TEST_CHECK(os_timer_start(&os_test_tflood_extra) == OS_STATUS_FULL,
                      "one timer past the registry's capacity is refused with FULL");

    os_delay_ms(OS_TEST_TFLOOD_WINDOW);

    for (i = 0U; i < OS_CONFIG_MAX_TIMERS; i++)
    {
        if (os_timer_stop(&os_test_tflood[i]) != OS_STATUS_OK) { all_stopped = false; }

        snapshot[i] = os_test_tflood_fired[i];
    }

    AHURA_TEST_CHECK(all_stopped, "every armed timer stopped cleanly");

    for (i = 0U; i < OS_CONFIG_MAX_TIMERS; i++)
    {
        uint32_t period_ms = 10U + (i * 5U);
        uint32_t expected  = OS_TEST_TFLOOD_WINDOW / period_ms;

        if (((snapshot[i] + 2U) < expected) || (snapshot[i] > (expected + 2U))) { counts_ok = false; }

        printf("  [INFO] timer %lu (period %lu ms): fired %lu times, expected ~%lu\r\n",
               (unsigned long)i, (unsigned long)period_ms, (unsigned long)snapshot[i],
               (unsigned long)expected);
    }

    AHURA_TEST_CHECK(counts_ok,
                      "each timer fired at its own period over the %u ms window (all within +/-2)",
                      (unsigned)OS_TEST_TFLOOD_WINDOW);

    /* Long enough for even the slowest of them to have expired again had the stop not taken. */
    os_delay_ms(80U);

    for (i = 0U; i < OS_CONFIG_MAX_TIMERS; i++)
    {
        if (os_test_tflood_fired[i] != snapshot[i]) { still_firing = true; }
    }

    AHURA_TEST_CHECK(!still_firing, "no stopped timer fired again afterwards");
}
#endif /* OS_CONFIG_TIMER_ENABLE */

#if (OS_CONFIG_MUTEX_ENABLE == 1U)

#define OS_TEST_CONVOY_WORKERS 4U
#define OS_TEST_CONVOY_ITERS   200U

typedef struct
{
    uint32_t id;

} test_convoy_ctx_t;

static test_convoy_ctx_t os_test_convoy_ctx[OS_TEST_CONVOY_WORKERS];
static os_mutex_t        os_test_convoy_mutex;
static __IO uint32_t     os_test_convoy_counter = 0U;
static __IO uint32_t     os_test_convoy_locks[OS_TEST_CONVOY_WORKERS];
static __IO bool         os_test_convoy_violation = false;

/******************************************************************************************************/
static void test_convoy_entry(void *context)
{
    test_convoy_ctx_t *ctx = (test_convoy_ctx_t *)context;
    uint32_t           i;

    for (i = 0U; i < OS_TEST_CONVOY_ITERS; i++)
    {
        if (os_mutex_lock(&os_test_convoy_mutex, 1000U) == OS_STATUS_OK)
        {
            uint32_t before = os_test_convoy_counter;

            /* Yielding while holding the mutex is the whole point: with a working mutex nothing
             * else can be inside the section, so the counter must still read `before` when this
             * task is scheduled again. A broken lock shows up here as a changed value, not merely
             * as a wrong total at the end. */
            os_task_yield();

            if (os_test_convoy_counter != before) { os_test_convoy_violation = true; }

            os_test_convoy_counter = before + 1U;
            os_test_convoy_locks[ctx->id]++;

            (void)os_mutex_unlock(&os_test_convoy_mutex);
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Four tasks at four different priorities queue up on a single mutex, 200 acquisitions
 *        each, yielding inside the critical section every time. Checks exclusivity from inside
 *        the section (see the worker), the exact total from outside, and that no task was starved
 *        - priority inheritance is supposed to keep the lowest-priority worker making progress,
 *        and only a run this long with a per-task tally can show whether it does.
 */
static void test_stress_mutex_convoy(void)
{
    uint32_t expected   = OS_TEST_CONVOY_WORKERS * OS_TEST_CONVOY_ITERS;
    uint32_t total      = 0U;
    bool     no_starve  = true;
    uint32_t i;

    test_print_section("Stress: 4 tasks convoy on one mutex, yielding inside the section");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_convoy_mutex) == OS_STATUS_OK, "convoy mutex initialized");

    os_test_convoy_counter   = 0U;
    os_test_convoy_violation = false;

    for (i = 0U; i < OS_TEST_CONVOY_WORKERS; i++)
    {
        os_test_convoy_ctx[i].id = i;
        os_test_convoy_locks[i]  = 0U;
    }

    AHURA_TEST_CHECK(test_stress_start_workers(test_convoy_entry, os_test_convoy_ctx, sizeof(os_test_convoy_ctx[0]),
                                               OS_TEST_CONVOY_WORKERS) == OS_TEST_CONVOY_WORKERS,
                      "all %u convoy workers created and started", (unsigned)OS_TEST_CONVOY_WORKERS);

    test_stress_join_workers(OS_TEST_CONVOY_WORKERS, 10000U);

    for (i = 0U; i < OS_TEST_CONVOY_WORKERS; i++)
    {
        total += os_test_convoy_locks[i];
        if (os_test_convoy_locks[i] == 0U) { no_starve = false; }
    }

    AHURA_TEST_CHECK(!os_test_convoy_violation,
                      "no worker ever observed the counter change while it held the mutex");
    AHURA_TEST_CHECK(total == expected, "every acquisition succeeded (%lu of %lu)",
                      (unsigned long)total, (unsigned long)expected);
    AHURA_TEST_CHECK(os_test_convoy_counter == total,
                      "the protected counter equals the acquisition count (%lu vs %lu - a mismatch is a lost update)",
                      (unsigned long)os_test_convoy_counter, (unsigned long)total);
    AHURA_TEST_CHECK(no_starve, "no worker was starved out of the mutex entirely");
    AHURA_TEST_CHECK(os_mutex_try_lock(&os_test_convoy_mutex) == OS_STATUS_OK, "the mutex ended unlocked");
    (void)os_mutex_unlock(&os_test_convoy_mutex);

    for (i = 0U; i < OS_TEST_CONVOY_WORKERS; i++)
    {
        printf("  [INFO] convoy worker %lu (priority %lu): %lu acquisitions\r\n",
               (unsigned long)i, (unsigned long)(3U + i), (unsigned long)os_test_convoy_locks[i]);
    }
}
#endif /* OS_CONFIG_MUTEX_ENABLE */

#endif /* OS_TEST_STRESS_EXTENDED */

/*
 * ***********************************************************************************************************
 * Task / stack footprint and context-switch timing (informational - no "correct" value to assert)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Prints task sizing info: the public handle size, each configured task stack size, and
 *        actual peak stack usage (watermark) for this task and a freshly spun-up worker.
 *
 * The kernel's internal TCB struct is a private implementation detail (not exposed via ahura.h,
 * by design - see os_internal.h), so "task size" here means what the public API can actually
 * report: os_task_t's own size, the configured stack budgets, and measured watermark usage.
 */
static void test_task_footprint(void)
{
    test_print_section("Task / Stack Footprint (informational)");

    printf("  [INFO] sizeof(os_task_t) = %lu bytes (the public task handle)\r\n",
           (unsigned long)sizeof(os_task_t));
    printf("  [INFO] OS_CONFIG_MIN_STACK_SIZE       = %lu bytes\r\n", (unsigned long)OS_CONFIG_MIN_STACK_SIZE);
#if (OS_CONFIG_WORK_ENABLE == 1U)
    printf("  [INFO] OS_CONFIG_WORK_STACK_SIZE      = %lu bytes (tsk_work)\r\n",
           (unsigned long)OS_CONFIG_WORK_STACK_SIZE);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    printf("  [INFO] OS_CONFIG_TIMER_STACK_SIZE     = %lu bytes (tsk_timer)\r\n",
           (unsigned long)OS_CONFIG_TIMER_STACK_SIZE);
#endif
    printf("  [INFO] OS_CONFIG_MAIN_TASK_STACK_SIZE = %lu bytes (tsk_main)\r\n",
           (unsigned long)OS_CONFIG_MAIN_TASK_STACK_SIZE);
    printf("  [INFO] OS_CONFIG_TEST_STACK_SIZE      = %lu bytes (tsk_test, this task)\r\n",
           (unsigned long)OS_CONFIG_TEST_STACK_SIZE);

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    {
        size_t    self_min_free;
        size_t    worker_min_free;
        os_status status;

        if (os_task_stack_watermark_get(NULL, &self_min_free) == OS_STATUS_OK)
        {
            printf("  [INFO] tsk_test peak stack usage so far: %lu / %lu bytes (%lu%% headroom left)\r\n",
                   (unsigned long)(OS_CONFIG_TEST_STACK_SIZE - self_min_free),
                   (unsigned long)OS_CONFIG_TEST_STACK_SIZE,
                   (unsigned long)((self_min_free * 100U) / OS_CONFIG_TEST_STACK_SIZE));
        }

        /* Give a freshly created task a moment to run, then read its watermark too - the same
         * feature applied to a task other than "self". */
        os_test_busy_counter    = 0U;
        os_test_busy_should_run = true;
        status = os_task_create(&worker, OS_TASK_CONFIG(test_busy_spin_entry, NULL, TEST_PRIO_LOW));
        if (status == OS_STATUS_OK)
        {
            (void)os_task_start(&worker);
            os_delay_ms(20U);
            os_test_busy_should_run = false;

            if (os_task_stack_watermark_get(&worker, &worker_min_free) == OS_STATUS_OK)
            {
                printf("  [INFO] worker task peak stack usage: %lu / %lu bytes (%lu%% headroom left)\r\n",
                       (unsigned long)(sizeof(worker_STACK) - worker_min_free),
                       (unsigned long)sizeof(worker_STACK),
                       (unsigned long)((worker_min_free * 100U) / sizeof(worker_STACK)));
            }

            (void)test_wait_inactive(&worker, 200U);
        }
    }
#else
    printf("  [SKIP] OS_CONFIG_STACK_WATERMARK_ENABLE=0: no watermark data available\r\n");
#endif
}

/******************************************************************************************************/
/**
 * @brief Estimates context-switch overhead: two equal-priority tasks ping-pong the CPU (each
 *        increments a shared counter then yields) for a fixed window; dividing the window by
 *        the total switch count gives an average, tick-resolution estimate of switch cost.
 *
 * There is no public cycle-counter API (os_test.c deliberately depends on nothing but ahura.h),
 * so this cannot report single-switch microsecond precision - only an average over many
 * thousands of switches, which is precise enough to be meaningful at 1 ms tick resolution.
 */
static void test_context_switch_timing(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  window_ms;
    uint32_t  switches;
    os_status status;

    test_print_section("Context Switch Timing (informational, tick-resolution estimate)");

    os_test_switch_count      = 0U;
    os_test_switch_should_run = true;

    status = os_task_create(&worker, OS_TASK_CONFIG(test_switch_ping_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "ping task created for the switch benchmark (priority 1)");
    status = os_task_create(&helper, OS_TASK_CONFIG(test_switch_ping_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "pong task created for the switch benchmark (priority 1)");

    t0 = os_tick_get();
    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    os_delay_ms(200U); /* let them ping-pong for a fixed window */
    os_test_switch_should_run = false;
    t1 = os_tick_get();

    switches  = os_test_switch_count;
    window_ms = t1 - t0;
    AHURA_TEST_CHECK(switches > 0U, "ping/pong tasks performed context switches (count=%lu)",
                      (unsigned long)switches);

    if (switches > 0U)
    {
        uint32_t avg_switch_us = (window_ms * 1000U) / switches;

        printf("  [INFO] ~%lu switches in %lu ms -> ~%lu us/switch average (includes loop overhead)\r\n",
               (unsigned long)switches, (unsigned long)window_ms, (unsigned long)avg_switch_us);
    }

    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "ping task stops cleanly");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "pong task stops cleanly");
}

/*
 * ***********************************************************************************************************
 * Tickless sleep hooks (called directly, in isolation - see the caveat printed below)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Exercises os_tickless_pre_sleep_cb()/os_tickless_post_sleep_cb() directly, in isolation
 *        from the idle task and tick accounting.
 *
 * os_tickless_idle_process() is not yet invoked by the idle task (see the kernel README "Tickless
 * idle") - the idle task still just does a plain WFI - so OS_CONFIG_TICKLESS_ENABLE currently has
 * no other observable runtime effect. This only proves the two hooks themselves run safely and
 * quickly and compose correctly back-to-back; it is not an end-to-end tickless sleep test.
 */
static void test_tickless_hooks(void)
{
#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
    uint32_t t0;
    uint32_t t1;

    test_print_section("Tickless Sleep Hooks (called directly, not via the idle task)");

    printf("  [INFO] os_tickless_idle_process() is not invoked by the idle task yet (see kernel\r\n"
           "         README \"Tickless idle\") - this only tests the two hooks in isolation.\r\n");

    /* Call right after a print still in flight: the realistic scenario the pre-sleep hook exists
     * for on this project (flush COM1 before the CPU would idle - see os_cb.c). */
    printf("  [INFO] flushing this line before the (simulated) sleep point...\r\n");
    t0 = os_tick_get();
    os_tickless_pre_sleep_cb();
    t1 = os_tick_get();
    AHURA_TEST_CHECK((t1 - t0) <= 20U, "os_tickless_pre_sleep_cb() returns promptly (%lu ticks)",
                      (unsigned long)(t1 - t0));

    t0 = os_tick_get();
    os_tickless_post_sleep_cb();
    t1 = os_tick_get();
    AHURA_TEST_CHECK((t1 - t0) <= 20U, "os_tickless_post_sleep_cb() returns promptly (%lu ticks)",
                      (unsigned long)(t1 - t0));

    AHURA_TEST_CHECK(os_kernel_is_running(), "kernel state is intact after calling both hooks directly");

    /* Paired back-to-back, the same way os_tickless_idle_process() calls them. */
    os_tickless_pre_sleep_cb();
    os_tickless_post_sleep_cb();
    AHURA_TEST_CHECK(os_kernel_is_running(), "kernel state is intact after a paired pre/post call");
#else
    /* The hooks are only declared when tickless idle is enabled, since the application is only
     * required to define them then, so there is nothing to call here. */
    test_print_section("Tickless Sleep Hooks (called directly, not via the idle task)");
    printf("  [SKIP] requires OS_CONFIG_TICKLESS_ENABLE=1\r\n");
#endif /* OS_CONFIG_TICKLESS_ENABLE */
}

#if (OS_CONFIG_TICKLESS_ENABLE == 1U) && (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief End-to-end tickless sleep, called directly (bypassing the not-yet-wired idle task, same
 *        as test_tickless_hooks() does for the sleep-bracket callbacks): arms a one-shot timer as
 *        a horizon, calls os_tickless_idle_process() once, and checks the real elapsed time was
 *        measured accurately - proving actual SysTick suppression, not just that the call is
 *        safe. Fails against a plain-WFI (un-suppressed) OS_ARCH_SLEEP, since the CPU would then
 *        wake at the very next real tick regardless of the requested horizon.
 *
 * The horizon is derived from os_tickless_max_suppressed_ticks_get() at runtime rather than any
 * fixed tick count: the safe suppressible window is register-width limited (e.g. SysTick's 24-bit
 * reload), so it depends on both the platform clock and OS_CONFIG_TICK_HZ - a constant tuned for
 * one board/speed could silently collide with the cap, or with too-small a window to measure
 * meaningfully, on another. This test holds across whatever platform/clock speed it runs on.
 */
static void test_tickless_sleep(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;
    uint32_t  max_suppressed;
    uint32_t  horizon;
    uint32_t  tolerance_low;
    uint32_t  tolerance_high;
    uint32_t  mask_before;
    uint32_t  mask_after;
    os_status init_status;
    os_status start_status;

    test_print_section("Tickless Sleep (end-to-end, real hardware timing)");

    max_suppressed = os_tickless_max_suppressed_ticks_get();

    if (max_suppressed < 4U)
    {
        printf("  [SKIP] os_tickless_max_suppressed_ticks_get() = %lu: this port does not yet\r\n"
               "         suppress ticking for real (see kernel README \"Tickless idle\"), or the\r\n"
               "         current clock/tick-rate combination allows too small a window to test.\r\n",
               (unsigned long)max_suppressed);
        return;
    }

    printf("  [INFO] calling os_tickless_idle_process() directly from this task (not the idle\r\n"
           "         task - see kernel README \"Tickless idle\") to verify the suppress/measure\r\n"
           "         mechanism before that wiring lands.\r\n");

    /* Half the safe maximum, floored at the maximum itself when that is already small, capped so
     * the test does not run unreasonably long on a platform where the safe window is huge. */
    horizon = max_suppressed / 2U;
    if (horizon < 4U)
    {
        horizon = max_suppressed;
    }
    if (horizon > 50U)
    {
        horizon = 50U;
    }

    /* Arm silently and sample t0 immediately after: any printf here would block on a polled
     * UART transmit and eat into the window we are about to measure. Check/report status once
     * the timing-critical section below is over instead. */
    os_test_oneshot_fired = 0U;
    init_status  = os_timer_init(&os_test_timer_oneshot, horizon, OS_TIMER_MODE_ONE_SHOT, timer_oneshot_cb, NULL);
    start_status = os_timer_start(&os_test_timer_oneshot);

    mask_before = os_arch_kernel_mask_active();

    t0 = os_tick_get();
    os_tickless_idle_process();
    t1 = os_tick_get();
    delta = t1 - t0;

    mask_after = os_arch_kernel_mask_active();

    /* A little slack either side for scheduling/measurement rounding, scaled to stay meaningful
     * for small horizons too (a fixed +/-N would be too tight for a tiny horizon and too loose
     * for a large one). */
    tolerance_low  = (horizon > 2U) ? (horizon - 2U) : 1U;
    tolerance_high = horizon + 5U;

    /* The sleep path masks interrupts before it decides how long to sleep, and the port masks
     * again inside os_arch_sleep_prepare, so two save/restore pairs are nested. Getting that
     * wrong leaves the core masked on return, which does not fail loudly - the system simply
     * stops taking interrupts and looks hung - so it is worth asserting directly rather than
     * inferring from later tests. */
    AHURA_TEST_CHECK(mask_after == mask_before,
                      "os_tickless_idle_process() restored the interrupt mask it found "
                      "(before=0x%08lX after=0x%08lX)",
                      (unsigned long)mask_before, (unsigned long)mask_after);

    AHURA_TEST_CHECK(init_status == OS_STATUS_OK, "os_timer_init() arms a %lu-tick horizon for the sleep test",
                      (unsigned long)horizon);
    AHURA_TEST_CHECK(start_status == OS_STATUS_OK, "one-shot timer started");
    AHURA_TEST_CHECK((delta >= tolerance_low) && (delta <= tolerance_high),
                      "os_tickless_idle_process() slept ~%lu ticks and measured it accurately (delta=%lu)",
                      (unsigned long)horizon, (unsigned long)delta);
    AHURA_TEST_CHECK(os_kernel_is_running(), "kernel state is intact after a real tickless sleep/wake cycle");

    os_delay_ms(5U); /* let the timer service task run the callback */
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "the timer bounding the sleep fired exactly once (fired=%lu)",
                      (unsigned long)os_test_oneshot_fired);

    (void)os_timer_stop(&os_test_timer_oneshot);
}
#else
/******************************************************************************************************/
static void test_tickless_sleep(void)
{
    test_print_section("Tickless Sleep (end-to-end)");
    printf("  [SKIP] requires OS_CONFIG_TICKLESS_ENABLE=1 and OS_CONFIG_TIMER_ENABLE=1\r\n");
}
#endif /* OS_CONFIG_TICKLESS_ENABLE && OS_CONFIG_TIMER_ENABLE */

/*
 * ***********************************************************************************************************
 * Benchmarks
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Print one benchmark row from a measured minimum cycle count.
 *
 * @param[in] name      Operation label.
 * @param[in] cycles    Minimum cycles observed, measurement overhead already subtracted.
 * @param[in] clock_hz  CPU clock for the ns conversion, 0 when unknown.
 */
static void test_bench_row(const char *name, uint32_t cycles, uint32_t clock_hz)
{
    if (clock_hz != 0U)
    {
        /* 64-bit throughout: a slow core with a big cycle count would overflow 32 bits here. */
        uint64_t ns = ((uint64_t)cycles * 1000000000ULL) / (uint64_t)clock_hz;

        printf("  %-40s %10lu %10lu\r\n", name, (unsigned long)cycles, (unsigned long)ns);
    }
    else
    {
        printf("  %-40s %10lu %10s\r\n", name, (unsigned long)cycles, "n/a");
    }
}

/******************************************************************************************************/
/**
 * @brief Timed cost of every hot kernel path, printed as a table at the end of the run.
 *
 * All measurements are UNCONTENDED fast paths (no blocking, no waiter wakeups) - the cost an
 * application pays per call in the common case. Every row includes the loop's own overhead, so
 * the first row measures an empty loop: subtract it to get the kernel call's own cost.
 *
 * These are real numbers from real silicon, not estimates, but they depend on the compiler's
 * optimization level, flash wait states / caching, and whatever else the board is doing - treat
 * them as a baseline to track regressions against, not as absolute specifications.
 */
static void test_benchmarks(void)
{
    __IO uint32_t sink = 0U;
    uint32_t          best;
    uint32_t          overhead;
    uint32_t          clock_hz = os_arch_clock_hz_get();

    printf("\r\n========================================\r\n");
    printf(" BENCHMARKS\r\n");
    printf("========================================\r\n");

    /* Architecture profile from the compiler's own target macros - the same ones the port
     * layer selects on, so this always names the code actually running. */
    printf("  core      : ");
#if defined(__ARM_ARCH_6M__)
    printf("ARMv6-M (Cortex-M0/M0+)");
#elif defined(__ARM_ARCH_7M__)
    printf("ARMv7-M (Cortex-M3)");
#elif defined(__ARM_ARCH_7EM__)
    printf("ARMv7E-M (Cortex-M4/M7)");
#elif defined(__ARM_ARCH_8M_BASE__)
    printf("ARMv8-M baseline (Cortex-M23)");
#elif defined(__ARM_ARCH_8M_MAIN__)
    printf("ARMv8-M mainline (Cortex-M33/M35P)");
#elif defined(__ARM_ARCH_8_1M_MAIN__)
    printf("ARMv8.1-M mainline (Cortex-M52/M55/M85)");
#else
    printf("unknown ARM profile");
#endif
#if defined(__ARM_FP)
    printf(", FPU");
#else
    printf(", no FPU");
#endif
#if defined(__ARM_FEATURE_MVE)
    printf(", MVE");
#elif defined(__ARM_FEATURE_DSP)
    printf(", DSP");
#endif
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_SECURE)
    printf(", TrustZone secure");
#elif (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
    printf(", TrustZone non-secure");
#endif
    printf("\r\n");

    /* GCC exposes no macro for the numeric -O level, only these category flags, so report the
     * category rather than guessing a number that could be wrong. */
    printf("  build     : ");
#if !defined(__OPTIMIZE__)
    printf("-O0, NO optimization - expect several times slower than a release build");
#elif defined(__OPTIMIZE_SIZE__)
    printf("-Os, optimized for size");
#else
    printf("-O1/-O2/-O3, optimized for speed");
#endif
    printf(", %u-bit\r\n", (unsigned)(sizeof(void *) * 8U));

    printf("  clocks    : tick %lu Hz", (unsigned long)OS_CONFIG_TICK_HZ);
    if (clock_hz != 0U)
    {
        printf(", CPU %lu Hz\r\n", (unsigned long)clock_hz);
    }
    else
    {
        printf(", CPU clock unknown (cycles/op unavailable)\r\n");
    }

    printf("\r\n  Each operation is measured alone with the CPU cycle counter, sampled %u times,\r\n",
           (unsigned)TEST_BENCH_SAMPLES);
    printf("  keeping the MINIMUM. Interference (the 1 kHz tick ISR, cache misses) only ever adds\r\n");
    printf("  cycles, so the minimum is the true uninterrupted cost. The cost of the two counter\r\n");
    printf("  reads is measured the same way and already subtracted from every row below.\r\n\r\n");

    printf("  %-40s %10s %10s\r\n", "Operation (uncontended fast path)", "cycles", "ns");
    printf("  ------------------------------------------------------------\r\n");

    /* Cost of the measurement itself: two counter reads with nothing between them. Subtracted
     * from every row, so a row shows the operation's own cycles and nothing else. */
    TEST_BENCH_MIN_CYCLES(overhead, TEST_BENCH_SAMPLES, (void)0);
    test_bench_row("(measurement overhead, subtracted)", overhead, clock_hz);

    TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES, sink += os_tick_get());
    test_bench_row("os_tick_get", TEST_BENCH_SUB(best, overhead), clock_hz);

    TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES, os_critical_enter(); os_critical_exit());
    test_bench_row("os_critical_enter + exit", TEST_BENCH_SUB(best, overhead), clock_hz);

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    if (os_mutex_init(&os_test_bench_mutex) == OS_STATUS_OK)
    {
        TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES,
                              (void)os_mutex_lock(&os_test_bench_mutex, OS_WAIT_FOREVER);
                              (void)os_mutex_unlock(&os_test_bench_mutex));
        test_bench_row("os_mutex_lock + unlock", TEST_BENCH_SUB(best, overhead), clock_hz);

        TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES,
                              (void)os_mutex_try_lock(&os_test_bench_mutex);
                              (void)os_mutex_unlock(&os_test_bench_mutex));
        test_bench_row("os_mutex_try_lock + unlock", TEST_BENCH_SUB(best, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    if (os_semaphore_init(&os_test_bench_sem, 0U, 1U) == OS_STATUS_OK)
    {
        TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES,
                              (void)os_semaphore_give(&os_test_bench_sem);
                              (void)os_semaphore_take(&os_test_bench_sem, OS_WAIT_NOTHING));
        test_bench_row("os_semaphore_give + take", TEST_BENCH_SUB(best, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    if (os_queue_cleanup(&os_test_bench_queue) == OS_STATUS_OK)
    {
        uint32_t item = 0x5A5A5A5AUL;
        uint32_t out;

        TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES,
                              (void)os_queue_send(&os_test_bench_queue, &item, OS_WAIT_NOTHING);
                              (void)os_queue_receive(&os_test_bench_queue, &out, OS_WAIT_NOTHING));
        test_bench_row("os_queue_send + receive (4-byte item)", TEST_BENCH_SUB(best, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_EVENT_ENABLE == 1U)
    if (os_event_init(&os_test_bench_event) == OS_STATUS_OK)
    {
        uint32_t matched;

        TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES,
                              (void)os_event_set_bits(&os_test_bench_event, 0x01U);
                              (void)os_event_wait_bits(&os_test_bench_event, 0x01U, false, true,
                                                              &matched, OS_WAIT_NOTHING));
        test_bench_row("os_event_set + wait (immediate)", TEST_BENCH_SUB(best, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    /* Created but never started: give() then only latches, which is exactly the ISR-side cost
     * an application cares about (the wake path is a context switch, measured below). */
    if (os_task_create(&helper, OS_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_STATUS_OK)
    {
        TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES, (void)os_notify_give(&helper, 1U));
        test_bench_row("os_notify_give (latch, no wake)", TEST_BENCH_SUB(best, overhead), clock_hz);

        (void)os_task_delete(&helper);
    }
#endif

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    {
        void *p;

        TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES, p = os_mem_alloc(64U); os_mem_free(p));
        test_bench_row("os_mem_alloc + os_mem_free (64 B)", TEST_BENCH_SUB(best, overhead), clock_hz);
    }
#endif

    /* os_task_yield always pends PendSV, so this is a FULL context-switch round trip - register
     * save, scheduler pick, register restore - that happens to re-select this same task because
     * nothing else is ready. That makes it the cleanest single-number context-switch cost:
     * no second task's cache/branch-predictor effects mixed in. */
    TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_SAMPLES, os_task_yield());
    test_bench_row("os_task_yield (switch, re-selects self)", TEST_BENCH_SUB(best, overhead), clock_hz);

    TEST_BENCH_MIN_CYCLES(best, TEST_BENCH_HEAVY_SAMPLES,
                          if (os_task_create(&helper, OS_TASK_CONFIG(test_worker_entry,
                                                                      NULL, 1U)) == OS_STATUS_OK)
                          {
                              (void)os_task_delete(&helper);
                          });
    test_bench_row("os_task_create + os_task_delete", TEST_BENCH_SUB(best, overhead), clock_hz);

    printf("  ------------------------------------------------------------\r\n");
    (void)sink;
}

/*
 * ***********************************************************************************************************
 * Intrusive list (always compiled in - the scheduler runs on it)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void test_list(void)
{
    os_list_t      list;
    os_list_node_t a;
    os_list_node_t b;
    os_list_node_t c;

    test_print_section("Intrusive List");

    os_list_init(&list);
    AHURA_TEST_CHECK(os_list_is_empty(&list), "a freshly initialized list is empty");

    os_list_push_back(&list, &a);
    os_list_push_back(&list, &b);
    os_list_push_back(&list, &c);
    AHURA_TEST_CHECK(!os_list_is_empty(&list), "list is non-empty after push_back");
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &a, "pop_front returns nodes in FIFO order (1st = a)");

    os_list_remove(&list, &c);
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &b, "removing a non-head node leaves the rest intact (2nd = b)");
    AHURA_TEST_CHECK(os_list_is_empty(&list), "list is empty after removing/popping everything pushed");

    os_list_push_back(&list, &a);
    os_list_insert_before(&list, &a, &b);
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &b, "insert_before(head) places the new node ahead of it");
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &a, "the original head follows");

    os_list_push_back(&list, &a);
    os_list_insert_before(&list, NULL, &b);
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &a, "insert_before(NULL) appends at the tail (a stays head)");
    AHURA_TEST_CHECK(os_list_pop_front(&list) == &b, "the appended node comes out last");

    os_list_remove(&list, &c); /* c is not in any list: must be a safe no-op */
    AHURA_TEST_CHECK(os_list_is_empty(&list), "removing a node that is not in the list is a safe no-op");
}

/*
 * ***********************************************************************************************************
 * Config-gated features (multi-core / TrustZone / tickless)
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
static void test_unsupported_features(void)
{
    test_print_section("Multi-core / TrustZone / Tickless (config-gated, informational)");

#if (OS_CONFIG_CORE_COUNT > 1U)
    printf("  [INFO] multi-core APIs compiled in (OS_CONFIG_CORE_COUNT=%u) - not exercised by this suite\r\n",
           (unsigned)OS_CONFIG_CORE_COUNT);
#else
    printf("  [SKIP] multi-core APIs compiled out (OS_CONFIG_CORE_COUNT=1: this build is single-core)\r\n");
#endif

#if (OS_CONFIG_TRUSTZONE != OS_CONFIG_TRUSTZONE_DISABLED)
    printf("  [INFO] TrustZone callbacks compiled in - not exercised by this suite\r\n");
#else
    printf("  [SKIP] TrustZone disabled (OS_CONFIG_TRUSTZONE_DISABLED)\r\n");
#endif

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
    printf("  [INFO] tickless idle enabled - not functionally wired in yet (see kernel README)\r\n");
#else
    printf("  [SKIP] tickless idle disabled (OS_CONFIG_TICKLESS_ENABLE=0)\r\n");
#endif
}

/*
 * ***********************************************************************************************************
 * Regressions for fixed defects
 * ***********************************************************************************************************
 *
 * What these share is a method rather than a subsystem: each needs an interleaving the scheduler
 * would not normally produce, and os_kernel_lock is what makes those reachable on target - it
 * holds a woken task in READY, with the tick and every interrupt still running, for as long as
 * the test needs. Messages are kept short here: this suite is already close to the flash limit.
*/

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
static os_semaphore_t os_test_reg_sem;
static __IO uint32_t  os_test_reg_order   = 0U;
static __IO uint32_t  os_test_reg_a_order = 0U;
static __IO os_status os_test_reg_a_st    = OS_STATUS_ERROR;
static __IO os_status os_test_reg_b_st    = OS_STATUS_ERROR;

/******************************************************************************************************/
static void test_reg_waiter_b(void *context)
{
    (void)context;
    os_test_reg_b_st = os_semaphore_take(&os_test_reg_sem, 400U);
    (void)++os_test_reg_order;
}

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_mutex_t os_test_reg_mutex;

/* Low priority: holds the mutex, then queues on the semaphore behind a higher-priority waiter.
 * The boost it takes while blocked there is what must re-sort it to the head of that queue. */
/******************************************************************************************************/
static void test_reg_boosted_entry(void *context)
{
    (void)context;

    if (os_mutex_lock(&os_test_reg_mutex, 200U) != OS_STATUS_OK)
    {
        return;
    }

    os_test_reg_a_st    = os_semaphore_take(&os_test_reg_sem, 400U);
    os_test_reg_a_order = ++os_test_reg_order;

    (void)os_mutex_unlock(&os_test_reg_mutex);
}

/* Highest priority: contends the mutex purely to trigger the inheritance boost. */
/******************************************************************************************************/
static void test_reg_booster_entry(void *context)
{
    (void)context;

    if (os_mutex_lock(&os_test_reg_mutex, 400U) == OS_STATUS_OK)
    {
        (void)os_mutex_unlock(&os_test_reg_mutex);
    }
}
#endif /* OS_CONFIG_MUTEX_ENABLE */
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
static void test_reg_timer_cb(void *context)
{
    (void)context;
}
#endif

/******************************************************************************************************/
/**
 * @brief Helper for the priority test: records that it ran, then returns. Deliberately has no
 *        loop - a spinning helper raised above the suite's own task would never hand the CPU
 *        back, whatever it yielded.
 */
static __IO uint32_t os_test_prio_ran = 0U;

/******************************************************************************************************/
static void test_prio_entry(void *context)
{
    (void)context;
    os_test_prio_ran++;
}

/******************************************************************************************************/
/**
 * @brief The public priority API: read back what was set, refuse what is out of range, and take
 *        effect immediately.
 */
static void test_priority_api(void)
{
    os_task_priority_t priority = OS_TASK_PRIO_1;
    os_status          status;

    test_print_section("Task Priority (get / set)");

    status = os_task_create(&worker, OS_TASK_CONFIG(test_worker_entry, NULL, OS_TASK_PRIO_2));
    AHURA_TEST_CHECK(status == OS_STATUS_OK, "worker created at priority 2");

    AHURA_TEST_CHECK(os_task_priority_get(&worker, &priority) == OS_STATUS_OK, "priority read back");
    AHURA_TEST_CHECK(priority == OS_TASK_PRIO_2, "it reports what creation asked for (%u)",
                      (unsigned)priority);

    AHURA_TEST_CHECK(os_task_priority_set(&worker, OS_TASK_PRIO_5) == OS_STATUS_OK, "priority raised to 5");
    (void)os_task_priority_get(&worker, &priority);
    AHURA_TEST_CHECK(priority == OS_TASK_PRIO_5, "the new priority is what comes back (%u)",
                      (unsigned)priority);

    /* The suite's own task must still be able to read and restore its own priority. */
    AHURA_TEST_CHECK(os_task_priority_get(NULL, &priority) == OS_STATUS_OK,
                      "NULL means the calling task");
    AHURA_TEST_CHECK(priority == (os_task_priority_t)OS_CONFIG_TEST_PRIORITY,
                      "and reports the suite's own configured priority (%u)", (unsigned)priority);

    /* Levels outside the user range belong to the idle task and the kernel's service tasks. */
    AHURA_TEST_CHECK(os_task_priority_set(&worker, (os_task_priority_t)0U) == OS_STATUS_INVALID_ARG,
                      "priority 0 (idle) is refused");
    AHURA_TEST_CHECK(os_task_priority_set(&worker, OS_TASK_PRIO_MAX) == OS_STATUS_INVALID_ARG,
                      "OS_TASK_PRIO_MAX (kernel service level) is refused");
    AHURA_TEST_CHECK(os_task_priority_get(&worker, NULL) == OS_STATUS_INVALID_ARG,
                      "a NULL output pointer is refused");

    /* A refused set must not have changed anything. */
    (void)os_task_priority_get(&worker, &priority);
    AHURA_TEST_CHECK(priority == OS_TASK_PRIO_5, "a refused set left the priority alone (%u)",
                      (unsigned)priority);

    (void)os_task_delete(&worker);

    {
        os_task_t stale = { 0 };

        AHURA_TEST_CHECK(os_task_priority_get(&stale, &priority) == OS_STATUS_INVALID_ARG,
                          "an unknown handle is refused by get");
        AHURA_TEST_CHECK(os_task_priority_set(&stale, OS_TASK_PRIO_3) == OS_STATUS_INVALID_ARG,
                          "an unknown handle is refused by set");
    }

    /* A change takes effect at once, not at the next dispatch. The helper runs to completion and
     * exits on its own, so it can be raised above this task without being able to starve it. */
    os_test_prio_ran = 0U;
    (void)os_task_create(&worker, OS_TASK_CONFIG(test_prio_entry, NULL, TEST_PRIO_LOW));
    (void)os_task_start(&worker);

    AHURA_TEST_CHECK(os_test_prio_ran == 0U, "a task below this one stays ready without running");

    (void)os_task_priority_set(&worker, (os_task_priority_t)TEST_PRIO_HIGH);

    /* No delay here on purpose: had the switch waited for the next tick, this would still read 0. */
    AHURA_TEST_CHECK(os_test_prio_ran == 1U,
                      "raising it above this task ran it before the next line (ran=%lu)",
                      (unsigned long)os_test_prio_ran);

    (void)test_wait_inactive(&worker, 300U);
}

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Queue occupancy accounting: count and free must always agree with the capacity.
 */
static void test_queue_accounting(void)
{
    const size_t capacity = sizeof(os_test_queue_buf) / sizeof(os_test_queue_buf[0]);
    uint32_t     item     = 0U;
    size_t       sent     = 0U;
    bool         held     = true;

    test_print_section("Queue Accounting (count / free)");

    (void)os_queue_cleanup(&os_test_queue);   /* empties it; the array is not the queue's to free */

    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 0U, "an emptied queue holds nothing");
    AHURA_TEST_CHECK(os_queue_free_get(&os_test_queue) == capacity,
                      "and reports its whole capacity free (%lu)", (unsigned long)capacity);

    while (os_queue_send(&os_test_queue, &item, OS_WAIT_NOTHING) == OS_STATUS_OK)
    {
        item++;
        sent++;

        if ((os_queue_count_get(&os_test_queue) + os_queue_free_get(&os_test_queue)) != capacity)
        {
            held = false;
        }
    }

    AHURA_TEST_CHECK(held, "count + free equalled capacity after every send");

    AHURA_TEST_CHECK(sent == capacity, "it accepted exactly capacity items (%lu)", (unsigned long)sent);
    AHURA_TEST_CHECK(os_queue_free_get(&os_test_queue) == 0U, "a full queue reports no free slots");
    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == capacity, "and a count of capacity");

    held = true;
    while (os_queue_receive(&os_test_queue, &item, OS_WAIT_NOTHING) == OS_STATUS_OK)
    {
        if ((os_queue_count_get(&os_test_queue) + os_queue_free_get(&os_test_queue)) != capacity)
        {
            held = false;
        }
    }

    AHURA_TEST_CHECK(held, "count + free equalled capacity after every receive");

    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 0U, "the drained queue holds nothing");
    AHURA_TEST_CHECK(os_queue_free_get(&os_test_queue) == capacity, "and is all free again");
}

#endif /* OS_CONFIG_QUEUE_ENABLE */

/******************************************************************************************************/
/**
 * @brief Regression checks: tick saturation, wake handoff on pause, priority-boost re-ordering,
 *        timer restart with an undrained expiry, and timer registry slot release.
 */
static void test_regressions(void)
{
    test_print_section("Regressions");

    /* A duration too large for the tick range must clamp, never wrap to a small plausible count
     * and never land on the "wait forever" sentinel by accident. */
    AHURA_TEST_CHECK(OS_TICKS_FROM_MS(0xFFFFFFFFU) == (OS_WAIT_FOREVER - 1U), "ms conversion saturates");
    AHURA_TEST_CHECK(OS_TICKS_FROM_S(0xFFFFFFFFU) == (OS_WAIT_FOREVER - 1U), "s conversion saturates");

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    /* An unconsumed wake is handed on, not lost with the task that never used it. */
    {
        os_test_reg_b_st = OS_STATUS_ERROR;
        (void)os_semaphore_init(&os_test_reg_sem, 0U, 2U);

        (void)os_task_create(&helper, OS_TASK_CONFIG(test_reg_waiter_b, NULL, TEST_PRIO_LOW));
        (void)os_task_start(&helper);
        (void)os_task_create(&helper2, OS_TASK_CONFIG(test_reg_waiter_b, NULL, TEST_PRIO_LOW));
        (void)os_task_start(&helper2);
        os_delay_ms(20U);   /* both are below this task, so they queue while it sleeps */

        /* The window: give() wakes the first waiter, the lock stops it running, and the pause
         * removes it before it can consume the token. */
        os_kernel_lock();
        (void)os_semaphore_give(&os_test_reg_sem);
        (void)os_task_pause(&helper);
        os_kernel_unlock();

        os_delay_ms(40U);
        AHURA_TEST_CHECK(os_test_reg_b_st == OS_STATUS_OK, "paused task's wake passed to the next waiter");
        AHURA_TEST_CHECK(os_task_state_get(&helper) == OS_TASK_STATE_SUSPENDED, "paused task stayed paused");

        (void)os_task_delete(&helper);
        (void)test_wait_inactive(&helper2, 500U);
    }

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    /* A boost must re-sort a task already queued on some OTHER object.
     *
     * The three levels are fixed rather than derived from OS_CONFIG_TEST_PRIORITY: only their
     * order matters, and the bottom three user levels exist in every build, so this runs whatever
     * the suite itself is configured to run at. */
    {
        os_test_reg_order   = 0U;
        os_test_reg_a_order = 0U;
        os_test_reg_a_st    = OS_STATUS_ERROR;
        os_test_reg_b_st    = OS_STATUS_ERROR;

        (void)os_semaphore_init(&os_test_reg_sem, 0U, 2U);
        (void)os_mutex_init(&os_test_reg_mutex);

        (void)os_task_create(&helper, OS_TASK_CONFIG(test_reg_boosted_entry, NULL, OS_TASK_PRIO_1));
        (void)os_task_start(&helper);
        os_delay_ms(20U);   /* holds the mutex, now queued on the semaphore */

        /* Higher priority, so the waiter list puts it AHEAD of the low-priority holder. */
        (void)os_task_create(&helper2, OS_TASK_CONFIG(test_reg_waiter_b, NULL, OS_TASK_PRIO_2));
        (void)os_task_start(&helper2);
        os_delay_ms(20U);

        /* Contending the mutex boosts its owner above that waiter. */
        (void)os_task_create(&helper3, OS_TASK_CONFIG(test_reg_booster_entry, NULL, OS_TASK_PRIO_3));
        (void)os_task_start(&helper3);
        os_delay_ms(20U);

        (void)os_semaphore_give(&os_test_reg_sem);
        os_delay_ms(30U);

        AHURA_TEST_CHECK(os_test_reg_a_order == 1U, "boosted waiter woken first (order=%lu)",
                          (unsigned long)os_test_reg_a_order);

        (void)os_semaphore_give(&os_test_reg_sem);
        AHURA_TEST_CHECK(test_wait_inactive(&helper, 500U) && test_wait_inactive(&helper2, 500U) &&
                          test_wait_inactive(&helper3, 500U), "all three helpers finished");
    }
#endif /* OS_CONFIG_MUTEX_ENABLE */
#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

#if (OS_CONFIG_TIMER_ENABLE == 1U)
    /* Restart must discard an expiry the tick noted but the timer task has not drained. */
    {
        os_test_oneshot_fired = 0U;
        (void)os_timer_init(&os_test_timer_oneshot, OS_TICKS_FROM_MS(30U), OS_TIMER_MODE_ONE_SHOT,
                            timer_oneshot_cb, NULL);
        (void)os_timer_start(&os_test_timer_oneshot);

        /* The lock keeps the timer service task off the CPU while the tick keeps running, so the
         * expiry is flagged and left undrained - the state a restart used to inherit. */
        os_kernel_lock();
        os_delay_ms(45U);   /* busy-waits under the lock; ticks still arrive */
        (void)os_timer_restart(&os_test_timer_oneshot);
        os_kernel_unlock();

        os_delay_ms(15U);
        AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "restart dropped the undrained expiry");
        os_delay_ms(40U);
        AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "and fired a full period later");
        (void)os_timer_stop(&os_test_timer_oneshot);
    }

    /* Re-initializing a started timer must return its registry slot. */
    {
        uint32_t index;

        for (index = 0U; index < OS_CONFIG_MAX_TIMERS; index++)
        {
            /* Far longer than this section runs, so none of them can fire and disturb it. */
            (void)os_timer_init(&os_test_tflood[index], OS_TICKS_FROM_MS(60000U),
                                OS_TIMER_MODE_PERIODIC, test_reg_timer_cb, NULL);
            (void)os_timer_start(&os_test_tflood[index]);
        }

        (void)os_timer_init(&os_test_tflood_extra, OS_TICKS_FROM_MS(60000U), OS_TIMER_MODE_PERIODIC,
                            test_reg_timer_cb, NULL);
        AHURA_TEST_CHECK(os_timer_start(&os_test_tflood_extra) == OS_STATUS_FULL, "registry full");

        (void)os_timer_init(&os_test_tflood[0], OS_TICKS_FROM_MS(60000U), OS_TIMER_MODE_PERIODIC,
                            test_reg_timer_cb, NULL);
        AHURA_TEST_CHECK(os_timer_start(&os_test_tflood_extra) == OS_STATUS_OK, "re-init freed its slot");

        (void)os_timer_stop(&os_test_tflood_extra);
        for (index = 0U; index < OS_CONFIG_MAX_TIMERS; index++)
        {
            (void)os_timer_stop(&os_test_tflood[index]);
        }
    }
#endif /* OS_CONFIG_TIMER_ENABLE */
}

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Kernel self-test suite entry point, supplying the os_test() declared in ahura.h.
 *        os_kernel.c creates a task that calls this automatically when OS_CONFIG_TEST_ENABLE
 *        is 1 - nothing else to call.
 */
void os_test(void)
{
    printf("\r\n========================================\r\n");
    printf(" Ahura RTOS self-test suite starting...\r\n");
    printf("========================================\r\n");

    test_kernel_core();
    test_delay();
    test_critical_section();
    test_task_lifecycle();
    test_task_identity();
    test_priority_preemption();
    test_scheduler_lock();

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    test_mutex();
#endif
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    test_semaphore();
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    test_queue();
    test_queue_define_and_dynamic();
#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
    test_atomic();
#endif
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
    test_event_group();
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    test_timer();
#endif
#if (OS_CONFIG_WORK_ENABLE == 1U)
    test_work();
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    test_task_notify();
#endif
    test_assert();
    test_log();
#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    test_alloc();
#endif
#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
    test_stack_watermark();
#endif
#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
    test_cpu_usage();
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
    test_pipeline();
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    test_mutex_priority_ordering();
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    test_mutex_priority_inheritance();
    test_mutex_multi_inheritance();
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_EVENT_ENABLE == 1U)
    test_event_queue_fanin();
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEMAPHORE_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
    (OS_CONFIG_EVENT_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
    test_stress_soak();
#endif
    test_stress_task_churn();
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    test_stress_timer_churn();
#endif

    /* Extended per-subsystem stress: each drives one subsystem at high volume with exact
     * accounting (see the OS_TEST_STRESS_EXTENDED section header). */
#if (OS_TEST_STRESS_EXTENDED == 1U)
#if (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_ALLOC_ENABLE == 1U)
    test_stress_queue_dynamic_churn();
    test_stress_queue_dynamic_concurrent();
#endif
#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    test_stress_heap_fragmentation();
#endif
#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)
    test_stress_semaphore_pingpong();
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    test_stress_notify_storm();
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
    test_stress_event_bit_storm();
#endif
#if (OS_CONFIG_WORK_ENABLE == 1U)
    test_stress_work_flood();
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    test_stress_timer_flood();
#endif
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    test_stress_mutex_convoy();
#endif
#else
    test_print_section("Extended per-subsystem stress");
    printf("  [SKIP] OS_TEST_STRESS_EXTENDED=0: needs ~15 KB of flash this unoptimized build does\r\n"
           "         not have, and stress timings at -O0 do not reflect shipped firmware.\r\n"
           "         Build Release (-Os) to run them, or define OS_TEST_STRESS_EXTENDED=1.\r\n");
#endif /* OS_TEST_STRESS_EXTENDED */

    test_task_footprint();
    test_context_switch_timing();
    test_tickless_hooks();
    test_tickless_sleep();
    test_list();
    test_priority_api();
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    test_queue_accounting();
#endif
    test_regressions();
    test_unsupported_features();

    printf("\r\n========================================\r\n");
    printf(" RESULT: %lu passed, %lu failed (of %lu checks)\r\n", (unsigned long)os_test_pass_count,
           (unsigned long)os_test_fail_count, (unsigned long)(os_test_pass_count + os_test_fail_count));
    printf("%s\r\n", (os_test_fail_count == 0U) ? " ALL RTOS FEATURES VERIFIED OK" : " SOME CHECKS FAILED - see log above");
    printf("========================================\r\n");

    /* Last, so the timings are the final thing on the console and are not interleaved with
     * PASS/FAIL lines: benchmarks report numbers, they do not pass or fail. */
    test_benchmarks();
    printf("\r\n");
}
