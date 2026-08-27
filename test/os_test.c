/**
 * @file os_test.c
 * @brief Boot-time self-test suite for the Ahura RTOS kernel.
 *
 * Supplies os_test() (declared in ahura.h, defined nowhere else in the kernel; not named with the
 * "_cb" suffix - this is where the suite's own code runs, not a kernel query for platform
 * behavior): link this file's library (AhuraRTOS/test, target "os_test") and, when
 * OS_CONFIG_TEST_ENABLE is 1, os_init() creates a task that calls this automatically - no explicit
 * call needed from the application. Runs once, exercises whichever OS_CONFIG_<FEATURE>_ENABLE
 * switches are on, and
 * prints a detailed PASS/FAIL log via printf, finishing with a pass/fail summary. Depends on
 * nothing but ahura.h - no board/HAL headers - so it runs on any arch/board the kernel supports;
 * printf's destination (typically a UART) is the linking application's concern.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
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

/* OS_TASK_CONFIG takes a core affinity only when OS_CONFIG_CORE_COUNT is above 1 - the kernel
 * makes that a compile error at every creation site on purpose, so raising the core count is a
 * real port rather than a config edit. This suite has 53 such sites and none of them cares
 * where it runs, so it says so once here and stays buildable at either core count.
 *
 * A test that DOES care about placement uses OS_TASK_CONFIG directly, inside a
 * #if (OS_CONFIG_CORE_COUNT > 1U) block - see test_multicore(). */
#if (OS_CONFIG_CORE_COUNT == 1U)
#define TEST_TASK_CONFIG(entry, context, priority) \
    OS_TASK_CONFIG((entry), (context), (priority))
#else
/* PINNED TO CORE 0, not OS_TASK_CORE_ANY, and for now that is an experiment rather than a
 * decision. With ANY, this suite hangs at a different point in Task Lifecycle on almost every run
 * - sometimes a fault, sometimes a task that never runs, sometimes a dead stop with no output.
 * Non-determinism at that scale is a race, and the only thing ANY adds is migration between cores.
 *
 * Pinning removes exactly that variable while leaving core 1 busy with its own pinned worker, so
 * the two cores still contend for the kernel's locks. If the suite then runs clean, the fault is
 * in migration specifically; if it still breaks, migration is innocent and the contention itself
 * is at fault. Either answer is worth more than another run of the same test. */
#define TEST_TASK_CONFIG(entry, context, priority) \
    OS_TASK_CONFIG((entry), (context), (priority), OS_TASK_CORE(0))
#endif

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
#if (OS_CONFIG_SEM_ENABLE == 1U)
static os_sem_t os_test_sched_lock_sem;  /* left empty: a take would have to block */
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
/* How many timers the suite exercises at once. The kernel caps nothing - timers are linked
 * into a list rather than taking a slot - so this is the suite's own choice, not a limit. */
#define TEST_TIMER_SET             4U

#define TEST_BENCH_SAMPLES         2000U
#define TEST_BENCH_HEAVY_SAMPLES   200U

/* Saturating subtract: an operation cheaper than the measurement overhead itself would
 * otherwise wrap to a huge unsigned value. */
#define TEST_BENCH_SUB(total, over) (((total) > (over)) ? ((total) - (over)) : 0U)

/* Sample one operation TEST_BENCH_SAMPLES times, keeping the cheapest AND the dearest run. The
 * statement is pasted inline (not called through a function pointer) so no call overhead is
 * attributed to it; the compiler cannot reorder it across the two counter reads, which are
 * asm volatile with a memory clobber.
 *
 * Two numbers because they answer different questions. BEST is what the operation costs - every
 * source of interference only ever adds cycles, so the minimum converges on the uninterrupted
 * path. WORST is what a caller can actually be made to wait, which is a different thing and
 * usually a more useful one: it is the same code plus whatever landed on top of it during these
 * samples, and at a 1 kHz tick that is nearly always the tick ISR. */
#define TEST_BENCH_CYCLES(best_out, worst_out, samples, op_stmt)                 \
    do {                                                                         \
        uint32_t bench_best  = UINT32_MAX;                                       \
        uint32_t bench_worst = 0U;                                               \
        uint32_t bench_i;                                                        \
        for (bench_i = 0U; bench_i < (samples); bench_i++)                       \
        {                                                                        \
            uint32_t bench_c0 = os_arch_cycle_count_get();                       \
            op_stmt;                                                             \
            uint32_t bench_d = os_arch_cycle_count_get() - bench_c0;             \
            if (bench_d < bench_best)  { bench_best  = bench_d; }                \
            if (bench_d > bench_worst) { bench_worst = bench_d; }                \
        }                                                                        \
        (best_out)  = bench_best;                                                \
        (worst_out) = bench_worst;                                               \
    } while (0)

/* Dedicated benchmark objects, kept separate from the functional tests' shared ones so a
 * leftover count/item/waiter from an earlier section cannot skew a measurement. */
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_mutex_t     os_test_bench_mutex;
#endif
#if (OS_CONFIG_SEM_ENABLE == 1U)
static os_sem_t os_test_bench_sem;
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
OS_QUEUE_DEFINE_STATIC_ATTR(os_test_bench_queue, uint32_t, 4, );
#endif
#if (OS_CONFIG_MSG_ENABLE == 1U)
/* Room for one message of the longest size benchmarked below, header included - only one is
 * ever in flight, since each sample sends and then receives. */
OS_MSG_DEFINE_STATIC_ATTR(os_test_bench_msg, OS_MSG_SPACE(64U), );
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
static os_event_t os_test_bench_event;
#endif
#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
/* File scope, not a local: the port's operations want a naturally aligned 32-bit word, which a
 * static of this type is by definition. */
static os_atomic_t os_test_bench_atomic = OS_ATOMIC_INIT(0);
#endif
/* Filler for the scheduler row: tasks that are READY and never get the CPU, so the ready lists are
 * loaded while the measurement itself is unchanged. Four is enough to tell a bitmap pick from a
 * walk - a walk would already be four times the work - and leaves the table's own tasks room
 * inside OS_CONFIG_MAX_USER_TASKS. Not under any feature guard: the scheduler is PART 1, so this
 * row runs in every configuration. */
#define TEST_BENCH_TASK_FILL   4U

OS_TASK_DEFINE(os_test_bench_task_fill0, OS_CONFIG_MIN_STACK_SIZE);
OS_TASK_DEFINE(os_test_bench_task_fill1, OS_CONFIG_MIN_STACK_SIZE);
OS_TASK_DEFINE(os_test_bench_task_fill2, OS_CONFIG_MIN_STACK_SIZE);
OS_TASK_DEFINE(os_test_bench_task_fill3, OS_CONFIG_MIN_STACK_SIZE);

static os_task_t *os_test_bench_task_fill[TEST_BENCH_TASK_FILL] = {
    &os_test_bench_task_fill0, &os_test_bench_task_fill1,
    &os_test_bench_task_fill2, &os_test_bench_task_fill3
};

#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void test_bench_timer_cb(void *context, uint32_t value);

/* A minute long, so it can be armed and cancelled 2000 times over without ever expiring: the
 * measurement sees the arm/cancel path alone, never a delivery. */
OS_TIMER_DEFINE_ONESHOT(os_test_bench_timer, 60000U, test_bench_timer_cb);

/* Filler for the second timer row: start and stop search the running list, so their cost depends
 * on how many timers are RUNNING. These sit in that list doing nothing, to make the slope visible
 * rather than leaving it to be reasoned about. */
#define TEST_BENCH_TIMER_FILL  8U

OS_TIMER_DEFINE_ONESHOT(os_test_bf0, 60000U, test_bench_timer_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_bf1, 60000U, test_bench_timer_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_bf2, 60000U, test_bench_timer_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_bf3, 60000U, test_bench_timer_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_bf4, 60000U, test_bench_timer_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_bf5, 60000U, test_bench_timer_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_bf6, 60000U, test_bench_timer_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_bf7, 60000U, test_bench_timer_cb);

static os_timer_t *os_test_bench_fill[TEST_BENCH_TIMER_FILL] = {
    &os_test_bf0, &os_test_bf1, &os_test_bf2, &os_test_bf3,
    &os_test_bf4, &os_test_bf5, &os_test_bf6, &os_test_bf7,
};
#endif

/* Shared between two equal-priority tasks in test_context_switch_timing(): each increments
 * this once per loop turn, then yields - so its total over a fixed window is (approximately)
 * the number of context switches that occurred. */
static __IO uint32_t os_test_switch_count      = 0U;
static __IO bool     os_test_switch_should_run = true;

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_mutex_t os_test_mutex;
#endif

/* test_spawn_helper has to exist whenever a section that CALLS it is compiled in, and no more.
 * Guarding it on a single feature left the others calling an undeclared function; listing a
 * feature that does not actually call it leaves the helper defined and unused, which is what
 * -Wunused-function reports (and -Werror fails on).
 *
 * OS_CONFIG_MUTEX_ENABLE is deliberately NOT here even though the mutex section contains a call:
 * that call sits inside a nested OS_CONFIG_SEM_ENABLE guard, since handing a mutex over
 * needs a semaphore to do it with. Mutexes alone never reach the helper, so the semaphore term
 * below already covers that case. */
#define TEST_HELPER_NEEDED ((OS_CONFIG_SEM_ENABLE == 1U) || \
                            (OS_CONFIG_QUEUE_ENABLE == 1U)     || \
                            (OS_CONFIG_EVENT_ENABLE == 1U))

#if (OS_CONFIG_SEM_ENABLE == 1U)
static os_sem_t os_test_bin_sem;
static os_sem_t os_test_count_sem;
#endif

/* Every use of this one - the give in test_helper_entry and the init/take in test_mutex - sits
 * behind both switches, so defining it on the semaphore switch alone left it unused whenever
 * mutexes were compiled out. */
#if (OS_CONFIG_SEM_ENABLE == 1U) && (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_sem_t os_test_sync_sem;   /* helper -> main "ready" signal */
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
/* Defined with OS_QUEUE_DEFINE_STATIC_ATTR rather than OS_QUEUE_DEFINE_STATIC so the suite covers that
 * macro too - with an empty attribute list, which has to compile as readily as a real one. Tests
 * reset it with os_queue_cleanup(), which empties a queue without freeing storage it does not
 * own. */
OS_QUEUE_DEFINE_STATIC_ATTR(os_test_queue, uint32_t, 3, );
#endif

#if (OS_CONFIG_EVENT_ENABLE == 1U)
static os_event_t os_test_event;
#endif

#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void timer_oneshot_cb(void *context, uint32_t value);
static void timer_periodic_cb(void *context, uint32_t value);
static void test_churn_timer_cb(void *context, uint32_t value);

/* Set up where they are declared: the kernel has no init call, so period, mode and callback are
 * settled here and only the period is ever retuned (os_timer_period_set) at run time. */
OS_TIMER_DEFINE_ONESHOT(os_test_timer_oneshot, 50U, timer_oneshot_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_timer_periodic, 30U, timer_periodic_cb);

/* The churn test hammers one object with its own callback, so it gets its own object rather than
 * repointing a shared one. */
OS_TIMER_DEFINE_ONESHOT(os_test_churn_timer, 1000U, test_churn_timer_cb);
static __IO uint32_t os_test_oneshot_fired  = 0U;
static __IO uint32_t os_test_periodic_fired = 0U;
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
static __IO os_err_t os_test_notify_wait_status;
static __IO uint32_t  os_test_notify_wait_value;
static __IO uint32_t  os_test_notify_wait_ticks;
static __IO os_err_t os_test_notify_second_status;
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

#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEM_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
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

static os_sem_t    os_test_stress_sem;
static os_event_t  os_test_stress_event;
OS_QUEUE_DEFINE_STATIC_ATTR(os_test_stress_queue, uint32_t, OS_TEST_STRESS_QUEUE_CAPACITY, );
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
static os_err_t test_spawn_helper(helper_role_t role, uint32_t hold_ms, uint32_t bits, uint32_t value);
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
#if (OS_CONFIG_SEM_ENABLE == 1U)
static void test_semaphore(void);
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
static void test_queue(void);
static void test_queue_define_and_dynamic(void);
#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
static void test_atomic(void);
#endif
#endif
#if (OS_CONFIG_MSG_ENABLE == 1U)
static void test_msg(void);
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
static void test_event_group(void);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void test_timer(void);
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void test_timer_isr(void);
static void test_timer_pool(void);
static void test_timer_real_world(void);
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
static void test_bench_row(const char *name, uint32_t best, uint32_t worst, uint32_t clock_hz);
static void test_benchmarks(void);
static void test_tickless_hooks(void);
static void test_tickless_sleep(void);
static void test_list(void);
#if (OS_CONFIG_CORE_COUNT > 1U)
static void test_multicore(void);
static void test_multicore_watch(uint32_t watch_ms, const char *when);
static void test_smp_critical_nested(void);
#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
static void test_smp_atomic_contention(void);
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
static void test_smp_notify_pingpong(void);
#endif
#if (OS_CONFIG_SEM_ENABLE == 1U)
static void test_smp_semaphore_pingpong(void);
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
static void test_smp_queue_accounting(void);
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
static void test_smp_event_pingpong(void);
#endif
static void test_smp_migration(void);
static void test_smp_lock_independent(void);
static void test_smp_task_churn(void);
#if (OS_CONFIG_TIMER_ENABLE == 1U)
static void test_smp_deferred_submit(void);
#endif
#if (OS_CONFIG_SEM_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_ATOMIC_ENABLE == 1U)
static void test_smp_soak_mixed(void);
#endif
#endif
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
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEM_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
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
    const char *haystack;

    os_test_log_capture[TEST_LOG_CAPTURE_SIZE - 1U] = '\0';

    /* Searched byte by byte rather than with strstr(), which is not the micro-optimisation it
     * looks like.
     *
     * newlib's strstr() switches to a two-way algorithm for longer needles and compares with
     * memcmp(), whose RISC-V build loads a WORD at a time from pointers it assumes are aligned.
     * They are not: both sides here are arbitrary offsets into a char buffer. Cortex-M lets that
     * through because its loads tolerate misalignment; Hazard3 traps it, and the suite dies
     * partway through the log section with a load-address-misaligned exception.
     *
     * The suite is meant to depend on nothing but ahura.h and printf, so the fix is to stop
     * depending on libc's string search rather than to argue with its alignment assumptions. The
     * captured buffer is a few KB and this runs a handful of times. */
    for (haystack = (const char *)os_test_log_capture; *haystack != '\0'; haystack++)
    {
        size_t index = 0U;

        while ((needle[index] != '\0') && (haystack[index] == needle[index]))
        {
            index++;
        }

        if (needle[index] == '\0')
        {
            return true;
        }
    }

    return (*needle == '\0');
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
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEM_ENABLE == 1U)
    case HELPER_MUTEX_HOLD:
        (void)os_mutex_lock(&os_test_mutex, OS_WAIT_FOREVER);
        (void)os_sem_give(&os_test_sync_sem);
        os_delay_ms(os_test_helper_ctx.hold_ms);
        (void)os_mutex_unlock(&os_test_mutex);
        break;
#endif

#if (OS_CONFIG_SEM_ENABLE == 1U)
    case HELPER_SEM_GIVE_AFTER:
        os_delay_ms(os_test_helper_ctx.hold_ms);
        (void)os_sem_give(&os_test_count_sem);
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
static os_err_t test_spawn_helper(helper_role_t role, uint32_t hold_ms, uint32_t bits, uint32_t value)
{
    os_err_t status;

    os_test_helper_ctx.role    = role;
    os_test_helper_ctx.hold_ms = hold_ms;
    os_test_helper_ctx.bits    = bits;
    os_test_helper_ctx.value   = value;

    status = os_task_create(&helper, TEST_TASK_CONFIG(test_helper_entry, NULL, 3U));
    if (status != OS_ERR_NONE)
    {
        return status;
    }

    return os_task_start(&helper);
}
#endif /* OS_CONFIG_SEM_ENABLE */

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
    os_delay_ms(1000U);
    t1    = os_tick_get();
    delta = t1 - t0;
    AHURA_TEST_CHECK((delta >= 1000U) && (delta <= 1060U), "os_delay_ms(1000) elapsed %lu ticks (expected ~1000)",
                      (unsigned long)delta);
}

/******************************************************************************************************/
static void test_critical_section(void)
{
    test_print_section("Critical Sections");

    /* os_arch_kernel_mask_active reads PRIMASK or BASEPRI depending on
     * OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY, so the checks hold in both
     * kernel mask modes. */
    /* Every reading is taken first and printed afterwards. Printing from INSIDE the section is
     * what this test used to do, and on a multi-core build it is not merely slow - it is a
     * correctness bug in the test itself:
     *
     * os_critical_enter() takes the cross-core kernel spinlock with interrupts masked. printf on
     * this board goes to USB CDC, which waits on the host and can take a very long time. While it
     * does, the OTHER core's SysTick lands, calls os_task_reschedule_possible(), and spins on that
     * same spinlock - inside an interrupt handler, with its own interrupts masked. Its ticks
     * coalesce and it stops scheduling entirely.
     *
     * The rule this restores is the one every kernel states about critical sections: keep them
     * short and never block inside one. A test that breaks that rule is not testing the kernel,
     * it is testing what the kernel does when misused. */
    bool before  = (os_arch_kernel_mask_active() == 0U);
    bool outer;
    bool nested;
    bool inner_exit;
    bool after;

    os_critical_enter();
    outer = (os_arch_kernel_mask_active() != 0U);

    os_critical_enter(); /* nested */
    nested = (os_arch_kernel_mask_active() != 0U);

    os_critical_exit(); /* inner exit: outer level still held */
    inner_exit = (os_arch_kernel_mask_active() != 0U);

    os_critical_exit(); /* outer exit */
    after = (os_arch_kernel_mask_active() == 0U);

    AHURA_TEST_CHECK(before, "the kernel mask is lowered before entering a critical section");
    AHURA_TEST_CHECK(outer, "os_critical_enter() raises the kernel mask");
    AHURA_TEST_CHECK(nested, "a nested os_critical_enter() keeps the kernel mask raised");
    AHURA_TEST_CHECK(inner_exit, "exiting the inner level keeps the kernel mask raised (nesting works)");
    AHURA_TEST_CHECK(after, "the matching outer os_critical_exit() lowers the kernel mask");
}

/******************************************************************************************************/
static void test_task_lifecycle(void)
{
    os_task_config_t cfg;
    os_err_t        status;
    uint32_t         snapshot;

    test_print_section("Task Lifecycle");

    /* --- Reject invalid creation parameters (should not touch any handle). --- */
    cfg = *TEST_TASK_CONFIG(test_worker_entry, NULL, 1U);

    cfg.priority = 0U;
    AHURA_TEST_CHECK(os_task_create(&helper, &cfg) == OS_ERR_INVALID_ARG,
                      "os_task_create() rejects priority 0 (idle-reserved)");

    cfg.priority = OS_TASK_PRIO_MAX;
    AHURA_TEST_CHECK(os_task_create(&helper, &cfg) == OS_ERR_INVALID_ARG,
                      "os_task_create() rejects priority %u (kernel-reserved)", (unsigned)OS_TASK_PRIO_MAX);

    cfg.priority = OS_TASK_PRIO_1_LOWEST;

    /* The stack travels with the handle now, not the config, so an unusable stack is an unusable
     * handle. These descriptors stand in for an OS_TASK_DEFINE that somehow got it wrong - which
     * the macro itself cannot, since it derives both fields from the array it just declared. */
    {
        /* Designated, not positional: OS_CONFIG_TASK_NAME_ENABLE at 0 removes the name field
         * from the descriptor, and a positional list would then feed the stack pointer to
         * stack_bytes. OS_TASK_NAME_INIT is what the DEFINE macros use for the same reason. */
        static const os_task_storage_t storage_too_small = {
            OS_TASK_NAME_INIT(too_small)
            .stack_memory = helper_stack_buf,
            .stack_bytes  = OS_CONFIG_MIN_STACK_SIZE - 8U
        };
        static const os_task_storage_t storage_misaligned = {
            OS_TASK_NAME_INIT(misaligned)
            .stack_memory = &helper_stack_buf[1],
            .stack_bytes  = sizeof(helper_stack_buf) - 8U
        };

        os_task_t bad = { 0 };

        bad.storage = &storage_too_small;
        AHURA_TEST_CHECK(os_task_create(&bad, &cfg) == OS_ERR_INVALID_ARG,
                          "os_task_create() rejects a stack smaller than OS_CONFIG_MIN_STACK_SIZE");

        bad.storage = &storage_misaligned;
        AHURA_TEST_CHECK(os_task_create(&bad, &cfg) == OS_ERR_INVALID_ARG,
                          "os_task_create() rejects a misaligned stack pointer");

        bad.storage = NULL;
        AHURA_TEST_CHECK(os_task_create(&bad, &cfg) == OS_ERR_INVALID_ARG,
                          "os_task_create() rejects a handle OS_TASK_DEFINE never set up");
    }

    /* --- Real worker: create / start / observe / pause / resume / delete. --- */
    os_test_worker_counter    = 0U;
    os_test_worker_should_run = true;

    status = os_task_create(&worker, TEST_TASK_CONFIG(test_worker_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "os_task_create() creates the worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED,
                      "a created-but-not-started task reports SUSPENDED");

    /* --- Task names: what OS_TASK_DEFINE spelled, or nothing at all. --- */
    {
        const char *name = os_task_name_get(&worker);

#if (OS_CONFIG_TASK_NAME_ENABLE == 1U)
        AHURA_TEST_CHECK((name != NULL) && (strcmp(name, "worker") == 0),
                          "os_task_name_get() reports the name OS_TASK_DEFINE gave the handle (%s)",
                          (name != NULL) ? name : "NULL");
        AHURA_TEST_CHECK(os_task_name_get(NULL) != NULL,
                          "os_task_name_get(NULL) names the calling task");
#else
        AHURA_TEST_CHECK(name == NULL,
                          "os_task_name_get() answers NULL with OS_CONFIG_TASK_NAME_ENABLE at 0");
        AHURA_TEST_CHECK(os_task_name_get(NULL) == NULL,
                          "and NULL for the calling task in that build too");
#endif

        /* Independent of the option: an unresolvable handle has no name either way, which is
         * what lets a caller treat NULL as one case rather than two. */
        {
            os_task_t unknown = { 0 };

            AHURA_TEST_CHECK(os_task_name_get(&unknown) == NULL,
                              "os_task_name_get() answers NULL for a handle no task owns");
        }
    }

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "os_task_start() starts the worker task");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter > 0U, "worker task actually executed (counter=%lu)",
                      (unsigned long)os_test_worker_counter);
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_READY,
                      "a lower-priority runnable task reports READY while this task runs");

    AHURA_TEST_CHECK(os_task_pause(&worker) == OS_ERR_NONE, "os_task_pause() suspends the worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED, "paused task reports SUSPENDED");
    snapshot = os_test_worker_counter;
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter == snapshot, "counter is frozen while the worker is paused");

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "os_task_start() resumes a paused task");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter > snapshot, "counter resumes advancing after os_task_start()");

    AHURA_TEST_CHECK(os_task_delete(&worker) == OS_ERR_NONE, "os_task_delete() deletes the live worker task");
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_INACTIVE,
                      "a deleted task's handle reports INACTIVE");
    snapshot = os_test_worker_counter;
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_worker_counter == snapshot, "counter is frozen after deletion (worker truly stopped)");

    /* --- NULL means "current task": the worker pauses itself; we resume it. --- */
    os_test_worker_counter = 0U;
    status = os_task_create(&worker, TEST_TASK_CONFIG(test_self_pause_worker_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "worker task re-created for the self-pause test");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "os_task_start() starts it");

    os_delay_ms(40U); /* let it reach os_task_pause(NULL) */
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_SUSPENDED,
                      "os_task_pause(NULL) suspends the calling task itself");

    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE,
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
    AHURA_TEST_CHECK(os_task_create(&worker, TEST_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_ERR_NONE,
                      "identity task A created");
    AHURA_TEST_CHECK(os_task_create(&helper, TEST_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_ERR_NONE,
                      "identity task B created");
    AHURA_TEST_CHECK(os_task_create(&helper2, TEST_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_ERR_NONE,
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

    AHURA_TEST_CHECK(os_task_delete(&helper) == OS_ERR_NONE, "identity task B deleted, freeing its slot");
    AHURA_TEST_CHECK(os_task_state_get(&stale_handle) == OS_TASK_STATE_INACTIVE,
                      "a stale handle to the deleted task reports INACTIVE");

    /* The next task very likely lands in B's freed slot - but must not inherit B's id. */
    AHURA_TEST_CHECK(os_task_create(&helper3, TEST_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_ERR_NONE,
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
    os_err_t status;
    os_err_t start_status;

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
    status = os_task_create(&worker, TEST_TASK_CONFIG(test_busy_spin_entry, NULL, TEST_PRIO_LOW));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "low-priority spinner task created (priority %u)",
                      (unsigned)TEST_PRIO_LOW);
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "low-priority spinner started");

    os_delay_ms(20U);
    snapshot_before = os_test_busy_counter;
    AHURA_TEST_CHECK(snapshot_before > 0U,
                      "the low-priority spinner gets CPU time when nothing outranks it (count=%lu)",
                      (unsigned long)snapshot_before);

    /* A task at a strictly higher priority than both the spinner and this test task never
     * yields/delays for its whole burst - so the spinner cannot possibly run until it is gone. */
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_burst_spin_entry, NULL,
                                                            TEST_PRIO_HIGH));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "higher-priority burst task created (priority %u)",
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

    AHURA_TEST_CHECK(start_status == OS_ERR_NONE, "higher-priority burst task started");
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
    os_err_t status;
    bool      locked_flag;
    bool      ran_while_locked;
#if (OS_CONFIG_SEM_ENABLE == 1U)
    os_err_t take_status;
#endif

    test_print_section("Scheduler Lock");

    AHURA_TEST_CHECK(!os_kernel_is_locked(), "the scheduler starts out unlocked");

    os_test_sched_lock_ran = false;

#if (OS_CONFIG_SEM_ENABLE == 1U)
    AHURA_TEST_CHECK(os_sem_init(&os_test_sched_lock_sem, 0U, 1U) == OS_ERR_NONE,
                      "empty semaphore initialized (a take on it can only block)");
#endif

    status = os_task_create(&helper, TEST_TASK_CONFIG(test_sched_lock_entry, NULL, TEST_PRIO_HIGH));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "higher-priority task created (priority %u)",
                      (unsigned)TEST_PRIO_HIGH);

    os_kernel_lock();

    locked_flag = os_kernel_is_locked();
    (void)os_task_start(&helper);
    os_delay_us(200U);
    ran_while_locked = os_test_sched_lock_ran;
#if (OS_CONFIG_SEM_ENABLE == 1U)
    take_status = os_sem_take(&os_test_sched_lock_sem, 10U);
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

#if (OS_CONFIG_SEM_ENABLE == 1U)
    /* Blocking under the lock would park a task the lock then refuses to switch away from, so
     * every blocking primitive degrades to its OS_WAIT_NOTHING behaviour instead - EMPTY here,
     * NOT the TIMEOUT a real 10 ms wait would have reported. */
    AHURA_TEST_CHECK(take_status == OS_ERR_EMPTY,
                      "os_sem_take(10 ms) is non-blocking under the lock (status=%d)",
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

    AHURA_TEST_CHECK(status == OS_ERR_BUSY, "os_task_pause(self) is refused under the lock (status=%d)",
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
#if (OS_CONFIG_SEM_ENABLE == 1U)
    /* Only the contention part below uses these, and it needs a semaphore to know when the helper
     * has actually taken the mutex. Declaring them unconditionally left them unused whenever
     * semaphores were compiled out. */
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;
    os_err_t status;
#endif

    test_print_section("Mutex");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_mutex) == OS_ERR_NONE, "os_mutex_init() succeeds");
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_mutex) == OS_ERR_ERROR, "unlocking a free mutex returns ERROR");

    AHURA_TEST_CHECK(os_mutex_lock(&os_test_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "os_mutex_lock() acquires a free mutex");
    AHURA_TEST_CHECK(os_mutex_lock(&os_test_mutex, OS_WAIT_NOTHING) == OS_ERR_BUSY,
                      "re-locking from the owner fails BUSY (not recursive)");
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_mutex) == OS_ERR_NONE, "owner os_mutex_unlock() releases the mutex");

#if (OS_CONFIG_SEM_ENABLE == 1U)
    /* Contention: a helper task holds the mutex for 150 ms. */
    (void)os_sem_init(&os_test_sync_sem, 0U, 1U);
    AHURA_TEST_CHECK(test_spawn_helper(HELPER_MUTEX_HOLD, 150U, 0U, 0U) == OS_ERR_NONE,
                      "helper task spawned to hold the mutex");
    AHURA_TEST_CHECK(os_sem_take(&os_test_sync_sem, 200U) == OS_ERR_NONE, "helper signals once it holds the mutex");

    AHURA_TEST_CHECK(os_mutex_lock(&os_test_mutex, OS_WAIT_NOTHING) == OS_ERR_BUSY,
                      "os_mutex_lock(OS_WAIT_NOTHING) fails while another task holds the mutex");
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_mutex) == OS_ERR_NOT_OWNER,
                      "unlocking a mutex owned by another task returns NOT_OWNER");

    t0     = os_tick_get();
    status = os_mutex_lock(&os_test_mutex, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "blocking os_mutex_lock() succeeds once the holder releases it");
    AHURA_TEST_CHECK((delta >= 100U) && (delta <= 250U), "blocking lock woke ~when the holder unlocked (%lu ticks)",
                      (unsigned long)delta);

    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_mutex) == OS_ERR_NONE, "final os_mutex_unlock() releases the mutex");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "mutex-holder helper task terminated cleanly");
#endif
}
#endif /* OS_CONFIG_MUTEX_ENABLE */

/*
 * ***********************************************************************************************************
 * Semaphore
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_SEM_ENABLE == 1U)
/******************************************************************************************************/
static void test_semaphore(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;
    os_err_t status;

    test_print_section("Semaphore");

    AHURA_TEST_CHECK(os_sem_init(&os_test_bin_sem, 0U, 1U) == OS_ERR_NONE,
                      "os_sem_init() creates a binary semaphore (0/1)");
    AHURA_TEST_CHECK(os_sem_take(&os_test_bin_sem, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                      "take on an empty semaphore with OS_WAIT_NOTHING returns EMPTY");
    AHURA_TEST_CHECK(os_sem_give(&os_test_bin_sem) == OS_ERR_NONE, "os_sem_give() adds a token");
    AHURA_TEST_CHECK(os_sem_give(&os_test_bin_sem) == OS_ERR_FULL, "giving beyond max_count returns FULL");
    AHURA_TEST_CHECK(os_sem_take(&os_test_bin_sem, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "take succeeds once a token is available");

    t0     = os_tick_get();
    status = os_sem_take(&os_test_bin_sem, 100U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_ERR_TIMEOUT, "take on an empty semaphore times out");
    AHURA_TEST_CHECK((delta >= 95U) && (delta <= 150U), "timeout elapsed ~100 ticks (%lu)", (unsigned long)delta);

    AHURA_TEST_CHECK(os_sem_init(&os_test_count_sem, 0U, 3U) == OS_ERR_NONE,
                      "os_sem_init() creates a counting semaphore (0/3)");
    AHURA_TEST_CHECK(test_spawn_helper(HELPER_SEM_GIVE_AFTER, 80U, 0U, 0U) == OS_ERR_NONE,
                      "helper spawned to give the counting semaphore after 80 ms");

    t0     = os_tick_get();
    status = os_sem_take(&os_test_count_sem, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "blocking take succeeds once the helper gives");
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "take woke ~when the helper gave (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "semaphore-giver helper task terminated cleanly");
}
#endif /* OS_CONFIG_SEM_ENABLE */

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
    if ((os_task_create(&worker, TEST_TASK_CONFIG(test_atomic_hammer_entry, NULL,
                                                        TEST_PRIO_HIGH)) == OS_ERR_NONE) &&
        (os_task_create(&helper, TEST_TASK_CONFIG(test_atomic_hammer_entry, NULL,
                                                        TEST_PRIO_HIGH)) == OS_ERR_NONE))
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
    os_err_t         status;

    test_print_section("Queue Definition (static macro and dynamic allocation)");

    /* --- OS_QUEUE_DEFINE_STATIC --- */

    AHURA_TEST_CHECK(sizeof(os_test_defined_queue_queue_buf) == (4U * sizeof(test_queue_item_t)),
                      "OS_QUEUE_DEFINE_STATIC() sized the buffer for 4 items of the declared type (%u bytes)",
                      (unsigned)sizeof(os_test_defined_queue_queue_buf));

    /* Nothing has been called on this queue: every field below was written by the macro at compile
     * time. The geometry has to match the declaration, since getting either wrong is exactly the
     * out-of-bounds bug that deriving it from the declaration exists to make impossible. */
    AHURA_TEST_CHECK(os_test_defined_queue.item_size == sizeof(test_queue_item_t),
                      "the item size comes from the declared type with no init call (%u bytes)",
                      (unsigned)os_test_defined_queue.item_size);
    AHURA_TEST_CHECK(os_test_defined_queue.capacity == 4U,
                      "the capacity comes from the declared count (%u)",
                      (unsigned)os_test_defined_queue.capacity);
    AHURA_TEST_CHECK(os_test_defined_queue.buffer == (uint8_t *)os_test_defined_queue_queue_buf,
                      "the queue points at the buffer the macro declared");
    AHURA_TEST_CHECK((os_test_defined_queue.count == 0U) && (os_test_defined_queue.head == 0U) &&
                      (os_test_defined_queue.tail == 0U) && !os_test_defined_queue.buffer_owned &&
                      (os_test_defined_queue.send_waiters.head == NULL) &&
                      (os_test_defined_queue.receive_waiters.head == NULL),
                      "and starts empty with empty waiter lists, owning nothing");

    sent.id         = 0xA5A5A5A5UL;
    sent.payload[0] = 0x11U;
    sent.payload[5] = 0x99U;

    AHURA_TEST_CHECK(os_queue_send(&os_test_defined_queue, &sent, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "a struct item goes into the statically defined queue, still with no init call");
    AHURA_TEST_CHECK(os_queue_receive(&os_test_defined_queue, &got, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "and comes back out");
    AHURA_TEST_CHECK((got.id == sent.id) && (got.payload[0] == 0x11U) && (got.payload[5] == 0x99U),
                      "the whole struct survived the round trip intact");

    /* --- os_queue_init_dynamic / os_queue_cleanup --- */

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    heap_before = os_mem_free_get();

    status = os_queue_init_dynamic(&dynamic, sizeof(uint32_t), 8U);
    AHURA_TEST_CHECK(status == OS_ERR_NONE,
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
    AHURA_TEST_CHECK(os_queue_send(&dynamic, &value, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "the dynamic queue accepts an item");
    value = 0U;
    AHURA_TEST_CHECK(os_queue_receive(&dynamic, &value, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "the dynamic queue returns it");
    AHURA_TEST_CHECK(value == 0xDEADBEEFUL, "with the value intact (0x%08lX)", (unsigned long)value);

    AHURA_TEST_CHECK(os_queue_cleanup(&dynamic) == OS_ERR_NONE, "os_queue_cleanup() tears it down");
    AHURA_TEST_CHECK(os_mem_free_get() == heap_before,
                      "and returned every byte it took to the heap (%u bytes free)",
                      (unsigned)os_mem_free_get());
    AHURA_TEST_CHECK(dynamic.buffer == NULL, "the torn-down queue no longer points at freed memory");

    /* A zero or overflowing geometry must be refused rather than wrapped into a small allocation
     * that every later send would index past. */
    AHURA_TEST_CHECK(os_queue_init_dynamic(&dynamic, 0U, 4U) == OS_ERR_INVALID_ARG,
                      "os_queue_init_dynamic() rejects a zero item size");
    AHURA_TEST_CHECK(os_queue_init_dynamic(&dynamic, 4U, 0U) == OS_ERR_INVALID_ARG,
                      "os_queue_init_dynamic() rejects a zero capacity");
    AHURA_TEST_CHECK(os_queue_init_dynamic(&dynamic, SIZE_MAX / 2U, 4U) == OS_ERR_INVALID_ARG,
                      "os_queue_init_dynamic() rejects a geometry whose byte count would overflow");

    /* A geometry that is valid but larger than the whole heap has to come back as NO_MEMORY, so a
     * caller can tell "ask for less" apart from "that request was nonsense". */
    AHURA_TEST_CHECK(os_queue_init_dynamic(&dynamic, 1U, OS_CONFIG_HEAP_SIZE * 2U) == OS_ERR_NO_MEMORY,
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

    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_defined_queue) == OS_ERR_NONE,
                      "os_queue_cleanup() also accepts a statically defined queue");
    AHURA_TEST_CHECK(os_queue_count_get(&os_test_defined_queue) == 0U, "and empties it");
    AHURA_TEST_CHECK((os_test_defined_queue.buffer == (uint8_t *)os_test_defined_queue_queue_buf) &&
                      (os_test_defined_queue.item_size == sizeof(test_queue_item_t)) &&
                      (os_test_defined_queue.capacity == 4U),
                      "but keeps the storage it does not own, geometry intact");

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    AHURA_TEST_CHECK(os_mem_free_get() == heap_before,
                      "and did not free the static buffer into the kernel heap");
#endif

    /* Still usable with nothing called in between, which is the point of keeping the storage. */
    AHURA_TEST_CHECK(os_queue_send(&os_test_defined_queue, &sent, OS_WAIT_NOTHING) == OS_ERR_NONE,
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
    os_err_t status;
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;

    test_print_section("Queue");

    /* No init call: OS_QUEUE_DEFINE_STATIC_ATTR initialized os_test_queue over its own array at compile time.
     * The geometry below is what the macro derived from the array, never a number passed by hand. */
    AHURA_TEST_CHECK((os_test_queue.buffer == (uint8_t *)os_test_queue_queue_buf) &&
                      (os_test_queue.item_size == sizeof(os_test_queue_queue_buf[0])) &&
                      (os_test_queue.capacity == (sizeof(os_test_queue_queue_buf) / sizeof(os_test_queue_queue_buf[0]))),
                      "OS_QUEUE_DEFINE_STATIC_ATTR() bound the queue to its own array, geometry derived");
    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 0U, "a fresh queue reports 0 items");
    AHURA_TEST_CHECK(os_queue_free_get(&os_test_queue) == os_test_queue.capacity,
                      "a fresh queue reports its whole capacity free (%lu)",
                      (unsigned long)os_queue_free_get(&os_test_queue));
    AHURA_TEST_CHECK(os_queue_receive(&os_test_queue, &value, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                      "receive on an empty queue with OS_WAIT_NOTHING returns EMPTY");

    for (i = 0U; i < 3U; i++)
    {
        AHURA_TEST_CHECK(os_queue_send(&os_test_queue, &i, OS_WAIT_NOTHING) == OS_ERR_NONE,
                          "send #%lu succeeds while the queue has room", (unsigned long)i);
    }
    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 3U, "queue count reports 3/3 full");
    AHURA_TEST_CHECK(os_queue_free_get(&os_test_queue) == 0U, "a full queue reports 0 free slots");

    value = 99U;
    AHURA_TEST_CHECK(os_queue_send(&os_test_queue, &value, OS_WAIT_NOTHING) == OS_ERR_FULL,
                      "send on a full queue with OS_WAIT_NOTHING returns FULL");

    for (i = 0U; i < 3U; i++)
    {
        status = os_queue_receive(&os_test_queue, &items[i], OS_WAIT_NOTHING);
        if ((status != OS_ERR_NONE) || (items[i] != i))
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
    AHURA_TEST_CHECK(status == OS_ERR_TIMEOUT, "receive on an empty queue times out");
    AHURA_TEST_CHECK((delta >= 95U) && (delta <= 150U), "timeout elapsed ~100 ticks (%lu)", (unsigned long)delta);

    AHURA_TEST_CHECK(test_spawn_helper(HELPER_QUEUE_SEND_AFTER, 80U, 0U, 42U) == OS_ERR_NONE,
                      "helper spawned to send item 42 after 80 ms");
    t0     = os_tick_get();
    status = os_queue_receive(&os_test_queue, &value, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK((status == OS_ERR_NONE) && (value == 42U),
                      "blocking receive gets the helper's item (value=%lu)", (unsigned long)value);
    AHURA_TEST_CHECK((delta >= 70U) && (delta <= 200U), "receive woke ~when the helper sent (%lu ticks)",
                      (unsigned long)delta);
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "queue-sender helper task terminated cleanly");
}
#endif /* OS_CONFIG_QUEUE_ENABLE */

/*
 * ***********************************************************************************************************
 * Message buffer
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MSG_ENABLE == 1U)

/* Deliberately not a round number of anything: 3 * OS_MSG_SPACE(16) is 54, so a run of 16-byte
 * messages leaves head and tail at a different offset on every lap and the ring is forced to wrap
 * mid-message rather than only at a tidy boundary. That wrap is the one thing a byte ring can get
 * wrong that a slot queue cannot. */
OS_MSG_DEFINE_STATIC(os_test_msg, 3U * OS_MSG_SPACE(16U));

/******************************************************************************************************/
/**
 * @brief Variable-length messages: whole-message delivery, byte accounting, the wrap, and the
 *        three refusals that keep a bad call from becoming a hang.
 */
static void test_msg(void)
{
    static const uint8_t pattern[24] =
    {
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU,
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U
    };

    uint8_t  rx[64];
    size_t   rx_len   = 0U;
    size_t   capacity = os_test_msg.capacity;
    bool     ok       = true;
    uint32_t lap;
    os_err_t status;
    uint32_t t0;
    uint32_t t1;
    uint32_t delta;

    test_print_section("Message Buffer (variable length)");

    /* No init call: OS_MSG_DEFINE_STATIC initialized the object over its own array at compile time. */
    AHURA_TEST_CHECK(capacity == (3U * OS_MSG_SPACE(16U)),
                      "OS_MSG_DEFINE_STATIC() sized the buffer in bytes (%lu)", (unsigned long)capacity);
    AHURA_TEST_CHECK(os_msg_count_get(&os_test_msg) == 0U, "a fresh buffer holds 0 messages");
    AHURA_TEST_CHECK(os_msg_free_get(&os_test_msg) == capacity, "a fresh buffer reports every byte free");
    AHURA_TEST_CHECK(os_msg_peek_size(&os_test_msg) == 0U, "peek on an empty buffer reports 0");
    AHURA_TEST_CHECK(os_msg_receive(&os_test_msg, rx, sizeof(rx), &rx_len, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                      "receive on an empty buffer with OS_WAIT_NOTHING returns EMPTY");

    /* --- Different lengths in, same lengths and bytes out, in order --------------------------- */

    AHURA_TEST_CHECK(os_msg_send(&os_test_msg, pattern, 1U, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "a 1-byte message is sent");
    AHURA_TEST_CHECK(os_msg_send(&os_test_msg, pattern, 16U, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "a 16-byte message is sent into the same buffer");
    AHURA_TEST_CHECK(os_msg_count_get(&os_test_msg) == 2U, "both are counted as whole messages");
    AHURA_TEST_CHECK(os_msg_free_get(&os_test_msg) == (capacity - OS_MSG_SPACE(1U) - OS_MSG_SPACE(16U)),
                      "free bytes account for each message plus its own header (%lu)",
                      (unsigned long)os_msg_free_get(&os_test_msg));

    AHURA_TEST_CHECK(os_msg_peek_size(&os_test_msg) == 1U,
                      "peek reports the length of the OLDEST message without consuming it");
    AHURA_TEST_CHECK(os_msg_count_get(&os_test_msg) == 2U, "and the peek really consumed nothing");

    (void)memset(rx, 0, sizeof(rx));
    status = os_msg_receive(&os_test_msg, rx, sizeof(rx), &rx_len, OS_WAIT_NOTHING);
    AHURA_TEST_CHECK((status == OS_ERR_NONE) && (rx_len == 1U) && (rx[0] == pattern[0]) && (rx[1] == 0U),
                      "the 1-byte message comes back at exactly 1 byte, nothing past it touched");

    (void)memset(rx, 0, sizeof(rx));
    status = os_msg_receive(&os_test_msg, rx, sizeof(rx), &rx_len, OS_WAIT_NOTHING);
    AHURA_TEST_CHECK((status == OS_ERR_NONE) && (rx_len == 16U) && (memcmp(rx, pattern, 16U) == 0),
                      "the 16-byte message follows it, whole and in order (len=%lu)", (unsigned long)rx_len);
    AHURA_TEST_CHECK(os_msg_free_get(&os_test_msg) == capacity, "draining it returns every byte");

    /* --- The wrap: enough laps that head and tail pass the end mid-message ------------------- */

    for (lap = 0U; lap < 12U; lap++)
    {
        size_t length = (size_t)(1U + (lap % 16U));

        if (os_msg_send(&os_test_msg, pattern, length, OS_WAIT_NOTHING) != OS_ERR_NONE)
        {
            ok = false;
        }

        (void)memset(rx, 0, sizeof(rx));

        if ((os_msg_receive(&os_test_msg, rx, sizeof(rx), &rx_len, OS_WAIT_NOTHING) != OS_ERR_NONE) ||
            (rx_len != length) || (memcmp(rx, pattern, length) != 0))
        {
            ok = false;
        }
    }
    AHURA_TEST_CHECK(ok, "12 send/receive laps of changing length survive the ring wrap intact");
    AHURA_TEST_CHECK((os_msg_count_get(&os_test_msg) == 0U) && (os_msg_free_get(&os_test_msg) == capacity),
                      "and leave the accounting exactly where it started");

    /* --- A destination too small: refused, and the message stays ------------------------------ */

    AHURA_TEST_CHECK(os_msg_send(&os_test_msg, pattern, 24U, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "a 24-byte message is sent");

    rx_len = 0U;
    status = os_msg_receive(&os_test_msg, rx, 8U, &rx_len, OS_WAIT_NOTHING);
    AHURA_TEST_CHECK(status == OS_ERR_INVALID_ARG, "an 8-byte destination refuses it rather than truncating");
    AHURA_TEST_CHECK(rx_len == 24U, "and reports the size it would have needed (%lu)", (unsigned long)rx_len);
    AHURA_TEST_CHECK(os_msg_count_get(&os_test_msg) == 1U, "the message is still there after the refusal");

    (void)memset(rx, 0, sizeof(rx));
    status = os_msg_receive(&os_test_msg, rx, sizeof(rx), &rx_len, OS_WAIT_NOTHING);
    AHURA_TEST_CHECK((status == OS_ERR_NONE) && (rx_len == 24U) && (memcmp(rx, pattern, 24U) == 0),
                      "a big enough destination then gets it, unharmed");

    /* --- The three refusals ------------------------------------------------------------------ */

    AHURA_TEST_CHECK(os_msg_send(&os_test_msg, pattern, 0U, OS_WAIT_NOTHING) == OS_ERR_INVALID_ARG,
                      "a zero-length message is refused");

    /* OS_WAIT_FOREVER on purpose: a message that cannot fit even in an empty buffer has to be
     * refused rather than waited on, or this call would never return. */
    AHURA_TEST_CHECK(os_msg_send(&os_test_msg, pattern, capacity, OS_WAIT_FOREVER) == OS_ERR_INVALID_ARG,
                      "a message larger than the whole buffer is refused, not waited on");

    /* --- Back-pressure: FULL without waiting, TIMEOUT with --------------------------------- */

    ok = true;
    for (lap = 0U; lap < 3U; lap++)
    {
        if (os_msg_send(&os_test_msg, pattern, 16U, OS_WAIT_NOTHING) != OS_ERR_NONE)
        {
            ok = false;
        }
    }
    AHURA_TEST_CHECK(ok, "three 16-byte messages fill the buffer exactly");
    AHURA_TEST_CHECK(os_msg_free_get(&os_test_msg) == 0U, "a full buffer reports 0 bytes free");

    AHURA_TEST_CHECK(os_msg_send(&os_test_msg, pattern, 1U, OS_WAIT_NOTHING) == OS_ERR_FULL,
                      "send on a full buffer with OS_WAIT_NOTHING returns FULL");

    t0     = os_tick_get();
    status = os_msg_send(&os_test_msg, pattern, 1U, 100U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_ERR_TIMEOUT, "send on a full buffer times out");
    AHURA_TEST_CHECK((delta >= 95U) && (delta <= 150U), "timeout elapsed ~100 ticks (%lu)",
                      (unsigned long)delta);

    /* --- cleanup() puts it back --------------------------------------------------------------- */

    AHURA_TEST_CHECK(os_msg_cleanup(&os_test_msg) == OS_ERR_NONE,
                      "os_msg_cleanup() succeeds with no waiters");
    AHURA_TEST_CHECK((os_msg_count_get(&os_test_msg) == 0U) && (os_msg_free_get(&os_test_msg) == capacity),
                      "and leaves the statically defined buffer empty and whole again");
    AHURA_TEST_CHECK(os_test_msg.buffer != NULL,
                      "a compile-time buffer survives cleanup - there is nothing to release");

    /* --- os_msg_init_dynamic / os_msg_cleanup -------------------------------------------------- */

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    {
        OS_MSG_DEFINE_DYNAMIC(dynamic_msg);

        size_t  msg_heap_before;
        size_t  dyn_len;
        uint8_t dyn_rx[32];

        msg_heap_before = os_mem_free_get();

        AHURA_TEST_CHECK(os_msg_init_dynamic(&dynamic_msg, 4U * OS_MSG_SPACE(16U)) == OS_ERR_NONE,
                          "os_msg_init_dynamic() allocates room for four 16-byte messages");

        /* Ownership has to be true the moment the object is usable, not a moment later: it is what
         * tells os_msg_cleanup the buffer came from the heap. A call that published the object
         * before claiming ownership would leak that buffer to any cleanup landing in between. */
        AHURA_TEST_CHECK(dynamic_msg.buffer_owned,
                          "an allocated message buffer owns its storage as soon as it is usable");
        AHURA_TEST_CHECK(!os_test_msg.buffer_owned,
                          "a statically defined message buffer never claims ownership");

        AHURA_TEST_CHECK(os_mem_free_get() < msg_heap_before,
                          "creating it consumed kernel heap (%u -> %u bytes free)",
                          (unsigned)msg_heap_before, (unsigned)os_mem_free_get());

        AHURA_TEST_CHECK(os_msg_send(&dynamic_msg, "variable", 8U, OS_WAIT_NOTHING) == OS_ERR_NONE,
                          "the dynamic buffer accepts a message");
        dyn_len = 0U;
        AHURA_TEST_CHECK(os_msg_receive(&dynamic_msg, dyn_rx, sizeof(dyn_rx), &dyn_len,
                                        OS_WAIT_NOTHING) == OS_ERR_NONE,
                          "the dynamic buffer returns it");
        AHURA_TEST_CHECK((dyn_len == 8U) && (memcmp(dyn_rx, "variable", 8U) == 0),
                          "with the length and the bytes intact (%u)", (unsigned)dyn_len);

        AHURA_TEST_CHECK(os_msg_cleanup(&dynamic_msg) == OS_ERR_NONE,
                          "os_msg_cleanup() tears the dynamic one down");
        AHURA_TEST_CHECK(os_mem_free_get() == msg_heap_before,
                          "and returned every byte it took to the heap (%u bytes free)",
                          (unsigned)os_mem_free_get());
        AHURA_TEST_CHECK(dynamic_msg.buffer == NULL,
                          "the torn-down buffer no longer points at freed memory");

        /* A budget with no room for even one message is refused rather than accepted as an object
         * that exists and rejects every send it is ever given. */
        AHURA_TEST_CHECK(os_msg_init_dynamic(&dynamic_msg, 0U) == OS_ERR_INVALID_ARG,
                          "os_msg_init_dynamic() rejects a zero byte budget");
        AHURA_TEST_CHECK(os_msg_init_dynamic(&dynamic_msg, OS_MSG_HEADER_BYTES) == OS_ERR_INVALID_ARG,
                          "os_msg_init_dynamic() rejects a budget with room for a header and nothing else");
        AHURA_TEST_CHECK(os_msg_init_dynamic(NULL, 64U) == OS_ERR_INVALID_ARG,
                          "os_msg_init_dynamic() rejects a NULL object");

        /* Valid but larger than the whole heap has to come back as NO_MEMORY, so a caller can tell
         * "ask for less" apart from "that request was nonsense". */
        AHURA_TEST_CHECK(os_msg_init_dynamic(&dynamic_msg, OS_CONFIG_HEAP_SIZE * 2U) == OS_ERR_NO_MEMORY,
                          "os_msg_init_dynamic() reports NO_MEMORY when the heap cannot cover it");
    }
#endif /* OS_CONFIG_ALLOC_ENABLE */


    t0     = os_tick_get();
    status = os_msg_receive(&os_test_msg, rx, sizeof(rx), &rx_len, 100U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK(status == OS_ERR_TIMEOUT, "receive on an emptied buffer times out");
    AHURA_TEST_CHECK((delta >= 95U) && (delta <= 150U), "timeout elapsed ~100 ticks (%lu)",
                      (unsigned long)delta);
}
#endif /* OS_CONFIG_MSG_ENABLE */

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
    os_err_t status;
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  delta;

    test_print_section("Events");

    AHURA_TEST_CHECK(os_event_init(&os_test_event) == OS_ERR_NONE, "os_event_init() succeeds");

    matched = 0xFFFFFFFFU;
    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x03U, false, false, &matched, OS_WAIT_NOTHING) == OS_ERR_BUSY,
                      "wait-any on unset bits with OS_WAIT_NOTHING returns BUSY");
    AHURA_TEST_CHECK(matched == 0U, "matched_bits reports 0 when nothing matched");

    AHURA_TEST_CHECK(os_event_set_bits(&os_test_event, 0x01U) == OS_ERR_NONE,
                      "os_event_set_bits(0x01) succeeds");
    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x03U, false, false, &matched, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "wait-any matches once one of the requested bits is set");
    AHURA_TEST_CHECK(matched == 0x01U, "matched_bits reports the intersecting bits (0x%02lx)",
                      (unsigned long)matched);

    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x03U, true, false, &matched, OS_WAIT_NOTHING) == OS_ERR_BUSY,
                      "wait-all is still BUSY while only some requested bits are set");

    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x01U, false, true, &matched, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "wait-any with clear_on_exit consumes the matched bit");
    AHURA_TEST_CHECK(os_event_wait_bits(&os_test_event, 0x01U, false, false, &matched, OS_WAIT_NOTHING) == OS_ERR_BUSY,
                      "a consumed (atomically cleared) bit no longer matches");

    AHURA_TEST_CHECK(test_spawn_helper(HELPER_EVENT_SET_AFTER, 80U, 0x06U, 0U) == OS_ERR_NONE,
                      "helper spawned to set bits 0x06 after 80 ms");
    t0     = os_tick_get();
    status = os_event_wait_bits(&os_test_event, 0x06U, true, false, &matched, 500U);
    t1     = os_tick_get();
    delta  = t1 - t0;
    AHURA_TEST_CHECK((status == OS_ERR_NONE) && (matched == 0x06U),
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
static void timer_oneshot_cb(void *context, uint32_t value);

/* This object must be startable straight from its definition - the kernel has no init call. */
OS_TIMER_DEFINE_ONESHOT(os_test_defined_timer, 40U, timer_oneshot_cb);

static void timer_args_cb(void *context, uint32_t value);

OS_TIMER_DEFINE_PERIODIC(os_test_args_timer, 20U, timer_args_cb);

static __IO uint32_t os_test_args_runs    = 0U;
static __IO bool     os_test_args_ok      = false;
static uint32_t      os_test_args_marker  = 0x5EED5EEDUL;

/******************************************************************************************************/
static void timer_oneshot_cb(void *context, uint32_t value)
{
    (void)value;
    (void)context;
    os_test_oneshot_fired++;
}

/******************************************************************************************************/
static void timer_periodic_cb(void *context, uint32_t value)
{
    (void)value;
    (void)context;
    os_test_periodic_fired++;
}

/******************************************************************************************************/
/**
 * @brief Records whether both arguments arrived exactly as os_timer_start was given them.
 */
static void timer_args_cb(void *context, uint32_t value)
{
    os_test_args_ok = (context == &os_test_args_marker) && (value == 0xABCDU);
    os_test_args_runs++;
}

/******************************************************************************************************/
static void test_timer(void)
{
    uint32_t snapshot;

    test_print_section("Software Timer");

    os_test_oneshot_fired = 0U;
    AHURA_TEST_CHECK(os_timer_period_set(&os_test_timer_oneshot, 50U) == OS_ERR_NONE,
                      "the one-shot timer is defined at compile time and retuned to 50 ms");
    AHURA_TEST_CHECK(os_timer_start(&os_test_timer_oneshot, NULL, 0U) == OS_ERR_NONE, "os_timer_start() arms the one-shot timer");

    os_delay_ms(30U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "one-shot timer has not fired before its period elapses");
    os_delay_ms(50U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "one-shot timer fires exactly once (fired=%lu)",
                      (unsigned long)os_test_oneshot_fired);
    os_delay_ms(80U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "one-shot timer does not fire again on its own");

    os_test_periodic_fired = 0U;
    AHURA_TEST_CHECK(os_timer_period_set(&os_test_timer_periodic, 30U) == OS_ERR_NONE,
                      "the periodic timer is defined at compile time and retuned to 30 ms");
    AHURA_TEST_CHECK(os_timer_start(&os_test_timer_periodic, NULL, 0U) == OS_ERR_NONE, "os_timer_start() arms the periodic timer");
    os_delay_ms(160U);
    AHURA_TEST_CHECK((os_test_periodic_fired >= 4U) && (os_test_periodic_fired <= 7U),
                      "periodic timer fires repeatedly (~5x expected in 160 ms, fired=%lu)",
                      (unsigned long)os_test_periodic_fired);

    AHURA_TEST_CHECK(os_timer_stop(&os_test_timer_periodic) == OS_ERR_NONE, "os_timer_stop() disarms the periodic timer");
    snapshot = os_test_periodic_fired;
    os_delay_ms(90U);
    AHURA_TEST_CHECK(os_test_periodic_fired == snapshot, "no further fires after os_timer_stop()");

    /* --- pause / resume, restart, delete --- */

    /* A 100 ms one-shot paused 40 ms in has ~60 ms left. Resuming must fire ~60 ms later, not
     * ~100 ms, which is what separates os_timer_start's resume from os_timer_restart's reload. */
    os_test_oneshot_fired = 0U;
    (void)os_timer_period_set(&os_test_timer_oneshot, 100U);
    (void)os_timer_start(&os_test_timer_oneshot, NULL, 0U);
    os_delay_ms(40U);

    AHURA_TEST_CHECK(os_timer_pause(&os_test_timer_oneshot) == OS_ERR_NONE, "os_timer_pause() halts a running timer");
    os_delay_ms(150U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "a paused timer does not fire");

    (void)os_timer_start(&os_test_timer_oneshot, NULL, 0U);
    os_delay_ms(40U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "start() resumes the time left, not a full period");
    os_delay_ms(50U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "the resumed timer expires");
    AHURA_TEST_CHECK(os_timer_pause(&os_test_timer_oneshot) == OS_ERR_ERROR, "pausing a stopped timer is an error");

    /* Restart 70 ms into a 100 ms period: the deadline moves out a whole period from now. */
    os_test_oneshot_fired = 0U;
    (void)os_timer_period_set(&os_test_timer_oneshot, 100U);
    (void)os_timer_start(&os_test_timer_oneshot, NULL, 0U);
    os_delay_ms(70U);
    AHURA_TEST_CHECK(os_timer_restart(&os_test_timer_oneshot, NULL, 0U) == OS_ERR_NONE, "os_timer_restart() re-arms 70 ms in");
    os_delay_ms(50U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "restart moved the deadline");
    os_delay_ms(70U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "fires a full period after restart");

    os_test_periodic_fired = 0U;
    (void)os_timer_period_set(&os_test_timer_periodic, 30U);
    (void)os_timer_start(&os_test_timer_periodic, NULL, 0U);
    os_delay_ms(50U);

    AHURA_TEST_CHECK(os_timer_stop(&os_test_timer_periodic) == OS_ERR_NONE, "os_timer_stop() tears it down");
    snapshot = os_test_periodic_fired;
    os_delay_ms(90U);
    AHURA_TEST_CHECK(os_test_periodic_fired == snapshot, "no further fires after os_timer_stop()");

    /* A stopped timer keeps its configuration, so starting it again needs no re-init. */
    AHURA_TEST_CHECK(os_timer_start(&os_test_timer_periodic, NULL, 0U) == OS_ERR_NONE,
                      "a stopped timer starts again without being re-initialized");
    (void)os_timer_stop(&os_test_timer_periodic);

    /* Retuning leaves the countdown alone, so the period only bites on the next reload. */
    AHURA_TEST_CHECK(os_timer_period_set(&os_test_timer_periodic, 25U) == OS_ERR_NONE,
                      "os_timer_period_set() accepts a new period");
    AHURA_TEST_CHECK((os_timer_period_set(NULL, 25U) == OS_ERR_INVALID_ARG) &&
                     (os_timer_period_set(&os_test_timer_periodic, 0U) == OS_ERR_INVALID_ARG) &&
                     (os_timer_period_set(&os_test_timer_periodic, OS_WAIT_FOREVER) == OS_ERR_INVALID_ARG),
                      "and refuses NULL, 0 and OS_WAIT_FOREVER");
    os_test_periodic_fired = 0U;
    (void)os_timer_start(&os_test_timer_periodic, NULL, 0U);
    os_delay_ms(120U);
    (void)os_timer_stop(&os_test_timer_periodic);
    AHURA_TEST_CHECK(os_test_periodic_fired >= 3U,
                      "and the retuned 25 ms period is what runs (fired=%lu in 120 ms)",
                      (unsigned long)os_test_periodic_fired);

    /* Repointing a timer at a different callback is how one object serves more than one job now
     * that there is no init call. */
    AHURA_TEST_CHECK(os_timer_callback_set(&os_test_timer_periodic, timer_periodic_cb) == OS_ERR_NONE,
                      "os_timer_callback_set() accepts a new callback");
    AHURA_TEST_CHECK((os_timer_callback_set(NULL, timer_periodic_cb) == OS_ERR_INVALID_ARG) &&
                     (os_timer_callback_set(&os_test_timer_periodic, NULL) == OS_ERR_INVALID_ARG),
                      "and refuses a NULL timer or callback");

    /* The DEFINE macro settles period, mode and callback at compile time, so the object is
     * startable with no init call at all, and the state it produces has to be exactly what a
     * timer starts life in, or it would never fire. */
    os_test_oneshot_fired = 0U;
    AHURA_TEST_CHECK(os_timer_start(&os_test_defined_timer, NULL, 0U) == OS_ERR_NONE,
                      "a DEFINE macro gives a timer that is startable as declared");
    os_delay_ms(90U);
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U,
                      "and it fires once, on its compile-time period (fired=%lu)",
                      (unsigned long)os_test_oneshot_fired);
    (void)os_timer_period_set(&os_test_timer_periodic, 30U);
    (void)os_timer_stop(&os_test_timer_periodic);

    /* ---- the run's arguments come from os_timer_start ---- */
    /* A periodic timer, so the second expiry proves they PERSIST across reloads rather than being
     * consumed by the first delivery. */
    os_test_args_runs = 0U;
    os_test_args_ok   = false;
    AHURA_TEST_CHECK(os_timer_start(&os_test_args_timer, &os_test_args_marker, 0xABCDU) == OS_ERR_NONE,
                      "os_timer_start() takes the context and value for this run");
    os_delay_ms(70U);
    (void)os_timer_stop(&os_test_args_timer);
    AHURA_TEST_CHECK(os_test_args_runs >= 2U, "the periodic timer ran more than once (%lu)",
                      (unsigned long)os_test_args_runs);
    AHURA_TEST_CHECK(os_test_args_ok,
                      "and both arguments reached the callback unchanged, on every expiry");

    /* Restarting for a different job re-points both, which is what makes one object reusable. */
    os_test_args_ok = false;
    AHURA_TEST_CHECK(os_timer_restart(&os_test_args_timer, NULL, 0U) == OS_ERR_NONE,
                      "os_timer_restart() re-points them for the next run");
    os_delay_ms(40U);
    (void)os_timer_stop(&os_test_args_timer);
    AHURA_TEST_CHECK(!os_test_args_ok, "and the callback saw the new pair, not the old one");

    /* ---- a timer that never came from a DEFINE macro is refused, not followed ---- */
    /* This is the check the magic word exists for. Such an object's list nodes hold whatever the
     * memory contained; a kernel that trusted them would run os_list_remove over a garbage
     * neighbour pointer and write to an address nobody chose. Every entry point has to refuse it -
     * os_timer_stop above all, since that is the one that unlinks. The object is deliberately
     * filled with a non-zero pattern rather than left blank, because zeroed memory would pass a
     * NULL check and hide the very failure this is about. */
    {
        os_timer_t undefined_timer;

        memset(&undefined_timer, 0xA5, sizeof(undefined_timer));

        AHURA_TEST_CHECK(os_timer_stop(&undefined_timer) == OS_ERR_INVALID_ARG,
                          "os_timer_stop() refuses a timer that never came from a DEFINE macro");
        AHURA_TEST_CHECK(os_timer_start(&undefined_timer, NULL, 0U) == OS_ERR_INVALID_ARG,
                          "os_timer_start() refuses it too");
        AHURA_TEST_CHECK(os_timer_restart(&undefined_timer, NULL, 0U) == OS_ERR_INVALID_ARG,
                          "and os_timer_restart()");
        AHURA_TEST_CHECK(os_timer_pause(&undefined_timer) == OS_ERR_INVALID_ARG,
                          "and os_timer_pause()");
        AHURA_TEST_CHECK(os_timer_period_set(&undefined_timer, 10U) == OS_ERR_INVALID_ARG,
                          "and os_timer_period_set()");
        AHURA_TEST_CHECK(os_timer_callback_set(&undefined_timer, timer_oneshot_cb) == OS_ERR_INVALID_ARG,
                          "and os_timer_callback_set()");
        AHURA_TEST_CHECK(os_timer_value_set(&undefined_timer, 1U) == OS_ERR_INVALID_ARG,
                          "and os_timer_value_set()");

        /* A COPY of a perfectly valid timer, which is the case a fixed magic constant cannot see:
         * every field including the signature would match, but the copy's list nodes still point
         * into the ORIGINAL, so unlinking the copy would corrupt the original's neighbours. The
         * self-pointer catches it because the copy no longer lives where its self field says. */
        {
            os_timer_t copy = os_test_defined_timer;

            AHURA_TEST_CHECK(os_timer_stop(&copy) == OS_ERR_INVALID_ARG,
                              "a COPY of a valid timer is refused - its links belong to the original");
            AHURA_TEST_CHECK(os_timer_start(&copy, NULL, 0U) == OS_ERR_INVALID_ARG,
                              "and starting the copy is refused too");
        }

        /* The residual case, and the one the membership walk exists for: an object whose marker
         * is FORGED, so it passes every identity check the kernel can make, but whose list nodes
         * are still garbage. This is what "the self-pointer happens to match by accident" would
         * look like, constructed deliberately because it cannot be produced by chance.
         *
         * Without the walk, os_timer_stop reaches os_list_remove, reads prev = 0xA5A5A5A5 and
         * executes a store to 0xA5A5A5A9. With it, the list is searched, the node is not a member,
         * and nothing is written at all. */
        {
            os_timer_t rogue;

            memset(&rogue, 0xA5, sizeof(rogue));
            rogue.self         = &rogue;            /* forged: passes the identity check */
            rogue.callback     = timer_oneshot_cb;
            rogue.period_ticks = 5U;

            AHURA_TEST_CHECK(os_timer_stop(&rogue) == OS_ERR_NONE,
                              "a forged timer passes the identity check, as constructed");
            AHURA_TEST_CHECK(os_timer_pause(&rogue) == OS_ERR_ERROR,
                              "and pausing it reports 'not running' rather than touching a list");
        }

        /* Still healthy afterwards: had any of those followed a garbage pointer, the damage would
         * have landed in the running list, and this timer would not come back. */
        os_test_oneshot_fired = 0U;
        (void)os_timer_start(&os_test_defined_timer, NULL, 0U);
        os_delay_ms(90U);
        AHURA_TEST_CHECK(os_test_oneshot_fired == 1U,
                          "and the kernel's own lists are intact after all seven refusals");
    }
}
#endif /* OS_CONFIG_TIMER_ENABLE */

/*
 * ***********************************************************************************************************
 * The timer API from an ISR
 * ***********************************************************************************************************
 *
 * Everything above calls the timer API from a task. This section calls it from a real exception
 * handler, because "ISR-safe" is a claim worth executing rather than reasoning about.
 *
 * SVC is the vehicle. It exists on every Cortex-M, it is synchronous - so the test knows exactly
 * when the handler ran, with no peripheral to configure and no vendor header to include - and the
 * kernel deliberately claims no SVC handler of its own. An application that defines one cannot link
 * this suite: a duplicate symbol, which is the loudest way for that clash to be noticed.
*/

#if (OS_CONFIG_TIMER_ENABLE == 1U)

#define TEST_ISR_ACTION_ARM   0U
#define TEST_ISR_ACTION_STOP  1U

static void test_isr_timer_cb(void *context, uint32_t value);
static void test_isr_defer_cb(void *context, uint32_t value);

/* One timer the ISR arms and later cancels, and one it uses as a deferred call - the "run this
 * soon" case, which in this kernel is simply a one-shot with a one-tick period. */
OS_TIMER_DEFINE_ONESHOT(os_test_isr_timer, 1000U, test_isr_timer_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_isr_defer, 1U, test_isr_defer_cb);

static uint32_t       os_test_isr_marker        = 0xC0FFEEUL;

static __IO uint32_t  os_test_isr_action        = TEST_ISR_ACTION_ARM;
static __IO uint32_t  os_test_isr_entered       = 0U;
static __IO bool      os_test_isr_was_isr       = false;
static __IO os_err_t os_test_isr_period_status = OS_ERR_ERROR;
static __IO os_err_t os_test_isr_start_status  = OS_ERR_ERROR;
static __IO os_err_t os_test_isr_defer_status  = OS_ERR_ERROR;
static __IO os_err_t os_test_isr_stop_status   = OS_ERR_ERROR;
static __IO uint32_t  os_test_isr_timer_fired   = 0U;
static __IO uint32_t  os_test_isr_defer_ran     = 0U;
static __IO bool      os_test_isr_args_ok       = false;

/******************************************************************************************************/
static void test_isr_timer_cb(void *context, uint32_t value)
{
    (void)context;
    (void)value;
    os_test_isr_timer_fired++;
}

/******************************************************************************************************/
/**
 * @brief The deferred call the ISR scheduled. Both of its arguments came from the os_timer_start
 *        the ISR made, which is the whole point of the check.
 */
static void test_isr_defer_cb(void *context, uint32_t value)
{
    os_test_isr_args_ok = (context == &os_test_isr_marker) && (value == 0x0DEFU);
    os_test_isr_defer_ran++;
}

/******************************************************************************************************/
/**
 * @brief The interrupt under test: it does nothing but call the timer API and record what came
 *        back. Reached with "svc #0" from the test task below.
 */
void OS_CONFIG_ARCH_SVC_HANDLER(void)
{
    os_test_isr_entered++;
    os_test_isr_was_isr = os_arch_in_isr();

    if (os_test_isr_action == TEST_ISR_ACTION_ARM)
    {
        os_test_isr_period_status = os_timer_period_set(&os_test_isr_timer, 40U);
        os_test_isr_start_status  = os_timer_start(&os_test_isr_timer, NULL, 0U);

        /* Deferred work, scheduled from an interrupt, carrying its own data - one call, no pool. */
        os_test_isr_defer_status  = os_timer_start(&os_test_isr_defer, &os_test_isr_marker, 0x0DEFU);
    }
    else
    {
        os_test_isr_stop_status = os_timer_stop(&os_test_isr_timer);
    }
}

/******************************************************************************************************/
static void test_timer_isr(void)
{
    /* This section synthesises ISR context with `svc #0`, and confirms the vector first by
     * reading the table through VTOR. Both are Cortex-M: RISC-V has no SVC exception, no VTOR,
     * and its vector table holds jump instructions rather than addresses, so there is nothing
     * here to translate one-for-one.
     *
     * Skipped loudly rather than quietly compiled out, because "Timer API from an ISR" simply
     * absent from the report would read as a suite that passed everything it should have run.
     *
     * The natural equivalent on this platform is a spare IRQ raised with irq_set_pending(),
     * which would exercise these same paths through the real external-interrupt route that
     * os_arch_in_isr() actually watches. That needs SDK calls the suite currently does without,
     * so it is a deliberate gap rather than an oversight. */
#if !defined(OS_ARCH_REG_VTOR) || !defined(OS_ARCH_VECTOR_SVC)
    test_print_section("Timer API from an ISR");
    printf("  [SKIP] this port has no SVC exception to raise ISR context with\r\n");
    printf("         Cortex-M specific; see the comment at test_timer_isr().\r\n");
    return;
#else
    /* SHPR2: SVCall's priority byte is the top one. Written as a WORD - byte access to this bank is
     * not architecturally guaranteed on ARMv6-M - and 0xFF saturates to the lowest priority
     * whatever the implemented bits are. SVCall resets to 0, which is above any
     * OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY threshold, and the kernel traps a call from an interrupt
     * its mask cannot reach. Lowering it is exactly what an application must do for an ISR of its
     * own, so doing it here is part of what the section demonstrates. */
    __IO uint32_t *shpr2 = (__IO uint32_t *)0xE000ED1CUL;

    test_print_section("Timer API from an ISR");

    /* Before the first svc: confirm the vector table actually routes SVC to the handler above.
     * It is not a link error when it does not - the handler links fine and simply nothing points
     * at it, because the startup file named that entry something else (isr_svcall on the Pico SDK,
     * SVC_Handler under CMSIS-Pack). The instruction then lands on the startup file's default
     * stub, which is a breakpoint on every SDK checked, and with no debugger attached that
     * escalates to HardFault whose default handler is also a breakpoint. The board hangs here,
     * mid-suite, with the last PASS line as the only clue.
     *
     * OS_CONFIG_ARCH_SVC_HANDLER is what fixes it, and a SoC package sets it. Skipping loudly is
     * the right behaviour when it is wrong: a suite that stops dead tells you less than one that
     * says which vector to name. */
    {
        const uint32_t *vector_table = (const uint32_t *)(uintptr_t)OS_ARCH_REG_VTOR;
        uint32_t        installed    = vector_table[OS_ARCH_VECTOR_SVC] & ~(uint32_t)1U;
        uint32_t        expected     = (uint32_t)(uintptr_t)&OS_CONFIG_ARCH_SVC_HANDLER & ~(uint32_t)1U;

        if (installed != expected)
        {
            printf("  [SKIP] the SVC vector does not point at this suite's handler\r\n");
            printf("         vector[11]=0x%08lX, handler=0x%08lX\r\n",
                   (unsigned long)installed, (unsigned long)expected);
            printf("         Set OS_CONFIG_ARCH_SVC_HANDLER to this target's SVC vector name\r\n");
            printf("         (a SoC package does this: AHURA_SOC_SVC_HANDLER in its soc.cmake).\r\n");
            printf("         Skipped rather than run: `svc` would hang the board here.\r\n");
            return;
        }
    }

    *shpr2 = (*shpr2 & 0x00FFFFFFUL) | 0xFF000000UL;

    /* ---- start, retune and defer, all from the handler ---- */
    os_test_isr_entered     = 0U;
    os_test_isr_timer_fired = 0U;
    os_test_isr_defer_ran   = 0U;
    os_test_isr_args_ok     = false;
    os_test_isr_action      = TEST_ISR_ACTION_ARM;

    __asm volatile("svc #0" ::: "memory");

    AHURA_TEST_CHECK(os_test_isr_entered == 1U, "the SVC handler ran once (%lu)",
                      (unsigned long)os_test_isr_entered);
    AHURA_TEST_CHECK(os_test_isr_was_isr, "and the kernel agrees that was ISR context");
    AHURA_TEST_CHECK(os_test_isr_period_status == OS_ERR_NONE, "os_timer_period_set() from an ISR returns OK");
    AHURA_TEST_CHECK(os_test_isr_start_status == OS_ERR_NONE, "os_timer_start() from an ISR returns OK");
    AHURA_TEST_CHECK(os_test_isr_defer_status == OS_ERR_NONE,
                      "and deferring work from an ISR is the same call, which cannot be refused");

    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_isr_defer_ran == 1U, "the deferred call ran once (%lu)",
                      (unsigned long)os_test_isr_defer_ran);
    AHURA_TEST_CHECK(os_test_isr_args_ok,
                      "with the context and value the ISR handed to os_timer_start");

    os_delay_ms(60U);
    AHURA_TEST_CHECK(os_test_isr_timer_fired == 1U,
                      "and the timer the ISR started fired once, on the period the ISR set (%lu)",
                      (unsigned long)os_test_isr_timer_fired);

    /* ---- and cancelling from the handler, which is the other half of the claim ---- */
    os_test_isr_timer_fired = 0U;
    (void)os_timer_period_set(&os_test_isr_timer, 40U);
    (void)os_timer_start(&os_test_isr_timer, NULL, 0U);

    os_test_isr_action = TEST_ISR_ACTION_STOP;
    __asm volatile("svc #0" ::: "memory");

    AHURA_TEST_CHECK(os_test_isr_stop_status == OS_ERR_NONE, "os_timer_stop() from an ISR returns OK");
    os_delay_ms(80U);
    AHURA_TEST_CHECK(os_test_isr_timer_fired == 0U,
                      "and the timer it cancelled never fired (%lu)",
                      (unsigned long)os_test_isr_timer_fired);
    AHURA_TEST_CHECK(os_test_isr_entered == 2U, "both handler entries accounted for (%lu)",
                      (unsigned long)os_test_isr_entered);
#endif /* OS_ARCH_REG_VTOR && OS_ARCH_VECTOR_SVC */
}
#endif /* OS_CONFIG_TIMER_ENABLE */


/*
 * ***********************************************************************************************************
 * Deferred calls - os_timer_submit and its pool
 * ***********************************************************************************************************
 *
 * The property that separates a submission from a start, and the reason both exist: os_timer_start
 * on a pending timer RESCHEDULES it, so an interrupt firing twice produces one callback carrying
 * only the second event. os_timer_submit takes a fresh slot each time, so both events arrive.
 *
 * The tests below assert both halves - the coalescing AND the non-coalescing - because either one
 * alone would leave the difference undocumented by anything executable.
*/

#if (OS_CONFIG_TIMER_ENABLE == 1U)

#define TEST_POOL_SIZE     4U
#define TEST_POOL_LOG_MAX  8U

static void test_pool_cb(void *context, uint32_t value);
static void test_pool_other_cb(void *context, uint32_t value);
static void test_coalesce_cb(void *context, uint32_t value);

/* Three pools rather than three delays at the call site: the delay belongs to the definition now,
 * so "same work, different timing" is a second pool. */
OS_TIMER_DEFINE_SUBMIT(os_test_pool,      TEST_POOL_SIZE, 0U,  test_pool_cb);
OS_TIMER_DEFINE_SUBMIT(os_test_pool_slow, TEST_POOL_SIZE, 60U, test_pool_cb);
OS_TIMER_DEFINE_SUBMIT(os_test_pool_b,    2U,             0U,  test_pool_other_cb);

/* The contrast case: one ordinary one-shot, started twice. */
OS_TIMER_DEFINE_ONESHOT(os_test_coalesce, 30U, test_coalesce_cb);

static uint32_t      os_test_pool_marker = 0xFEEDU;
static __IO uint32_t os_test_pool_log[TEST_POOL_LOG_MAX];
static __IO uint32_t os_test_pool_runs  = 0U;
static __IO bool     os_test_pool_ctx_ok = true;
static __IO uint32_t os_test_pool_b_runs = 0U;
static __IO uint32_t os_test_coalesce_runs = 0U;
static __IO uint32_t os_test_coalesce_last = 0U;

/******************************************************************************************************/
static void test_pool_cb(void *context, uint32_t value)
{
    if (context != &os_test_pool_marker) { os_test_pool_ctx_ok = false; }

    if (os_test_pool_runs < TEST_POOL_LOG_MAX) { os_test_pool_log[os_test_pool_runs] = value; }

    os_test_pool_runs++;
}

/******************************************************************************************************/
static void test_pool_other_cb(void *context, uint32_t value)
{
    (void)context;
    (void)value;
    os_test_pool_b_runs++;
}

/******************************************************************************************************/
static void test_coalesce_cb(void *context, uint32_t value)
{
    (void)context;
    os_test_coalesce_last = value;
    os_test_coalesce_runs++;
}

/******************************************************************************************************/
static void test_timer_pool(void)
{
    uint32_t filled;
    uint32_t index;
    bool     ok;

    test_print_section("Deferred Calls (os_timer_submit)");

    /* ---- the whole point: two submissions before either is delivered ---- */
    /* The kernel lock is what makes this an honest model of an interrupt firing twice: the timer
     * task runs above this one, so without it the first call would be delivered before the second
     * was even made, and nothing would be proved. */
    os_test_pool_runs   = 0U;
    os_test_pool_ctx_ok = true;

    os_kernel_lock();
    ok  = (os_timer_submit(&os_test_pool, &os_test_pool_marker, 11U) == OS_ERR_NONE);
    ok &= (os_timer_submit(&os_test_pool, &os_test_pool_marker, 22U) == OS_ERR_NONE);
    os_kernel_unlock();

    AHURA_TEST_CHECK(ok, "two submissions made back to back are both accepted");
    os_delay_ms(30U);
    AHURA_TEST_CHECK(os_test_pool_runs == 2U,
                      "and the callback runs TWICE, not once (%lu)", (unsigned long)os_test_pool_runs);
    AHURA_TEST_CHECK((os_test_pool_log[0] == 11U) && (os_test_pool_log[1] == 22U),
                      "each carrying its own value, in submission order (%lu, %lu)",
                      (unsigned long)os_test_pool_log[0], (unsigned long)os_test_pool_log[1]);
    AHURA_TEST_CHECK(os_test_pool_ctx_ok, "and its own context");

    /* ---- the contrast, which is why both calls exist ---- */
    os_test_coalesce_runs = 0U;
    os_test_coalesce_last = 0U;

    os_kernel_lock();
    (void)os_timer_start(&os_test_coalesce, &os_test_pool_marker, 11U);
    (void)os_timer_start(&os_test_coalesce, &os_test_pool_marker, 22U);
    os_kernel_unlock();

    os_delay_ms(70U);
    AHURA_TEST_CHECK(os_test_coalesce_runs == 1U,
                      "os_timer_start twice over RESCHEDULES: one callback, not two (%lu)",
                      (unsigned long)os_test_coalesce_runs);
    AHURA_TEST_CHECK(os_test_coalesce_last == 22U,
                      "carrying only the later event - which is the loss os_timer_submit avoids");

    /* ---- a pool runs out on its own, and only its own ---- */
    os_test_pool_runs = 0U;
    filled            = 0U;
    while (os_timer_submit(&os_test_pool_slow, &os_test_pool_marker, filled) == OS_ERR_NONE)
    {
        filled++;
    }
    AHURA_TEST_CHECK(filled == TEST_POOL_SIZE,
                      "a pool accepts exactly its own entry count (%lu of %u)",
                      (unsigned long)filled, (unsigned)TEST_POOL_SIZE);

    os_test_pool_b_runs = 0U;
    AHURA_TEST_CHECK(os_timer_submit(&os_test_pool_b, NULL, 0U) == OS_ERR_NONE,
                      "and a FULL pool does not affect another pool at all");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_test_pool_b_runs == 1U, "whose own call really ran");

    os_delay_ms(120U);
    AHURA_TEST_CHECK(os_test_pool_runs == TEST_POOL_SIZE,
                      "every queued submission ran once its delay elapsed (%lu of %u)",
                      (unsigned long)os_test_pool_runs, (unsigned)TEST_POOL_SIZE);

    ok = true;
    for (index = 0U; index < TEST_POOL_SIZE; index++)
    {
        if (os_test_pool_log[index] != index) { ok = false; }
    }
    AHURA_TEST_CHECK(ok, "in the order they were submitted, each with its own value");

    /* ---- entries come back, cycle after cycle ---- */
    ok = true;
    for (index = 0U; index < 3U; index++)
    {
        filled = 0U;
        while (os_timer_submit(&os_test_pool_slow, &os_test_pool_marker, 0U) == OS_ERR_NONE)
        {
            filled++;
        }

        if (filled != TEST_POOL_SIZE) { ok = false; }

        os_delay_ms(120U);
    }
    AHURA_TEST_CHECK(ok, "the pool refills to exactly %u entries after every drain (last %lu)",
                      (unsigned)TEST_POOL_SIZE, (unsigned long)filled);

    /* ---- a pool entry is not a timer the public API will touch ---- */
    /* OS_TIMER_DEFINE_SUBMIT declares the entries array in this file's scope, so one can be named.
     * Arming a FREE entry would link its running_node into the running list while its ready_node is
     * still on the pool's free list, and the next expiry would push that same node onto the
     * delivery queue - overwriting the links the free list holds it by. The pool has been used by
     * now, so these entries are fully prepared and would otherwise pass every other check. */
    ok  = (os_timer_start(&os_test_pool_timer_buf[0].timer, NULL, 0U) == OS_ERR_INVALID_ARG);
    ok &= (os_timer_restart(&os_test_pool_timer_buf[0].timer, NULL, 0U) == OS_ERR_INVALID_ARG);
    ok &= (os_timer_stop(&os_test_pool_timer_buf[0].timer) == OS_ERR_INVALID_ARG);
    ok &= (os_timer_pause(&os_test_pool_timer_buf[0].timer) == OS_ERR_INVALID_ARG);
    ok &= (os_timer_period_set(&os_test_pool_timer_buf[0].timer, 10U) == OS_ERR_INVALID_ARG);
    ok &= (os_timer_callback_set(&os_test_pool_timer_buf[0].timer, test_pool_cb) == OS_ERR_INVALID_ARG);
    ok &= (os_timer_value_set(&os_test_pool_timer_buf[0].timer, 1U) == OS_ERR_INVALID_ARG);
    AHURA_TEST_CHECK(ok, "every os_timer_* call refuses a pool entry - it belongs to os_timer_submit");

    /* Filling THIS pool needs the kernel lock: its delay is 0, so without it the timer task -
     * which outranks this one - would deliver and free each entry before the loop asked for the
     * next, the pool would never report FULL, and the loop would never end. */
    os_test_pool_runs = 0U;
    filled            = 0U;

    os_kernel_lock();
    while (os_timer_submit(&os_test_pool, &os_test_pool_marker, filled) == OS_ERR_NONE)
    {
        filled++;
    }
    os_kernel_unlock();

    os_delay_ms(40U);
    AHURA_TEST_CHECK((filled == TEST_POOL_SIZE) && (os_test_pool_runs == TEST_POOL_SIZE),
                      "and the pool is intact afterwards (%lu accepted, %lu ran)",
                      (unsigned long)filled, (unsigned long)os_test_pool_runs);

    /* ---- refusals ---- */
    AHURA_TEST_CHECK(os_timer_submit(NULL, NULL, 0U) == OS_ERR_INVALID_ARG,
                      "a NULL pool is refused");

    {
        os_timer_pool_t rogue;

        memset(&rogue, 0xA5, sizeof(rogue));
        AHURA_TEST_CHECK(os_timer_submit(&rogue, NULL, 0U) == OS_ERR_INVALID_ARG,
                          "and a pool that never came from OS_TIMER_DEFINE_SUBMIT is refused");
    }
}
#endif /* OS_CONFIG_TIMER_ENABLE */


/*
 * ***********************************************************************************************************
 * Timers under real-project conditions
 * ***********************************************************************************************************
 *
 * The sections above check one property at a time. These are the situations an application actually
 * produces: callbacks that re-arm or cancel themselves, a callback that blocks, work that outruns
 * its own period, a cancel that races the expiry it is cancelling, and an interrupt burst deeper
 * than the pool it feeds. Each is a place where a plausible implementation passes every test above
 * and still fails in the field.
*/

#if (OS_CONFIG_TIMER_ENABLE == 1U)

#define TEST_RW_CHAIN_TARGET   5U
#define TEST_RW_SAME_PERIOD    4U

static void test_rw_rearm_cb(void *context, uint32_t value);
static void test_rw_selfstop_cb(void *context, uint32_t value);
static void test_rw_slow_cb(void *context, uint32_t value);
static void test_rw_owed_cb(void *context, uint32_t value);
static void test_rw_same_cb(void *context, uint32_t value);
static void test_rw_retune_cb(void *context, uint32_t value);
static void test_rw_chain_cb(void *context, uint32_t value);
static void test_rw_pool_cb(void *context, uint32_t value);
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static void test_rw_block_cb(void *context, uint32_t value);
#endif

OS_TIMER_DEFINE_ONESHOT(os_test_rw_rearm,    20U, test_rw_rearm_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_rw_selfstop, 20U, test_rw_selfstop_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_rw_slow,     10U, test_rw_slow_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_rw_owed,     20U, test_rw_owed_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_rw_retune,   40U, test_rw_retune_cb);
OS_TIMER_DEFINE_ONESHOT(os_test_rw_chain,    15U, test_rw_chain_cb);
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
OS_TIMER_DEFINE_ONESHOT(os_test_rw_block,    20U, test_rw_block_cb);
#endif

OS_TIMER_DEFINE_PERIODIC(os_test_rw_same0, 30U, test_rw_same_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_rw_same1, 30U, test_rw_same_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_rw_same2, 30U, test_rw_same_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_rw_same3, 30U, test_rw_same_cb);

static os_timer_t *os_test_rw_same[TEST_RW_SAME_PERIOD] = {
    &os_test_rw_same0, &os_test_rw_same1, &os_test_rw_same2, &os_test_rw_same3,
};

/* Two pools sharing ONE callback, to prove their values cannot cross. */
OS_TIMER_DEFINE_SUBMIT(os_test_rw_pool_a, 3U, 0U, test_rw_pool_cb);
OS_TIMER_DEFINE_SUBMIT(os_test_rw_pool_b, 3U, 0U, test_rw_pool_cb);

/* ONE entry, deliberately: a callback that submits again can only succeed if the entry it is being
 * delivered on is already back in the pool. Depth 1 makes that the only way the chain can run. */
OS_TIMER_DEFINE_SUBMIT(os_test_rw_chain_pool, 1U, 0U, test_rw_chain_cb);

static __IO uint32_t os_test_rw_rearm_runs   = 0U;
static __IO uint32_t os_test_rw_selfstop_runs = 0U;
static __IO uint32_t os_test_rw_slow_runs    = 0U;
static __IO uint32_t os_test_rw_owed_runs    = 0U;
static __IO uint32_t os_test_rw_same_seen[TEST_RW_SAME_PERIOD];
static __IO uint32_t os_test_rw_retune_runs  = 0U;
static __IO uint32_t os_test_rw_chain_runs   = 0U;
static __IO uint32_t os_test_rw_chain_fails  = 0U;
static __IO uint32_t os_test_rw_pool_a_sum   = 0U;
static __IO uint32_t os_test_rw_pool_b_sum   = 0U;
static uint32_t      os_test_rw_marker_a     = 0xAAU;
static uint32_t      os_test_rw_marker_b     = 0xBBU;
#if (OS_CONFIG_MUTEX_ENABLE == 1U)
static os_mutex_t    os_test_rw_mutex;
static __IO bool     os_test_rw_block_done = false;
#endif

/******************************************************************************************************/
/** A one-shot that re-arms itself: the classic self-chaining timer. */
static void test_rw_rearm_cb(void *context, uint32_t value)
{
    os_test_rw_rearm_runs++;

    if (value > 1U)
    {
        /* Legal precisely because a finished one-shot leaves the running list BEFORE its callback
         * is invoked - so the object is free to be armed again from inside itself. */
        (void)os_timer_start(&os_test_rw_rearm, context, value - 1U);
    }
}

/******************************************************************************************************/
/** A periodic timer that cancels itself once its job is done. */
static void test_rw_selfstop_cb(void *context, uint32_t value)
{
    (void)context;
    os_test_rw_selfstop_runs++;

    if (os_test_rw_selfstop_runs >= value)
    {
        (void)os_timer_stop(&os_test_rw_selfstop);
    }
}

/******************************************************************************************************/
/** Work that takes far longer than the period that scheduled it. */
static void test_rw_slow_cb(void *context, uint32_t value)
{
    (void)context;
    (void)value;
    os_test_rw_slow_runs++;
    os_delay_ms(40U);          /* four periods' worth, on the timer task */
}

/******************************************************************************************************/
static void test_rw_owed_cb(void *context, uint32_t value)
{
    (void)context;
    (void)value;
    os_test_rw_owed_runs++;
}

/******************************************************************************************************/
static void test_rw_same_cb(void *context, uint32_t value)
{
    (void)context;
    if (value < TEST_RW_SAME_PERIOD) { os_test_rw_same_seen[value]++; }
}

/******************************************************************************************************/
/** Retunes its own period from inside itself. */
static void test_rw_retune_cb(void *context, uint32_t value)
{
    (void)context;
    (void)value;
    os_test_rw_retune_runs++;

    if (os_test_rw_retune_runs == 1U)
    {
        (void)os_timer_period_set(&os_test_rw_retune, 15U);
    }
}

/******************************************************************************************************/
/** Defers more work from inside a deferred call. */
static void test_rw_chain_cb(void *context, uint32_t value)
{
    os_test_rw_chain_runs++;

    if (value > 1U)
    {
        if (os_timer_submit(&os_test_rw_chain_pool, context, value - 1U) != OS_ERR_NONE)
        {
            os_test_rw_chain_fails++;
        }
    }
}

/******************************************************************************************************/
static void test_rw_pool_cb(void *context, uint32_t value)
{
    if (context == &os_test_rw_marker_a)      { os_test_rw_pool_a_sum += value; }
    else if (context == &os_test_rw_marker_b) { os_test_rw_pool_b_sum += value; }
}

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
/******************************************************************************************************/
/** Blocks on a mutex - legal only because the callback runs on a task, not in the tick ISR. */
static void test_rw_block_cb(void *context, uint32_t value)
{
    (void)context;
    (void)value;

    if (os_mutex_lock(&os_test_rw_mutex, OS_WAIT_FOREVER) == OS_ERR_NONE)
    {
        os_test_rw_block_done = true;
        (void)os_mutex_unlock(&os_test_rw_mutex);
    }
}
#endif

/******************************************************************************************************/
static void test_timer_real_world(void)
{
    uint32_t index;
    uint32_t start_tick;
    uint32_t accepted;
    uint32_t refused;
    bool     ok;

    test_print_section("Timers: real-project situations");

    /* ---- a one-shot that re-arms itself from inside its own callback ---- */
    os_test_rw_rearm_runs = 0U;
    (void)os_timer_start(&os_test_rw_rearm, NULL, TEST_RW_CHAIN_TARGET);
    os_delay_ms((TEST_RW_CHAIN_TARGET * 20U) + 80U);
    AHURA_TEST_CHECK(os_test_rw_rearm_runs == TEST_RW_CHAIN_TARGET,
                      "a one-shot re-armed from inside its own callback ran %u times (%lu)",
                      (unsigned)TEST_RW_CHAIN_TARGET, (unsigned long)os_test_rw_rearm_runs);

    /* ---- a periodic timer that stops itself ---- */
    os_test_rw_selfstop_runs = 0U;
    (void)os_timer_start(&os_test_rw_selfstop, NULL, 4U);
    os_delay_ms(220U);
    AHURA_TEST_CHECK(os_test_rw_selfstop_runs == 4U,
                      "a periodic timer that stops itself ran exactly 4 times (%lu)",
                      (unsigned long)os_test_rw_selfstop_runs);

    /* ---- work that outruns its own period coalesces instead of piling up ---- */
    /* A 10 ms period whose callback takes 40 ms. Over 240 ms a backlog would show as ~24 runs and
     * would keep running long after the stop; coalescing gives roughly 240/40 and stops promptly.
     * This is the overload case every periodic-timer bug report is really about. */
    os_test_rw_slow_runs = 0U;
    (void)os_timer_start(&os_test_rw_slow, NULL, 0U);
    os_delay_ms(240U);
    (void)os_timer_stop(&os_test_rw_slow);
    index = os_test_rw_slow_runs;
    os_delay_ms(120U);
    AHURA_TEST_CHECK((os_test_rw_slow_runs <= (index + 1U)) && (os_test_rw_slow_runs <= 9U),
                      "a callback slower than its period coalesces, no backlog (%lu runs, %lu after stop)",
                      (unsigned long)index, (unsigned long)os_test_rw_slow_runs);

    /* ---- stop discards an expiry the tick already queued; pause keeps it ---- */
    /* The kernel lock keeps the timer TASK from delivering while the tick ISR - which the lock does
     * not mask - goes on counting. That is the only way to hold an expiry in the queued state long
     * enough to act on it, and it is exactly the race a real cancel hits. */
    os_test_rw_owed_runs = 0U;
    (void)os_timer_start(&os_test_rw_owed, NULL, 0U);
    os_kernel_lock();
    start_tick = os_tick_get();
    while ((os_tick_get() - start_tick) < 40U) { }
    (void)os_timer_stop(&os_test_rw_owed);
    os_kernel_unlock();
    os_delay_ms(40U);
    AHURA_TEST_CHECK(os_test_rw_owed_runs == 0U,
                      "os_timer_stop discards an expiry the tick had already queued (%lu ran)",
                      (unsigned long)os_test_rw_owed_runs);

    os_test_rw_owed_runs = 0U;
    (void)os_timer_start(&os_test_rw_owed, NULL, 0U);
    os_kernel_lock();
    start_tick = os_tick_get();
    while ((os_tick_get() - start_tick) < 40U) { }
    (void)os_timer_pause(&os_test_rw_owed);
    os_kernel_unlock();
    os_delay_ms(40U);
    AHURA_TEST_CHECK(os_test_rw_owed_runs == 1U,
                      "but os_timer_pause still owes it, and it runs (%lu)",
                      (unsigned long)os_test_rw_owed_runs);
    (void)os_timer_stop(&os_test_rw_owed);

    /* ---- several timers sharing one period all expire on the same tick ---- */
    for (index = 0U; index < TEST_RW_SAME_PERIOD; index++)
    {
        os_test_rw_same_seen[index] = 0U;
        (void)os_timer_start(os_test_rw_same[index], NULL, index);
    }
    os_delay_ms(100U);
    for (index = 0U; index < TEST_RW_SAME_PERIOD; index++)
    {
        (void)os_timer_stop(os_test_rw_same[index]);
    }
    ok = true;
    for (index = 0U; index < TEST_RW_SAME_PERIOD; index++)
    {
        if ((os_test_rw_same_seen[index] < 2U) || (os_test_rw_same_seen[index] > 4U)) { ok = false; }
    }
    AHURA_TEST_CHECK(ok, "%u timers on the same period all fired, none starved (%lu/%lu/%lu/%lu)",
                      (unsigned)TEST_RW_SAME_PERIOD,
                      (unsigned long)os_test_rw_same_seen[0], (unsigned long)os_test_rw_same_seen[1],
                      (unsigned long)os_test_rw_same_seen[2], (unsigned long)os_test_rw_same_seen[3]);

    /* ---- a timer that retunes its own period from inside its callback ---- */
    os_test_rw_retune_runs = 0U;
    (void)os_timer_period_set(&os_test_rw_retune, 40U);
    (void)os_timer_start(&os_test_rw_retune, NULL, 0U);
    os_delay_ms(160U);
    (void)os_timer_stop(&os_test_rw_retune);
    AHURA_TEST_CHECK(os_test_rw_retune_runs >= 5U,
                      "a timer retuned from inside its own callback speeds up (%lu runs in 160 ms)",
                      (unsigned long)os_test_rw_retune_runs);

    /* ---- an interrupt burst deeper than the pool that feeds it ---- */
    /* Everything the pool can take is accepted and runs; the excess is REFUSED rather than dropped
     * silently, which is what lets an application count its own overruns. */
    os_test_rw_pool_a_sum = 0U;
    accepted              = 0U;
    refused               = 0U;

    os_kernel_lock();
    for (index = 1U; index <= 6U; index++)
    {
        if (os_timer_submit(&os_test_rw_pool_a, &os_test_rw_marker_a, index) == OS_ERR_NONE)
        {
            accepted += index;
        }
        else
        {
            refused++;
        }
    }
    os_kernel_unlock();
    os_delay_ms(60U);
    AHURA_TEST_CHECK((refused == 3U) && (os_test_rw_pool_a_sum == accepted),
                      "a burst of 6 into a depth-3 pool: 3 refused, the rest all ran (sum %lu of %lu)",
                      (unsigned long)os_test_rw_pool_a_sum, (unsigned long)accepted);

    /* ---- and the pool is usable again immediately afterwards ---- */
    os_test_rw_pool_a_sum = 0U;
    AHURA_TEST_CHECK(os_timer_submit(&os_test_rw_pool_a, &os_test_rw_marker_a, 7U) == OS_ERR_NONE,
                      "the overrun left the pool healthy");
    os_delay_ms(30U);
    AHURA_TEST_CHECK(os_test_rw_pool_a_sum == 7U, "and that call ran with its own value");

    /* ---- two pools sharing one callback keep their values apart ---- */
    os_test_rw_pool_a_sum = 0U;
    os_test_rw_pool_b_sum = 0U;
    os_kernel_lock();
    (void)os_timer_submit(&os_test_rw_pool_a, &os_test_rw_marker_a, 10U);
    (void)os_timer_submit(&os_test_rw_pool_b, &os_test_rw_marker_b, 20U);
    (void)os_timer_submit(&os_test_rw_pool_a, &os_test_rw_marker_a, 30U);
    os_kernel_unlock();
    os_delay_ms(40U);
    AHURA_TEST_CHECK((os_test_rw_pool_a_sum == 40U) && (os_test_rw_pool_b_sum == 20U),
                      "two pools on one callback do not cross (a=%lu b=%lu)",
                      (unsigned long)os_test_rw_pool_a_sum, (unsigned long)os_test_rw_pool_b_sum);

    /* ---- deferring more work from inside a deferred call ---- */
    os_test_rw_chain_runs  = 0U;
    os_test_rw_chain_fails = 0U;
    (void)os_timer_start(&os_test_rw_chain, &os_test_rw_marker_a, 3U);
    os_delay_ms(120U);
    AHURA_TEST_CHECK((os_test_rw_chain_runs == 3U) && (os_test_rw_chain_fails == 0U),
                      "a callback re-submits on a depth-1 pool, so its entry was already free "
                      "(%lu ran, %lu refused)",
                      (unsigned long)os_test_rw_chain_runs, (unsigned long)os_test_rw_chain_fails);

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    /* ---- a callback that BLOCKS, which is only legal because it runs on a task ---- */
    /* Held by this task when the timer fires, so the callback must wait for it. If callbacks ran in
     * the tick ISR this would deadlock the system instead of simply waiting. */
    if (os_mutex_init(&os_test_rw_mutex) == OS_ERR_NONE)
    {
        os_test_rw_block_done = false;
        (void)os_mutex_lock(&os_test_rw_mutex, OS_WAIT_FOREVER);
        (void)os_timer_start(&os_test_rw_block, NULL, 0U);
        os_delay_ms(60U);
        AHURA_TEST_CHECK(!os_test_rw_block_done,
                          "a timer callback blocks on a held mutex rather than spinning or failing");
        (void)os_mutex_unlock(&os_test_rw_mutex);
        os_delay_ms(40U);
        AHURA_TEST_CHECK(os_test_rw_block_done,
                          "and completes once the mutex is released - proof it runs on a task");
    }
#endif
}
#endif /* OS_CONFIG_TIMER_ENABLE */

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
    os_err_t status;

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
    os_err_t status;

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
    os_err_t status;
    os_task_t stale_task;
    uint32_t  t0;
    uint32_t  t1;

    test_print_section("Task Notifications");

    /* NULL means the CALLING task, so this arms this task's own mailbox rather than being
     * rejected. Consuming it again with os_notify_wait is what proves it went to the right
     * task: any other target would leave this one empty. */
    {
        uint32_t self_value = 0U;

        AHURA_TEST_CHECK(os_notify_give(NULL, 0xC0DEU) == OS_ERR_NONE,
                          "os_notify_give(NULL) targets the calling task");
        AHURA_TEST_CHECK(os_notify_wait(OS_WAIT_NOTHING, &self_value) == OS_ERR_NONE,
                          "and the value is waiting in this task's own mailbox");
        AHURA_TEST_CHECK(self_value == 0xC0DEU, "with the value intact (0x%lX)",
                          (unsigned long)self_value);
        AHURA_TEST_CHECK(os_notify_wait(OS_WAIT_NOTHING, NULL) == OS_ERR_EMPTY,
                          "and nothing left behind after it was consumed");
    }

    stale_task.id = 0xFFFFFFF0U;
    AHURA_TEST_CHECK(os_notify_give(&stale_task, 1U) == OS_ERR_INVALID_ARG,
                      "os_notify_give() to a stale/unknown task id is rejected");

    /* Give-before-wait: the latched value must be delivered without blocking. */
    os_test_notify_wait_timeout_ms = 500U;
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_notify_wait_entry, NULL, 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "give-before-wait helper created");
    AHURA_TEST_CHECK(os_notify_give(&helper, 111U) == OS_ERR_NONE,
                      "os_notify_give() to a created-but-not-started task succeeds");
    t0 = os_tick_get();
    AHURA_TEST_CHECK(os_task_start(&helper) == OS_ERR_NONE, "give-before-wait helper started");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 200U), "give-before-wait helper finished");
    t1 = os_tick_get();
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_ERR_NONE, "the latched value was delivered without blocking");
    AHURA_TEST_CHECK(os_test_notify_wait_value == 111U, "the delivered value matches (got %lu)",
                      (unsigned long)os_test_notify_wait_value);
    AHURA_TEST_CHECK((t1 - t0) < OS_TICKS_FROM_MS(100U), "delivery was immediate (elapsed=%lu ticks)",
                      (unsigned long)(t1 - t0));

    /* Wait-then-give: blocks, then wakes promptly once given. */
    os_test_notify_wait_timeout_ms = 500U;
    status = os_task_create(&worker, TEST_TASK_CONFIG(test_notify_wait_entry, NULL, 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "wait-then-give helper created");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "wait-then-give helper started");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_task_state_get(&worker) == OS_TASK_STATE_BLOCKED,
                      "wait-then-give helper is blocked in os_notify_wait");
    AHURA_TEST_CHECK(os_notify_give(&worker, 222U) == OS_ERR_NONE, "os_notify_give() wakes it");
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 200U), "wait-then-give helper finished");
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_ERR_NONE, "the wait reports delivery, not timeout");
    AHURA_TEST_CHECK(os_test_notify_wait_value == 222U, "the delivered value matches (got %lu)",
                      (unsigned long)os_test_notify_wait_value);
    AHURA_TEST_CHECK(os_test_notify_wait_ticks < OS_TICKS_FROM_MS(200U),
                      "the wake was prompt, well under the 500ms budget (elapsed=%lu ticks)",
                      (unsigned long)os_test_notify_wait_ticks);

    /* Timeout: nobody gives. */
    os_test_notify_wait_timeout_ms = 200U;
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_notify_wait_entry, NULL, 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "timeout-case helper created");
    AHURA_TEST_CHECK(os_task_start(&helper) == OS_ERR_NONE, "timeout-case helper started");
    AHURA_TEST_CHECK(test_wait_inactive(&helper, 400U), "timeout-case helper finished");
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_ERR_TIMEOUT,
                      "os_notify_wait() times out when nobody gives");
    AHURA_TEST_CHECK(os_test_notify_wait_ticks >= OS_TICKS_FROM_MS(200U),
                      "the timeout waited its full budget (elapsed=%lu ticks)",
                      (unsigned long)os_test_notify_wait_ticks);

    /* give() during an unrelated block must not cut it short, and must not be lost. */
    status = os_task_create(&worker, TEST_TASK_CONFIG(test_notify_unrelated_block_entry, NULL, 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "unrelated-block helper created");
    t0 = os_tick_get();
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "unrelated-block helper started (delaying 80ms)");
    os_delay_ms(20U);
    AHURA_TEST_CHECK(os_notify_give(&worker, 333U) == OS_ERR_NONE,
                      "os_notify_give() during the unrelated delay succeeds");
    AHURA_TEST_CHECK(test_wait_inactive(&worker, 300U), "unrelated-block helper finished");
    t1 = os_tick_get();
    AHURA_TEST_CHECK((t1 - t0) >= OS_TICKS_FROM_MS(75U),
                      "the unrelated delay was not cut short by the give() (elapsed=%lu ticks)",
                      (unsigned long)(t1 - t0));
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_ERR_NONE,
                      "the latched value was not lost - picked up by the later non-blocking wait");
    AHURA_TEST_CHECK(os_test_notify_wait_value == 333U, "the delivered value matches (got %lu)",
                      (unsigned long)os_test_notify_wait_value);

    /* value_out = NULL: wait for the signal, discard the value, still consume the delivery. */
    (void)os_task_create(&helper, TEST_TASK_CONFIG(test_notify_discard_entry, NULL, 3U));
    (void)os_task_start(&helper);
    os_delay_ms(20U);
    (void)os_notify_give(&helper, 444U);
    (void)test_wait_inactive(&helper, 200U);
    AHURA_TEST_CHECK(os_test_notify_wait_status == OS_ERR_NONE, "notify_wait(NULL) reports the delivery");
    AHURA_TEST_CHECK(os_test_notify_second_status == OS_ERR_EMPTY, "and still consumed it");
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

    AHURA_TEST_CHECK(os_task_stack_watermark_get(NULL, &min_free) == OS_ERR_NONE,
                      "os_task_stack_watermark_get(NULL) reports the calling task");
    AHURA_TEST_CHECK(min_free < OS_CONFIG_TEST_STACK_SIZE,
                      "watermark (%lu) is less than the full stack (%lu bytes)",
                      (unsigned long)min_free, (unsigned long)OS_CONFIG_TEST_STACK_SIZE);

    AHURA_TEST_CHECK(os_task_stack_watermark_get(NULL, NULL) == OS_ERR_INVALID_ARG,
                      "a NULL output pointer is rejected");

#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
    /* The guard word the overflow check reads on every switch-out. Detection itself cannot be
     * tested here - tripping it parks the core by design - so this only proves the guard is in
     * place and that a healthy task has not disturbed it. */
    AHURA_TEST_CHECK(*(const uint32_t *)(const void *)worker_stack_buf == 0xA5A5A5A5UL,
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
    os_err_t status;

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
    status = os_task_create(&worker, TEST_TASK_CONFIG(test_busy_spin_entry, NULL, TEST_PRIO_LOW));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "busy worker task created to load the CPU (priority 1)");
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "busy worker task started");

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
        os_err_t status;
        bool      done;

        status = os_queue_receive(&os_test_queue, &value, 300U);
        if (status != OS_ERR_NONE)
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
    os_err_t status;
    uint32_t  expected_total = 0U;
    uint32_t  i;

    test_print_section("Combined: Queue + Mutex, 2 producers + 2 consumers");

    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_queue) == OS_ERR_NONE,
                      "pipeline queue emptied and reused (capacity %u, %u items will be produced)",
                      (unsigned)os_test_queue.capacity, (unsigned)TEST_PIPELINE_TOTAL_ITEMS);
    AHURA_TEST_CHECK(os_mutex_init(&os_test_pipeline_mutex) == OS_ERR_NONE, "pipeline mutex initialized");

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
    status = os_task_create(&helper2, TEST_TASK_CONFIG(test_pipeline_consumer_entry, NULL, 4U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "consumer task 1 created (priority 4)");
    status = os_task_create(&helper3, TEST_TASK_CONFIG(test_pipeline_consumer_entry, NULL, 4U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "consumer task 2 created (priority 4)");
    status = os_task_create(&worker, TEST_TASK_CONFIG(test_pipeline_producer_entry, &os_test_producer_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "producer task 1 created (priority 3, values 0-5)");
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_pipeline_producer_entry, &os_test_producer_ctx[1], 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "producer task 2 created (priority 3, values 100-105)");

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
    os_err_t status;

    test_print_section("Combined: Mutex + Priority, ordered contention across 3 tasks");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_prio_mutex) == OS_ERR_NONE, "priority-contention mutex initialized");
    AHURA_TEST_CHECK(os_mutex_lock(&os_test_prio_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "test task takes the mutex first, so all 3 waiters below must block");

    os_test_prio_order_count         = 0U;
    os_test_prio_ctx[0].priority_tag = 4U;
    os_test_prio_ctx[1].priority_tag = 5U;
    os_test_prio_ctx[2].priority_tag = 6U;

    status = os_task_create(&worker, TEST_TASK_CONFIG(test_prio_waiter_entry, &os_test_prio_ctx[0], 4U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "low-priority waiter created (priority 4)");
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_prio_waiter_entry, &os_test_prio_ctx[1], 5U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "medium-priority waiter created (priority 5)");
    status = os_task_create(&helper2, TEST_TASK_CONFIG(test_prio_waiter_entry, &os_test_prio_ctx[2], 6U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "high-priority waiter created (priority 6)");

    /* Start low first, high last: if the wake order below still comes out high-to-low, that
     * proves it is driven by priority, not by creation/start order. */
    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_task_start(&helper2);

    os_delay_ms(30U); /* let all 3 reach os_mutex_lock() and join the waiter list */

    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_prio_mutex) == OS_ERR_NONE,
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
    os_err_t status;

    test_print_section("Combined: Mutex Priority Inheritance");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_inherit_mutex) == OS_ERR_NONE, "priority-inheritance mutex initialized");
    AHURA_TEST_CHECK(os_mutex_lock(&os_test_inherit_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "test task takes the mutex first (at its own priority %u)",
                      (unsigned)OS_CONFIG_TEST_PRIORITY);

    os_test_inherit_high_done      = false;
    os_test_inherit_medium_counter = 0U;

    /* Higher priority than this test task: preempts immediately, finds the mutex locked, and
     * boosts this test task's effective priority before blocking - synchronously, inside this
     * os_task_start() call, so the test task resumes already boosted. */
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_inherit_high_entry, NULL,
                                                            OS_CONFIG_TEST_PRIORITY + 2U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "high-priority waiter created (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 2U));
    AHURA_TEST_CHECK(os_task_start(&helper) == OS_ERR_NONE, "high-priority waiter started");

    AHURA_TEST_CHECK(!os_test_inherit_high_done,
                      "the high-priority waiter blocked on the held mutex instead of finishing");

    /* A medium-priority task, created and started while this test task is (boosted) running: it
     * must not get any CPU time yet - proving the boost, not just "it'll run eventually". */
    status = os_task_create(&worker, TEST_TASK_CONFIG(test_inherit_medium_entry, NULL,
                                                             OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "medium-priority task created (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "medium-priority task started");

    AHURA_TEST_CHECK(os_test_inherit_medium_counter == 0U,
                      "medium-priority task got zero CPU time while the boosted owner held the mutex (count=%lu)",
                      (unsigned long)os_test_inherit_medium_counter);

    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_inherit_mutex) == OS_ERR_NONE,
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
    os_err_t status;

    test_print_section("Combined: Mutex Priority Inheritance across TWO held mutexes");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_inherit2_mutex_a) == OS_ERR_NONE, "mutex A initialized");
    AHURA_TEST_CHECK(os_mutex_init(&os_test_inherit2_mutex_b) == OS_ERR_NONE, "mutex B initialized");

    AHURA_TEST_CHECK(os_mutex_lock(&os_test_inherit2_mutex_a, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "test task takes mutex A (at its own priority %u)", (unsigned)OS_CONFIG_TEST_PRIORITY);
    AHURA_TEST_CHECK(os_mutex_lock(&os_test_inherit2_mutex_b, OS_WAIT_NOTHING) == OS_ERR_NONE,
                      "test task takes mutex B as well - two mutexes held at once");

    os_test_inherit2_done_mask     = 0U;
    os_test_inherit_medium_counter = 0U;

    os_test_inherit2_ctx[0].mutex = &os_test_inherit2_mutex_a;
    os_test_inherit2_ctx[0].tag   = 1U;
    os_test_inherit2_ctx[1].mutex = &os_test_inherit2_mutex_b;
    os_test_inherit2_ctx[1].tag   = 2U;

    /* HIGH blocks on A: boosts the owner to +2 (synchronously, inside os_task_start). */
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_inherit2_waiter_entry, &os_test_inherit2_ctx[0],
                                                            OS_CONFIG_TEST_PRIORITY + 2U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "waiter HIGH created for mutex A (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 2U));
    AHURA_TEST_CHECK(os_task_start(&helper) == OS_ERR_NONE, "waiter HIGH started");

    /* HIGHER blocks on B: boosts the owner again, to +3. */
    status = os_task_create(&helper2, TEST_TASK_CONFIG(test_inherit2_waiter_entry, &os_test_inherit2_ctx[1],
                                                             OS_CONFIG_TEST_PRIORITY + 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "waiter HIGHER created for mutex B (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 3U));
    AHURA_TEST_CHECK(os_task_start(&helper2) == OS_ERR_NONE, "waiter HIGHER started");

    AHURA_TEST_CHECK(os_test_inherit2_done_mask == 0U,
                      "both waiters blocked on the held mutexes instead of finishing (mask=%lu)",
                      (unsigned long)os_test_inherit2_done_mask);

    /* Medium (+1) must stay starved for as long as ANY boost is in effect. */
    status = os_task_create(&worker, TEST_TASK_CONFIG(test_inherit_medium_entry, NULL,
                                                             OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "medium-priority task created (priority %u)",
                      (unsigned)(OS_CONFIG_TEST_PRIORITY + 1U));
    AHURA_TEST_CHECK(os_task_start(&worker) == OS_ERR_NONE, "medium-priority task started");
    AHURA_TEST_CHECK(os_test_inherit_medium_counter == 0U,
                      "medium task got no CPU while the owner is boosted to +3 (count=%lu)",
                      (unsigned long)os_test_inherit_medium_counter);

    /* Release B only. HIGHER wakes, takes B and finishes; the owner must settle at +2 (still
     * owed to A's waiter), so medium STILL must not run. */
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_inherit2_mutex_b) == OS_ERR_NONE, "test task releases mutex B");
    AHURA_TEST_CHECK(test_wait_inactive(&helper2, 300U), "waiter HIGHER finished after B was released");
    AHURA_TEST_CHECK((os_test_inherit2_done_mask & 2U) != 0U, "waiter HIGHER actually acquired mutex B");

    AHURA_TEST_CHECK((os_test_inherit2_done_mask & 1U) == 0U,
                      "waiter HIGH is still blocked - mutex A was never released");
    AHURA_TEST_CHECK(os_test_inherit_medium_counter == 0U,
                      "THE KEY CHECK: releasing B kept the boost A's waiter is still owed, so the "
                      "medium task still got zero CPU (count=%lu)",
                      (unsigned long)os_test_inherit_medium_counter);

    /* Release A: no held mutex left, so the owner finally drops to base and medium is free. */
    AHURA_TEST_CHECK(os_mutex_unlock(&os_test_inherit2_mutex_a) == OS_ERR_NONE,
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
    os_err_t status;
    uint32_t  received[3] = { 0 };
    uint32_t  i;
    uint32_t  sum          = 0U;
    uint32_t  expected_sum;
    bool      saw[3]       = { false, false, false };

    test_print_section("Combined: Events + Queue, fan-out/fan-in across 3 tasks");

    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_queue) == OS_ERR_NONE,
                      "fan-in queue emptied and reused (capacity %u, one slot per worker)",
                      (unsigned)os_test_queue.capacity);
    AHURA_TEST_CHECK(os_event_init(&os_test_event) == OS_ERR_NONE, "fan-in event initialized");

    os_test_fanin_ctx[0].bit = 0x01U; os_test_fanin_ctx[0].value = 10U; os_test_fanin_ctx[0].work_ms = 60U;
    os_test_fanin_ctx[1].bit = 0x02U; os_test_fanin_ctx[1].value = 20U; os_test_fanin_ctx[1].work_ms = 20U;
    os_test_fanin_ctx[2].bit = 0x04U; os_test_fanin_ctx[2].value = 30U; os_test_fanin_ctx[2].work_ms = 40U;
    expected_sum = os_test_fanin_ctx[0].value + os_test_fanin_ctx[1].value + os_test_fanin_ctx[2].value;

    status = os_task_create(&worker, TEST_TASK_CONFIG(test_fanin_worker_entry, &os_test_fanin_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "fan-in worker 1 created (bit 0x01, 60 ms work)");
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_fanin_worker_entry, &os_test_fanin_ctx[1], 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "fan-in worker 2 created (bit 0x02, 20 ms work)");
    status = os_task_create(&helper2, TEST_TASK_CONFIG(test_fanin_worker_entry, &os_test_fanin_ctx[2], 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "fan-in worker 3 created (bit 0x04, 40 ms work)");

    (void)os_task_start(&worker);
    (void)os_task_start(&helper);
    (void)os_task_start(&helper2);

    status = os_event_wait_bits(&os_test_event, 0x07U, true, false, &matched, 500U);
    AHURA_TEST_CHECK((status == OS_ERR_NONE) && (matched == 0x07U),
                      "wait-all sees all 3 workers' bits despite different finish times (matched=0x%02lx)",
                      (unsigned long)matched);

    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 3U, "queue holds exactly the 3 workers' results");

    for (i = 0U; i < 3U; i++)
    {
        AHURA_TEST_CHECK(os_queue_receive(&os_test_queue, &received[i], OS_WAIT_NOTHING) == OS_ERR_NONE,
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

#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEM_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
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
                if (os_mutex_lock(&os_test_stress_mutex, 20U) == OS_ERR_NONE)
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
                if (os_sem_take(&os_test_stress_sem, 5U) == OS_ERR_NONE)
                {
                    os_delay_ms(test_stress_prng_next(&ctx->prng_state) % 3U);
                    (void)os_sem_give(&os_test_stress_sem);
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
                else if (os_queue_receive(&os_test_stress_queue, &received, 5U) == OS_ERR_NONE)
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
    os_err_t status;

    test_print_section("Stress/Soak: 4 tasks contend on mutex+semaphore+queue+event+heap at once");

    AHURA_TEST_CHECK(os_mutex_init(&os_test_stress_mutex) == OS_ERR_NONE, "stress mutex initialized");
    AHURA_TEST_CHECK(os_sem_init(&os_test_stress_sem, OS_TEST_STRESS_SEM_MAX, OS_TEST_STRESS_SEM_MAX) == OS_ERR_NONE,
                      "stress semaphore initialized (max=%u, deliberately < %u workers)",
                      (unsigned)OS_TEST_STRESS_SEM_MAX, (unsigned)OS_TEST_STRESS_WORKER_COUNT);
    AHURA_TEST_CHECK(os_event_init(&os_test_stress_event) == OS_ERR_NONE, "stress event initialized");
    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_stress_queue) == OS_ERR_NONE,
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

    status = os_task_create(&worker, TEST_TASK_CONFIG(test_stress_worker_entry, &os_test_stress_ctx[0], 3U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "stress worker 0 created (priority 3)");
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_stress_worker_entry, &os_test_stress_ctx[1], 4U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "stress worker 1 created (priority 4)");
    status = os_task_create(&helper2, TEST_TASK_CONFIG(test_stress_worker_entry, &os_test_stress_ctx[2], 5U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "stress worker 2 created (priority 5)");
    status = os_task_create(&helper3, TEST_TASK_CONFIG(test_stress_worker_entry, &os_test_stress_ctx[3], 6U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "stress worker 3 created (priority 6)");

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

    while (os_sem_take(&os_test_stress_sem, OS_WAIT_NOTHING) == OS_ERR_NONE)
    {
        drained_tokens++;
    }
    AHURA_TEST_CHECK(drained_tokens == OS_TEST_STRESS_SEM_MAX,
                      "every semaphore token was given back exactly once (drained %lu of %lu)",
                      (unsigned long)drained_tokens, (unsigned long)OS_TEST_STRESS_SEM_MAX);

    while (os_queue_receive(&os_test_stress_queue, &dummy, OS_WAIT_NOTHING) == OS_ERR_NONE)
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

    AHURA_TEST_CHECK(os_mutex_lock(&os_test_stress_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE, "stress mutex ended unlocked");
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
#endif /* OS_CONFIG_MUTEX_ENABLE && OS_CONFIG_SEM_ENABLE && OS_CONFIG_QUEUE_ENABLE && OS_CONFIG_EVENT_ENABLE && OS_CONFIG_ALLOC_ENABLE */

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
    os_err_t status;

    test_print_section("Stress: rapid task create/start/exit churn");

    os_test_churn_counter = 0U;

    for (i = 0U; i < OS_TEST_CHURN_ITERATIONS; i++)
    {
        status = os_task_create(&worker, TEST_TASK_CONFIG(test_churn_worker_entry, NULL, 1U));
        if (status != OS_ERR_NONE)
        {
            all_created = false;
            break;
        }

        status = os_task_start(&worker);
        if (status != OS_ERR_NONE)
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

        if (os_task_stack_watermark_get(&worker, &min_free) == OS_ERR_NONE)
        {
            AHURA_TEST_CHECK(min_free <= sizeof(worker_stack_buf),
                              "repeated slot reuse leaves a sane stack watermark (%lu / %lu bytes free)",
                              (unsigned long)min_free, (unsigned long)sizeof(worker_stack_buf));
        }
    }
#endif
}

#if (OS_CONFIG_TIMER_ENABLE == 1U)
#define OS_TEST_TIMER_CHURN_ITERATIONS 500U

static __IO uint32_t os_test_churn_timer_fired = 0U;

/******************************************************************************************************/
static void test_churn_timer_cb(void *context, uint32_t value)
{
    (void)value;
    (void)context;
    os_test_churn_timer_fired++;
}

/******************************************************************************************************/
/**
 * @brief Hammers os_timer_start()/os_timer_stop() on the same timer object back-
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
        if (os_timer_period_set(&os_test_churn_timer, 1000U) != OS_ERR_NONE)
        {
            all_ok = false;
            break;
        }

        if (os_timer_start(&os_test_churn_timer, NULL, 0U) != OS_ERR_NONE)
        {
            all_ok = false;
            break;
        }

        if (os_timer_stop(&os_test_churn_timer) != OS_ERR_NONE)
        {
            all_ok = false;
            break;
        }
    }

    AHURA_TEST_CHECK(all_ok, "timer init/start/stop succeeds on every one of %u rapid churn cycles",
                      (unsigned)OS_TEST_TIMER_CHURN_ITERATIONS);
    AHURA_TEST_CHECK(os_test_churn_timer_fired == 0U, "none of the stopped-before-expiry timers fired (fired=%lu)",
                      (unsigned long)os_test_churn_timer_fired);

    AHURA_TEST_CHECK(os_timer_period_set(&os_test_churn_timer, 30U) == OS_ERR_NONE,
                      "timer re-armed for a real run after the churn");
    AHURA_TEST_CHECK(os_timer_start(&os_test_churn_timer, NULL, 0U) == OS_ERR_NONE, "timer starts normally after the churn");
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
 * Defined out here, ahead of the OS_TEST_STRESS_EXTENDED block below, because BOTH the extended
 * timer-flood stress test (inside that block, so compiled out in unoptimized builds) and
 * test_regressions() (which runs in every build) fill the registry with them. Declaring them
 * inside the block made the whole suite fail to compile at -O0 - precisely the build a board
 * bring-up uses.
 *
 * The CALLBACK has to come out with them, not just a forward declaration of it. An
 * OS_TIMER_DEFINE_PERIODIC stores the function pointer in the timer object itself, so every one of
 * these definitions is a use: leaving the body inside the block satisfied the compiler and then
 * failed at link with "undefined reference to test_tflood_cb" in exactly the -O0 build this comment
 * was written to protect. The counter it writes moves with it for the same reason. */
static __IO uint32_t os_test_tflood_fired[TEST_TIMER_SET];

/******************************************************************************************************/
static void test_tflood_cb(void *context, uint32_t value)
{
    (void)context;

    /* value is what os_timer_start was given, so one shared definition still knows which of the
     * array's timers fired. */
    if (value < TEST_TIMER_SET) { os_test_tflood_fired[value]++; }
}

OS_TIMER_DEFINE_PERIODIC(os_test_tf0, 10U, test_tflood_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_tf1, 15U, test_tflood_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_tf2, 20U, test_tflood_cb);
OS_TIMER_DEFINE_PERIODIC(os_test_tf3, 25U, test_tflood_cb);

static os_timer_t *os_test_tflood[TEST_TIMER_SET] = {
    &os_test_tf0,
    &os_test_tf1,
    &os_test_tf2,
    &os_test_tf3,
};

OS_TIMER_DEFINE_PERIODIC(os_test_tflood_extra, 10U, test_tflood_cb);
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
         * This used to need a switch mapping the index back to the matching *_stack_buf symbol. */
        os_task_config_t config;

        config.entry         = entry;
        config.context       = (void *)((uint8_t *)contexts + (i * context_size));
        config.priority      = 3U + i;
        config.core_affinity = OS_TASK_CORE_ANY;

        if (os_task_create(os_test_stress_tasks[i], &config) != OS_ERR_NONE) { break; }
        if (os_task_start(os_test_stress_tasks[i]) != OS_ERR_NONE)           { break; }

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
        if (os_queue_init_dynamic(&q, item_size, capacity) != OS_ERR_NONE)
        {
            all_ok = false;
            break;
        }

        free_now = os_mem_free_get();
        if (free_now < worst_free) { worst_free = free_now; }

        sent[0] = 0xA5A50000UL | (i & 0xFFFFU);
        sent[1] = i;
        sent[2] = ~i;

        if (os_queue_send(&q, sent, OS_WAIT_NOTHING) != OS_ERR_NONE)
        {
            all_ok = false;
        }
        else if (os_queue_receive(&q, got, OS_WAIT_NOTHING) != OS_ERR_NONE)
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

        if (os_queue_cleanup(&q) != OS_ERR_NONE) { all_ok = false; }

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

        if (os_queue_send(&os_test_qprod_queue, &tag, 500U) == OS_ERR_NONE)
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

    AHURA_TEST_CHECK(os_queue_init_dynamic(&os_test_qprod_queue, sizeof(uint32_t), 2U) == OS_ERR_NONE,
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

        if (os_queue_receive(&os_test_qprod_queue, &tag, 500U) != OS_ERR_NONE) { break; }

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
    AHURA_TEST_CHECK(os_queue_cleanup(&os_test_qprod_queue) == OS_ERR_NONE, "the dynamic queue tears down cleanly");
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

#if (OS_CONFIG_SEM_ENABLE == 1U)

#define OS_TEST_PINGPONG_ROUNDS 1000U

static os_sem_t os_test_pp_ping;
static os_sem_t os_test_pp_pong;
static __IO uint32_t  os_test_pp_partner_rounds = 0U;

/******************************************************************************************************/
static void test_pp_entry(void *context)
{
    (void)context;

    while (os_test_pp_partner_rounds < OS_TEST_PINGPONG_ROUNDS)
    {
        if (os_sem_take(&os_test_pp_ping, 500U) != OS_ERR_NONE) { break; }

        os_test_pp_partner_rounds++;
        (void)os_sem_give(&os_test_pp_pong);
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

    AHURA_TEST_CHECK(os_sem_init(&os_test_pp_ping, 0U, 1U) == OS_ERR_NONE, "ping semaphore initialized (binary, empty)");
    AHURA_TEST_CHECK(os_sem_init(&os_test_pp_pong, 0U, 1U) == OS_ERR_NONE, "pong semaphore initialized (binary, empty)");

    if (os_task_create(&worker, TEST_TASK_CONFIG(test_pp_entry, NULL, TEST_PRIO_HIGH)) != OS_ERR_NONE)
    {
        printf("  [SKIP] could not create the ping-pong partner task\r\n");
        return;
    }
    (void)os_task_start(&worker);

    t0 = os_tick_get();

    for (i = 0U; i < OS_TEST_PINGPONG_ROUNDS; i++)
    {
        if (os_sem_give(&os_test_pp_ping) != OS_ERR_NONE)       { break; }
        if (os_sem_take(&os_test_pp_pong, 500U) != OS_ERR_NONE) { break; }

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
    AHURA_TEST_CHECK(os_sem_take(&os_test_pp_ping, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                      "no stray ping token was left behind");
    AHURA_TEST_CHECK(os_sem_take(&os_test_pp_pong, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                      "no stray pong token was left behind");

    printf("  [INFO] %lu blocking handoffs in %lu ms\r\n",
           (unsigned long)(2UL * OS_TEST_PINGPONG_ROUNDS), (unsigned long)elapsed);
}
#endif /* OS_CONFIG_SEM_ENABLE */

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

        if (os_notify_wait(50U, &value) == OS_ERR_NONE)
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

    if (os_task_create(&worker, TEST_TASK_CONFIG(test_ns_entry, NULL, TEST_PRIO_HIGH)) != OS_ERR_NONE)
    {
        printf("  [SKIP] could not create the notification waiter task\r\n");
        return;
    }
    (void)os_task_start(&worker);

    for (i = 1U; i <= OS_TEST_NOTIFY_STORM_COUNT; i++)
    {
        if (os_notify_give(&worker, i) != OS_ERR_NONE) { break; }

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

        if (os_event_wait_bits(&os_test_ebs_event, ctx->bit, false, true, &matched, 100U) == OS_ERR_NONE)
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

    AHURA_TEST_CHECK(os_event_init(&os_test_ebs_event) == OS_ERR_NONE, "bit-storm event initialized");

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

#if (OS_CONFIG_TIMER_ENABLE == 1U)

#define OS_TEST_TFLOOD_WINDOW 200U

/* os_test_tflood[], os_test_tflood_extra, test_tflood_cb and os_test_tflood_fired all live
 * further up, outside this OS_TEST_STRESS_EXTENDED block: test_regressions() fills the timer
 * registry with them too, and that test always runs. */

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
    uint32_t snapshot[TEST_TIMER_SET];
    uint32_t started      = 0U;
    bool     all_stopped  = true;
    bool     counts_ok    = true;
    bool     still_firing = false;
    uint32_t i;

    test_print_section("Stress: every timer slot armed periodically at once");

    for (i = 0U; i < TEST_TIMER_SET; i++)
    {
        uint32_t period_ms = 10U + (i * 5U);

        os_test_tflood_fired[i] = 0U;

        (void)os_timer_period_set(os_test_tflood[i], period_ms);

        /* The index IS the identity under the new API: it reaches test_tflood_cb as value,
         * which is how the per-timer counts below stay separate. */
        if (os_timer_start(os_test_tflood[i], NULL, i) == OS_ERR_NONE) { started++; }
    }

    AHURA_TEST_CHECK(started == TEST_TIMER_SET,
                      "all %u timer slots armed periodically (%lu started)",
                      (unsigned)TEST_TIMER_SET, (unsigned long)started);

    AHURA_TEST_CHECK(os_timer_start(&os_test_tflood_extra, NULL, TEST_TIMER_SET) == OS_ERR_NONE,
                      "and one more on top is accepted - there is no capacity to exceed");
    (void)os_timer_stop(&os_test_tflood_extra);

    os_delay_ms(OS_TEST_TFLOOD_WINDOW);

    for (i = 0U; i < TEST_TIMER_SET; i++)
    {
        if (os_timer_stop(os_test_tflood[i]) != OS_ERR_NONE) { all_stopped = false; }

        snapshot[i] = os_test_tflood_fired[i];
    }

    AHURA_TEST_CHECK(all_stopped, "every armed timer stopped cleanly");

    for (i = 0U; i < TEST_TIMER_SET; i++)
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

    for (i = 0U; i < TEST_TIMER_SET; i++)
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
        if (os_mutex_lock(&os_test_convoy_mutex, 1000U) == OS_ERR_NONE)
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

    AHURA_TEST_CHECK(os_mutex_init(&os_test_convoy_mutex) == OS_ERR_NONE, "convoy mutex initialized");

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
    AHURA_TEST_CHECK(os_mutex_lock(&os_test_convoy_mutex, OS_WAIT_NOTHING) == OS_ERR_NONE, "the mutex ended unlocked");
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
        os_err_t status;

        if (os_task_stack_watermark_get(NULL, &self_min_free) == OS_ERR_NONE)
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
        status = os_task_create(&worker, TEST_TASK_CONFIG(test_busy_spin_entry, NULL, TEST_PRIO_LOW));
        if (status == OS_ERR_NONE)
        {
            (void)os_task_start(&worker);
            os_delay_ms(20U);
            os_test_busy_should_run = false;

            if (os_task_stack_watermark_get(&worker, &worker_min_free) == OS_ERR_NONE)
            {
                printf("  [INFO] worker task peak stack usage: %lu / %lu bytes (%lu%% headroom left)\r\n",
                       (unsigned long)(sizeof(worker_stack_buf) - worker_min_free),
                       (unsigned long)sizeof(worker_stack_buf),
                       (unsigned long)((worker_min_free * 100U) / sizeof(worker_stack_buf)));
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
 * This one is a FUNCTIONAL check: that two equal-priority tasks really do hand the CPU back and
 * forth. Its microsecond figure is a tick-resolution average over thousands of switches and
 * includes the loop that drives them, so it is a sanity number rather than a specification.
 *
 * The cycle-accurate cost of a single task-to-task switch is in the BENCHMARKS table at the end of
 * the run, as "ONE context switch, task to task".
 */
static void test_context_switch_timing(void)
{
    uint32_t  t0;
    uint32_t  t1;
    uint32_t  window_ms;
    uint32_t  switches;
    os_err_t status;

    test_print_section("Context Switch Timing (informational, tick-resolution estimate)");

    os_test_switch_count      = 0U;
    os_test_switch_should_run = true;

    status = os_task_create(&worker, TEST_TASK_CONFIG(test_switch_ping_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "ping task created for the switch benchmark (priority 1)");
    status = os_task_create(&helper, TEST_TASK_CONFIG(test_switch_ping_entry, NULL, 1U));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "pong task created for the switch benchmark (priority 1)");

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

        printf("  [INFO] ~%lu switches in %lu ms -> ~%lu us/switch average, loop overhead included\r\n"
               "         (the BENCHMARKS table below measures one switch to the cycle)\r\n",
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
 * os_tickless_idle_process() is not yet invoked by the idle task (see doc/porting.md "Tickless
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

    printf("  [INFO] os_tickless_idle_process() is not invoked by the idle task yet (see\r\n"
           "         doc/porting.md \"Tickless idle\") - this only tests the two hooks in isolation.\r\n");

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
    os_err_t init_status;
    os_err_t start_status;

    test_print_section("Tickless Sleep (end-to-end, real hardware timing)");

    max_suppressed = os_tickless_max_suppressed_ticks_get();

    if (max_suppressed < 4U)
    {
        printf("  [SKIP] os_tickless_max_suppressed_ticks_get() = %lu: this port does not yet\r\n"
               "         suppress ticking for real (see doc/porting.md \"Tickless idle\"), or the\r\n"
               "         current clock/tick-rate combination allows too small a window to test.\r\n",
               (unsigned long)max_suppressed);
        return;
    }

    printf("  [INFO] calling os_tickless_idle_process() directly from this task (not the idle\r\n"
           "         task - see doc/porting.md \"Tickless idle\") to verify the suppress/measure\r\n"
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
    init_status  = os_timer_period_set(&os_test_timer_oneshot, horizon);
    start_status = os_timer_start(&os_test_timer_oneshot, NULL, 0U);

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

    AHURA_TEST_CHECK(init_status == OS_ERR_NONE, "the timer takes a %lu-tick horizon for the sleep test",
                      (unsigned long)horizon);
    AHURA_TEST_CHECK(start_status == OS_ERR_NONE, "one-shot timer started");
    AHURA_TEST_CHECK((delta >= tolerance_low) && (delta <= tolerance_high),
                      "os_tickless_idle_process() slept ~%lu ticks and measured it accurately (delta=%lu)",
                      (unsigned long)horizon, (unsigned long)delta);
    AHURA_TEST_CHECK(os_kernel_is_running(), "kernel state is intact after a real tickless sleep/wake cycle");

    os_delay_ms(5U); /* let the timer service task run the callback */
    AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "the timer bounding the sleep fired exactly once (fired=%lu)",
                      (unsigned long)os_test_oneshot_fired);

    (void)os_timer_stop(&os_test_churn_timer);
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
#if (OS_CONFIG_TIMER_ENABLE == 1U)
/**
 * @brief Never actually reached - the benchmark timer's period outlives the measurement.
 */
static void test_bench_timer_cb(void *context, uint32_t value)
{
    (void)context;
    (void)value;
}
#endif

/* Runs while the switch benchmark below is sampling: equal priority to the measuring task, so a
 * yield from either one rotates to the other. __IO because the two tasks take turns rather than
 * running together - without it the compiler is entitled to hoist the load out of the loop and the
 * partner would never see the stop. */
static __IO bool os_test_bench_partner_run = false;

/******************************************************************************************************/
/**
 * @brief Benchmark partner: hand the CPU straight back, until told to stop.
 *
 * The whole body is one yield on purpose. What the row measures is the switch, so anything else in
 * here would be counted as part of it.
 */
static void test_bench_partner_entry(void *context)
{
    (void)context;

    while (os_test_bench_partner_run)
    {
        os_task_yield();
    }
}

/******************************************************************************************************/
/**
 * @brief Print one benchmark row: the best and worst samples, each with its time in parentheses.
 *
 * Cycles first because that is what was actually counted; the nanoseconds beside it are the same
 * number divided by the CPU clock, and are what a deadline is written in. Both figures get one, so
 * neither column has to be converted in the reader's head.
 *
 * @param[in] name      Operation label; must fit the 40-column field or the row loses its
 *                      alignment, since printf pads a short string but never truncates a long one.
 * @param[in] best      Cheapest sample, measurement overhead already subtracted.
 * @param[in] worst     Dearest sample, the same overhead subtracted.
 * @param[in] clock_hz  CPU clock for the ns conversion, 0 when unknown.
 */
static void test_bench_row(const char *name, uint32_t best, uint32_t worst, uint32_t clock_hz)
{
    if (clock_hz != 0U)
    {
        /* 64-bit throughout: a slow core with a big cycle count would overflow 32 bits here. */
        uint64_t best_ns  = ((uint64_t)best  * 1000000000ULL) / (uint64_t)clock_hz;
        uint64_t worst_ns = ((uint64_t)worst * 1000000000ULL) / (uint64_t)clock_hz;

        printf("  %-40s %5lu (%6lu ns) %5lu (%6lu ns)\r\n", name,
               (unsigned long)best,  (unsigned long)best_ns,
               (unsigned long)worst, (unsigned long)worst_ns);
    }
    else
    {
        /* No clock to convert with - say so rather than printing a zero that reads as a time. */
        printf("  %-40s %5lu (   n/a   ) %5lu (   n/a   )\r\n", name,
               (unsigned long)best, (unsigned long)worst);
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
    uint32_t          worst;
    uint32_t          overhead;
    uint32_t          overhead_worst;
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
#elif defined(__riscv)
    /* RISC-V names its features individually rather than as a profile, so the line is assembled
     * from the extension macros the compiler defines - which is also exactly what the port
     * branches on, so this cannot disagree with the code that was built. */
    printf("RV%d", (int)__riscv_xlen);
    printf("I");
#if defined(__riscv_mul)
    printf("M");
#endif
#if defined(__riscv_atomic)
    printf("A");
#endif
#if defined(__riscv_compressed)
    printf("C");
#endif
#if defined(__riscv_zbb)
    printf(" +Zbb");
#endif
#else
    printf("unknown architecture");
#endif

    /* Floating point, asked the way each architecture answers it. */
#if defined(__ARM_FP) || (defined(__riscv) && defined(__riscv_flen))
    printf(", FPU");
#else
    printf(", no FPU");
#endif
#if defined(__ARM_FEATURE_MVE)
    printf(", MVE");
#elif defined(__ARM_FEATURE_DSP)
    printf(", DSP");
#endif

    /* TrustZone is an Arm feature. Guarded on the PORT capability rather than only on the config
     * value, because a port that does not define the OS_CONFIG_TRUSTZONE_* encodings would make
     * both sides of that comparison 0 and print "TrustZone secure" on a core that has none. */
#if (OS_ARCH_HAS_TRUSTZONE == 1)
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_SECURE)
    printf(", TrustZone secure");
#elif (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
    printf(", TrustZone non-secure");
#endif
#endif
    printf("\r\n");

    /* GCC exposes no macro for the numeric -O level, only these category flags - and -Og sets
     * exactly the same ones as -O2, so the preprocessor cannot tell a debug build from a release
     * one. That is not academic: it is how a -Og build comes to describe itself as "optimized for
     * speed" and have its benchmark numbers read as release numbers. CMake does know, so
     * test/CMakeLists.txt passes the build type in and it is printed first when available. */
    printf("  build     : ");
#ifdef OS_TEST_BUILD_TYPE
    printf("%s, ", OS_TEST_BUILD_TYPE);
#endif
#if !defined(__OPTIMIZE__)
    printf("-O0, NO optimization - expect several times slower than a release build");
#elif defined(__OPTIMIZE_SIZE__)
    printf("-Os, optimized for size");
#else
    printf("-Og/-O1/-O2/-O3 (the preprocessor cannot tell these apart)");
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

    printf("\r\n  Each operation is measured alone with the CPU cycle counter, sampled %u times.\r\n",
           (unsigned)TEST_BENCH_SAMPLES);
    printf("  BEST is the cheapest of those samples: interference only ever ADDS cycles, so it is\r\n");
    printf("  the uninterrupted cost of the code itself. WORST is the dearest of the same samples -\r\n");
    printf("  the same code plus whatever landed on top of it, which at a %u Hz tick is nearly\r\n",
           (unsigned)OS_CONFIG_TICK_HZ);
    printf("  always the tick ISR. Read BEST to compare kernels or spot a regression; read WORST to\r\n");
    printf("  budget for a deadline. Neither is a guaranteed bound: WORST is only the worst seen in\r\n");
    printf("  %u tries here, not a proof, and a busier system will beat it.\r\n",
           (unsigned)TEST_BENCH_SAMPLES);
    printf("  The two counter reads cost something too; that is measured the same way and already\r\n");
    printf("  subtracted from every row below.\r\n\r\n");

    /* Prove the clock the ns column is computed with, rather than trusting it.
     *
     * Every row below is a CYCLE count converted with clock_hz. If the cycle counter did not
     * actually advance at that rate - mcountinhibit left set, a clock changed after os_init(), a
     * counter that counts something other than core cycles - every ns figure would be wrong by the
     * same factor and nothing else in the run would say so. The tick comes from different hardware
     * entirely (mtime here, SysTick on Arm), so counting cycles across a known number of ticks
     * measures one clock against the other.
     *
     * Under the scheduler lock: preemption alone would not distort the ratio - the cycle counter
     * belongs to the core, so it advances for whoever is running, exactly as the tick does - but
     * migrating to another core mid-measurement would, since the two cores' counters are not the
     * same counter. The lock cannot stop the tick, which is what makes it the right tool here.
     *
     * 100 ticks so a +/-1 tick quantization is a 1% window; anything wrong enough to matter is far
     * outside that. */
    if (clock_hz != 0U)
    {
        uint32_t start_tick;
        uint32_t start_cycles;
        uint32_t elapsed_ticks;
        uint32_t elapsed_cycles;

        os_kernel_lock();

        start_tick = os_tick_get();
        while (os_tick_get() == start_tick)
        {
            /* Start on a tick edge, so the first tick is not a partial one. */
        }

        start_tick   = os_tick_get();
        start_cycles = os_arch_cycle_count_get();

        while ((os_tick_get() - start_tick) < 100U)
        {
            /* Busy-wait a known number of ticks. */
        }

        elapsed_cycles = os_arch_cycle_count_get() - start_cycles;
        elapsed_ticks  = os_tick_get() - start_tick;

        os_kernel_unlock();

        if (elapsed_ticks != 0U)
        {
            uint64_t measured = ((uint64_t)elapsed_cycles * (uint64_t)OS_CONFIG_TICK_HZ)
                                / (uint64_t)elapsed_ticks;
            uint32_t error_pct = (measured > (uint64_t)clock_hz)
                                 ? (uint32_t)(((measured - (uint64_t)clock_hz) * 100ULL) / (uint64_t)clock_hz)
                                 : (uint32_t)((((uint64_t)clock_hz - measured) * 100ULL) / (uint64_t)clock_hz);

            printf("  counter   : %lu Hz measured against the tick, %lu Hz configured (%lu%% apart)%s\r\n",
                   (unsigned long)measured, (unsigned long)clock_hz, (unsigned long)error_pct,
                   (error_pct <= 2U) ? " - agree" : "  *** DISAGREE, the ns column is wrong ***");
        }
    }

    printf("\r\n");
    printf("  %-40s %17s %17s\r\n", "Operation (each sampled alone)", "best", "worst");
    printf("  ----------------------------------------------------------------------------\r\n");

    /* Cost of the measurement itself: two counter reads with nothing between them. Subtracted
     * from every row, so a row shows the operation's own cycles and nothing else. */
    TEST_BENCH_CYCLES(overhead, overhead_worst, TEST_BENCH_SAMPLES, (void)0);
    test_bench_row("(measurement overhead, subtracted)", overhead, overhead_worst, clock_hz);

    TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, sink += os_tick_get());
    test_bench_row("os_tick_get", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

    TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, os_critical_enter(); os_critical_exit());
    test_bench_row("os_critical_enter + exit", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
    /* Placed directly under the critical section above, because that is the comparison that
     * decides how a shared word should be updated: on a core with exclusives an atomic is the
     * cheaper answer, and these rows say by how much. On ARMv6-M / ARMv8-M baseline the backend
     * IS a critical section, so the two should land within a few cycles of each other - that
     * result is the point, not a fault.
     *
     * The last row calls the port's operation directly, skipping the os_atomic_* wrapper's NULL
     * check and the branch into the port. The gap between it and the os_atomic_add row above is
     * the entire cost of the portable layer, which is the number to look at before trading the
     * port's out-of-line implementation away for an inline one. */
    {
        (void)os_atomic_set(&os_test_bench_atomic, 0);

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              sink += (uint32_t)os_atomic_get(&os_test_bench_atomic));
        test_bench_row("os_atomic_get (load)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, (void)os_atomic_add(&os_test_bench_atomic, 1));
        test_bench_row("os_atomic_add (read-modify-write)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_arch_atomic_add((__IO int32_t *)&os_test_bench_atomic, 1));
        test_bench_row("  ^ same, os_atomic_* layer skipped", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, os_atomic_set_bit(&os_test_bench_atomic, 0U));
        test_bench_row("os_atomic_set_bit", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

        /* expected == desired == what the word already holds, so the swap is always taken and
         * leaves the word where it started: this measures the successful path, not a retry. */
        (void)os_atomic_set(&os_test_bench_atomic, 0);
        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, (void)os_atomic_cas(&os_test_bench_atomic, 0, 0));
        test_bench_row("os_atomic_cas (swap taken)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_MUTEX_ENABLE == 1U)
    if (os_mutex_init(&os_test_bench_mutex) == OS_ERR_NONE)
    {
        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_mutex_lock(&os_test_bench_mutex, OS_WAIT_FOREVER);
                              (void)os_mutex_unlock(&os_test_bench_mutex));
        test_bench_row("os_mutex_lock + unlock", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_mutex_lock(&os_test_bench_mutex, OS_WAIT_NOTHING);
                              (void)os_mutex_unlock(&os_test_bench_mutex));
        test_bench_row("os_mutex_lock(NOTHING) + unlock", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_SEM_ENABLE == 1U)
    if (os_sem_init(&os_test_bench_sem, 0U, 1U) == OS_ERR_NONE)
    {
        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_sem_give(&os_test_bench_sem);
                              (void)os_sem_take(&os_test_bench_sem, OS_WAIT_NOTHING));
        test_bench_row("os_sem_give + take", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    if (os_queue_cleanup(&os_test_bench_queue) == OS_ERR_NONE)
    {
        uint32_t item = 0x5A5A5A5AUL;
        uint32_t out;

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_queue_send(&os_test_bench_queue, &item, OS_WAIT_NOTHING);
                              (void)os_queue_receive(&os_test_bench_queue, &out, OS_WAIT_NOTHING));
        test_bench_row("os_queue_send + receive (4-byte item)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_MSG_ENABLE == 1U)
    if (os_msg_cleanup(&os_test_bench_msg) == OS_ERR_NONE)
    {
        static const uint8_t payload[64] = { 0 };
        uint8_t              out[64];
        size_t               out_len;

        /* Two rows, because this is the one object whose cost is not a constant: it copies the
         * message rather than a fixed slot, so the difference between them IS the byte cost. The
         * 4-byte row is directly comparable to the queue's above - same payload, and what it
         * shows is what the length header and the byte ring cost over a slot index. */
        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_msg_send(&os_test_bench_msg, payload, 4U, OS_WAIT_NOTHING);
                              (void)os_msg_receive(&os_test_bench_msg, out, sizeof(out), &out_len,
                                                   OS_WAIT_NOTHING));
        test_bench_row("os_msg_send + receive (4-byte message)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_msg_send(&os_test_bench_msg, payload, 64U, OS_WAIT_NOTHING);
                              (void)os_msg_receive(&os_test_bench_msg, out, sizeof(out), &out_len,
                                                   OS_WAIT_NOTHING));
        test_bench_row("  ^ same, 64-byte message", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_EVENT_ENABLE == 1U)
    if (os_event_init(&os_test_bench_event) == OS_ERR_NONE)
    {
        uint32_t matched;

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_event_set_bits(&os_test_bench_event, 0x01U);
                              (void)os_event_wait_bits(&os_test_bench_event, 0x01U, false, true,
                                                              &matched, OS_WAIT_NOTHING));
        test_bench_row("os_event_set + wait (immediate)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);
    }
#endif

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    /* Created but never started: give() then only latches, which is exactly the ISR-side cost
     * an application cares about (the wake path is a context switch, measured below). */
    if (os_task_create(&helper, TEST_TASK_CONFIG(test_worker_entry, NULL, 1U)) == OS_ERR_NONE)
    {
        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, (void)os_notify_give(&helper, 1U));
        test_bench_row("os_notify_give (latch, no wake)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

        (void)os_task_delete(&helper);
    }
#endif

#if (OS_CONFIG_TIMER_ENABLE == 1U)
    /* The arming path, measured where nothing can interfere: the period outlives the run, so no
     * expiry is ever queued and no task is ever woken. This is also what deferred work costs, since
     * scheduling a deferred call IS starting a one-shot timer. Nothing here scales with a
     * configured maximum - there is no longer one. */
    TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                          (void)os_timer_start(&os_test_bench_timer, NULL, 0U);
                          (void)os_timer_stop(&os_test_bench_timer));
    test_bench_row("os_timer_start + stop (list empty)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

    /* The same pair with the running list already holding TEST_BENCH_TIMER_FILL timers. Both calls
     * search that list to PROVE membership rather than trusting the timer's own link pointers, so
     * the gap between these two rows is the cost of that guarantee - divide it by the fill count
     * for the per-running-timer price. */
    {
        uint32_t fill;

        for (fill = 0U; fill < TEST_BENCH_TIMER_FILL; fill++)
        {
            (void)os_timer_start(os_test_bench_fill[fill], NULL, 0U);
        }

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES,
                              (void)os_timer_start(&os_test_bench_timer, NULL, 0U);
                              (void)os_timer_stop(&os_test_bench_timer));
        test_bench_row("  ^ same, with 8 other timers running", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

        for (fill = 0U; fill < TEST_BENCH_TIMER_FILL; fill++)
        {
            (void)os_timer_stop(os_test_bench_fill[fill]);
        }
    }

#endif

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
    {
        void *p;

        TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, p = os_mem_alloc(64U); os_mem_free(p));
        test_bench_row("os_mem_alloc + os_mem_free (64 B)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);
    }
#endif

    /* os_task_yield always pends PendSV, so this is a FULL context-switch round trip - register
     * save, scheduler pick, register restore - that happens to re-select this same task because
     * nothing else is ready. That makes it the cleanest single-number context-switch cost:
     * no second task's cache/branch-predictor effects mixed in. */
    TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, os_task_yield());
    test_bench_row("os_task_yield (switch, re-selects self)", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

    /* Is the pick really O(1)? The same yield, with the ready lists loaded.
     *
     * The fillers sit at a LOWER priority, so this task is still what gets picked and the sample
     * keeps exactly the shape of the row above - the only thing that changed is how much is queued
     * behind it. A scheduler that WALKED its ready structure would show that here; this one reads a
     * bitmap, takes its highest set bit and pops one FIFO head, so the number should not move. Read
     * it against the timer rows further up, where the cost does grow with the list, to see the
     * difference between a claim and a measurement.
     *
     * They never run: they are ready, but this task never blocks, and yielding does not lower a
     * priority. That only holds if they are queued behind THIS task, which on a multi-core build
     * means pinning this one to their core - left free, it would sit on the other core and the
     * fillers would simply run on core 0 instead of waiting in the list, which is the opposite of
     * what the row is for. */
    {
        uint32_t fill;
        uint32_t started = 0U;

#if (OS_CONFIG_CORE_COUNT > 1U)
        (void)os_task_core_affinity_set(NULL, OS_TASK_CORE(0));
#endif

        for (fill = 0U; fill < TEST_BENCH_TASK_FILL; fill++)
        {
            if (os_task_create(os_test_bench_task_fill[fill],
                               TEST_TASK_CONFIG(test_worker_entry, NULL, TEST_PRIO_LOW)) == OS_ERR_NONE)
            {
                (void)os_task_start(os_test_bench_task_fill[fill]);
                started++;
            }
        }

        if (started == TEST_BENCH_TASK_FILL)
        {
            TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, os_task_yield());
            test_bench_row("  ^ same, with 4 more tasks ready", TEST_BENCH_SUB(best, overhead),
                           TEST_BENCH_SUB(worst, overhead), clock_hz);
        }

        for (fill = 0U; fill < TEST_BENCH_TASK_FILL; fill++)
        {
            (void)os_task_delete(os_test_bench_task_fill[fill]);
        }

#if (OS_CONFIG_CORE_COUNT > 1U)
        (void)os_task_core_affinity_set(NULL, OS_TASK_CORE_ANY);
#endif
    }

    /* The same yield with somewhere else to go: a partner task of equal priority sits ready, so the
     * switch restores ANOTHER task's registers, runs its yield, and comes back. That round trip is
     * two context switches, and half of it is the number an RTOS is usually asked for.
     *
     * Half rather than a direct single-switch measurement because there is no way to read the cycle
     * counter "in the middle": the counter can only be sampled by a running task, and between the
     * two samples the CPU has to leave this task and return. Two switches is the smallest closed
     * path, and the partner's loop adds only its own os_task_yield() call - which is the operation
     * being measured anyway.
     *
     * Both tasks are pinned to one core for the duration. Left free on a multi-core build they
     * would simply run side by side on two cores, and the yield would find nothing to switch to -
     * measuring the row above a second time instead of a real switch.
     */
    {
#if (OS_CONFIG_CORE_COUNT > 1U)
        (void)os_task_core_affinity_set(NULL, OS_TASK_CORE(0));
#endif
        os_test_bench_partner_run = true;

        if (os_task_create(&helper, TEST_TASK_CONFIG(test_bench_partner_entry, NULL,
                                                    (uint32_t)OS_CONFIG_TEST_PRIORITY)) == OS_ERR_NONE)
        {
            (void)os_task_start(&helper);
            os_task_yield();          /* let the partner reach its loop before the first sample */

            TEST_BENCH_CYCLES(best, worst, TEST_BENCH_SAMPLES, os_task_yield());
            best  = TEST_BENCH_SUB(best, overhead);
            worst = TEST_BENCH_SUB(worst, overhead);

            test_bench_row("os_task_yield (round trip, 2 switches)", best, worst, clock_hz);
            test_bench_row("  ^ ONE context switch, task to task", best / 2U, worst / 2U, clock_hz);
        }

        os_test_bench_partner_run = false;
        os_task_yield();              /* give it the CPU once more so it can see the flag and exit */
        (void)test_wait_inactive(&helper, 200U);

#if (OS_CONFIG_CORE_COUNT > 1U)
        (void)os_task_core_affinity_set(NULL, OS_TASK_CORE_ANY);
#endif
    }

    TEST_BENCH_CYCLES(best, worst, TEST_BENCH_HEAVY_SAMPLES,
                          if (os_task_create(&helper, TEST_TASK_CONFIG(test_worker_entry,
                                                                      NULL, 1U)) == OS_ERR_NONE)
                          {
                              (void)os_task_delete(&helper);
                          });
    test_bench_row("os_task_create + os_task_delete", TEST_BENCH_SUB(best, overhead),
                   TEST_BENCH_SUB(worst, overhead), clock_hz);

    printf("  ----------------------------------------------------------------------------\r\n");
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

/*
 * ***********************************************************************************************************
 * Multi-core (SMP)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)

/* Enough contention that a broken lock loses updates reliably rather than occasionally: each core
 * does this many read-modify-writes on one word, as fast as it can, at the same time as the other.
 * A lock that does not exclude drops hundreds, not one or two. */
#define TEST_MC_LOCK_ITERATIONS     4000U

/* How long core 0 waits for a task pinned to core 1 to run at all. Generous by design - this is
 * the check that says whether the second core booted, and a slow answer is still an answer. */
#define TEST_MC_START_TIMEOUT_MS    500U

/* How often each parked worker proves its core is still alive, and how long the late check
 * watches for. The window is several heartbeats so one missed wake cannot fail it. */
#define TEST_MC_HEARTBEAT_MS        100U
#define TEST_MC_WATCH_MS            600U

OS_TASK_DEFINE(test_mc_core0, 512U);
OS_TASK_DEFINE(test_mc_core1, 512U);

/* One slot per worker, indexed by the core it is PINNED to, so a worker writing the wrong slot is
 * itself a detectable failure. __IO throughout: written on one core and read on the other, with
 * no lock, so the compiler must not cache any of it in a register. */
static __IO uint32_t test_mc_seen_core[2]  = { 0xFFFFFFFFU, 0xFFFFFFFFU };
static __IO uint32_t test_mc_ready[2]      = { 0U, 0U };
static __IO uint32_t test_mc_done[2]       = { 0U, 0U };
static __IO uint32_t test_mc_begin_tick[2] = { 0U, 0U };
static __IO uint32_t test_mc_end_tick[2]   = { 0U, 0U };

/* Advanced by each parked worker, once per heartbeat, for the whole rest of the run. Read twice by
 * the late check to see whether that core is still going. */
static __IO uint32_t test_mc_alive[2]      = { 0U, 0U };

/* The kernel tick at each core's most recent heartbeat. Whatever it holds at the end is the moment
 * that core last ran. */
static __IO uint32_t test_mc_last_tick[2]  = { 0U, 0U };

/* Released by core 0 once BOTH workers have reported in, so the two hammer the shared counter at
 * the same time. Without it one could finish before the other starts and the lock would never be
 * contended - the test would pass on a lock that excludes nothing. */
static __IO uint32_t test_mc_gate = 0U;

/* The word the kernel spinlock is supposed to protect. Deliberately NOT atomic: the point is to
 * test os_critical_enter/exit, so the increment must be a plain read-modify-write that interleaves
 * destructively if the lock fails. */
static __IO uint32_t test_mc_counter = 0U;

/******************************************************************************************************/
/**
 * @brief Body of both multi-core workers: report which core we are on, then hammer the shared
 *        counter under the kernel lock.
 *
 * @param context The core index this task was pinned to, as a small integer cast to a pointer.
 */
static void test_mc_worker_entry(void *context)
{
    uint32_t slot = (uint32_t)(uintptr_t)context;
    uint32_t i;

    /* Read the id BEFORE anything can migrate us. With core affinity honoured this is fixed for
     * the task's lifetime; the whole point of the check is that it equals `slot`. */
    test_mc_seen_core[slot] = os_arch_core_id_get();
    test_mc_ready[slot]     = 1U;

    /* Spin, not block: a delay would let the two workers drift apart, and the lock is only under
     * test while both are inside the loop below at once. */
    while (test_mc_gate == 0U)
    {
    }

    test_mc_begin_tick[slot] = os_tick_get();

    for (i = 0U; i < TEST_MC_LOCK_ITERATIONS; i++)
    {
        os_critical_enter();
        test_mc_counter = test_mc_counter + 1U;
        os_critical_exit();
    }

    test_mc_end_tick[slot] = os_tick_get();
    test_mc_done[slot]     = 1U;

    /* Parked rather than deleted: a task running on another core cannot be deleted from this one
     * (OS_ERR_BUSY by design), so the suite leaves both workers blocked here.
     *
     * The heartbeat is the point of parking here rather than exiting. A secondary core that has
     * gone quiet is indistinguishable from one that simply has nothing to do, and this suite
     * spent a long time confusing the two. A counter that stops advancing is the difference. */
    while (1)
    {
        os_delay_ms(TEST_MC_HEARTBEAT_MS);

        test_mc_alive[slot]++;

        /* Stamped every beat, so the LAST one records the moment this core stopped. Knowing that a
         * core died is not actionable; knowing it died at tick 1200 out of 12000 points straight
         * at whichever test was running then. */
        test_mc_last_tick[slot] = os_tick_get();
    }
}

/******************************************************************************************************/
/**
 * @brief Exercise what a dual-core SoC package has to get right: that the second core starts at
 *        all, that each core reports its own id, that affinity pins a task where it was asked to
 *        go, and that the kernel spinlock really excludes across cores.
 *
 * Every check here fails SILENTLY on a broken port rather than loudly, which is why they are worth
 * running on real silicon. A core that never boots looks like tasks that are merely never
 * scheduled; a core-id callback stubbed to 0 looks like a scheduler that ignores affinity; a
 * spinlock that does not exclude looks like nothing at all until state corrupts hours later.
 */
static void test_multicore(void)
{
    uint32_t waited;
    uint32_t expected = TEST_MC_LOCK_ITERATIONS * 2U;
    bool     overlapped;

    test_print_section("Multi-core (SMP): core start, core id, affinity, kernel spinlock");

    AHURA_TEST_CHECK(os_task_create(&test_mc_core0,
                     OS_TASK_CONFIG(test_mc_worker_entry, (void *)0U, OS_TASK_PRIO_2,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "worker pinned to core 0 created");
    AHURA_TEST_CHECK(os_task_create(&test_mc_core1,
                     OS_TASK_CONFIG(test_mc_worker_entry, (void *)1U, OS_TASK_PRIO_2,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "worker pinned to core 1 created");

    AHURA_TEST_CHECK(os_task_start(&test_mc_core0) == OS_ERR_NONE, "worker on core 0 started");
    AHURA_TEST_CHECK(os_task_start(&test_mc_core1) == OS_ERR_NONE, "worker on core 1 started");

    /* The core-start check. Nothing else in the suite can tell whether os_arch_core_launch_cb()
     * did anything, because a core that never booted is indistinguishable from one whose tasks
     * are simply never picked. */
    for (waited = 0U; (waited < TEST_MC_START_TIMEOUT_MS) && (test_mc_ready[1] == 0U); waited++)
    {
        os_delay_ms(1U);
    }

    AHURA_TEST_CHECK(test_mc_ready[1] == 1U,
                     "core 1 is running: a task pinned to it ran within %u ms",
                     (unsigned)TEST_MC_START_TIMEOUT_MS);

    if (test_mc_ready[1] == 0U)
    {
        printf("  [INFO] core 1 never ran the task pinned to it.\r\n");
        printf("         Everything below this line is chip-specific, so the SoC package is where\r\n");
        printf("         to look - it owns core start-up, the inter-core interrupt and the fault\r\n");
        printf("         vectors.\r\n");

        /* The package reports what only it can know. Called from here rather than at start-up
         * because on a USB-console board nothing printed during os_start() is ever seen: the host
         * has not opened the port yet. Weak-defaulted, so a target without a package prints
         * nothing and this costs one call. */
        os_arch_soc_diagnose_cb();

        printf("         Remaining multi-core checks skipped: they share this one cause.\r\n");
        return;
    }

    for (waited = 0U; (waited < TEST_MC_START_TIMEOUT_MS) && (test_mc_ready[0] == 0U); waited++)
    {
        os_delay_ms(1U);
    }

    /* Both id checks together, because the failure they exist to catch is one callback answering
     * the same value everywhere. Either alone would pass against a stub returning 0. */
    AHURA_TEST_CHECK(test_mc_seen_core[0] == 0U,
                     "core id on core 0 reads 0 (saw %u)", (unsigned)test_mc_seen_core[0]);
    AHURA_TEST_CHECK(test_mc_seen_core[1] == 1U,
                     "core id on core 1 reads 1 (saw %u)", (unsigned)test_mc_seen_core[1]);
    AHURA_TEST_CHECK(test_mc_seen_core[0] != test_mc_seen_core[1],
                     "the two cores report DIFFERENT ids (a stubbed callback returns one value)");

    /* Now the lock. Both workers are parked on the gate, so releasing it puts them into the
     * critical section together. */
    test_mc_gate = 1U;

    for (waited = 0U; (waited < TEST_MC_START_TIMEOUT_MS) &&
                      ((test_mc_done[0] == 0U) || (test_mc_done[1] == 0U)); waited++)
    {
        os_delay_ms(1U);
    }

    AHURA_TEST_CHECK((test_mc_done[0] == 1U) && (test_mc_done[1] == 1U),
                     "both workers finished %u guarded increments each",
                     (unsigned)TEST_MC_LOCK_ITERATIONS);

    /* The result that matters. Any value below `expected` is a lost update, which means two cores
     * were inside the critical section at once. */
    AHURA_TEST_CHECK(test_mc_counter == expected,
                     "kernel spinlock excludes across cores: counter is %u, expected %u",
                     (unsigned)test_mc_counter, (unsigned)expected);

    /* Proof the previous check was actually under contention. If the two runs did not overlap in
     * time, a lock that excludes nothing would have passed it, so the result would mean nothing.
     * Reported rather than failed: overlap depends on scheduling, and a serialised run is a weak
     * test rather than a broken kernel. */
    overlapped = (test_mc_begin_tick[0] <= test_mc_end_tick[1]) &&
                 (test_mc_begin_tick[1] <= test_mc_end_tick[0]);

    if (overlapped)
    {
        printf("  [INFO] the two runs overlapped (core0 %u..%u, core1 %u..%u ticks) - the lock\r\n",
               (unsigned)test_mc_begin_tick[0], (unsigned)test_mc_end_tick[0],
               (unsigned)test_mc_begin_tick[1], (unsigned)test_mc_end_tick[1]);
        printf("         was genuinely contended, so the check above means something\r\n");
    }
    else
    {
        printf("  [INFO] the two runs did NOT overlap in time - the lock was never contended, so\r\n");
        printf("         the counter check above passes trivially and proves little\r\n");
    }

    /* Affinity at runtime, which is the one multi-core API an application is likely to call. */
    AHURA_TEST_CHECK(os_task_core_affinity_set(&test_mc_core1, OS_TASK_CORE(1)) == OS_ERR_NONE,
                     "os_task_core_affinity_set() accepts a valid mask");

    AHURA_TEST_CHECK(os_task_core_affinity_set(&test_mc_core1,
                     OS_TASK_CORE(OS_CONFIG_CORE_COUNT)) != OS_ERR_NONE,
                     "os_task_core_affinity_set() rejects a core beyond OS_CONFIG_CORE_COUNT");
}

#endif /* OS_CONFIG_CORE_COUNT > 1U */

#if (OS_CONFIG_CORE_COUNT > 1U)

/******************************************************************************************************/
/**
 * @brief Re-check, at the very end of the run, that both cores are still alive.
 *
 * The same mechanisms test_multicore() proves at one instant, watched over a window: the two
 * workers that test parked are still beating, several heartbeats apart, so a single missed wake
 * cannot fail it. That is the question worth asking LAST - whether the secondary core survived
 * the whole suite, not just its own section - which is why the multi-core pair runs after
 * everything else.
 */
static void test_multicore_watch(uint32_t watch_ms, const char *when)
{
    uint32_t before0;
    uint32_t before1;

    printf("\r\n--- Multi-core (SMP): still alive %s? ---\r\n", when);

    if (test_mc_ready[1] == 0U)
    {
        printf("  [SKIP] core 1 never started, so there is nothing to outlive\r\n");
        return;
    }

    before0 = test_mc_alive[0];
    before1 = test_mc_alive[1];

    os_delay_ms(watch_ms);

    AHURA_TEST_CHECK(test_mc_alive[0] > before0,
                     "core 0 still scheduling (%lu beats in %lu ms)",
                     (unsigned long)(test_mc_alive[0] - before0), (unsigned long)watch_ms);

    /* The one that matters. A core that ran at the start of the suite and is silent now has died
     * somewhere in between, and being idle for long stretches is the only thing that separates
     * core 1 from core 0 here. */
    AHURA_TEST_CHECK(test_mc_alive[1] > before1,
                     "core 1 STILL ALIVE (%lu beats in %lu ms)",
                     (unsigned long)(test_mc_alive[1] - before1), (unsigned long)watch_ms);

    if (test_mc_alive[1] == before1)
    {
        printf("  [INFO] core 1 started fine and then stopped during the run.\r\n");
        printf("         core 1 last ran at tick %lu; the clock now reads %lu.\r\n",
               (unsigned long)test_mc_last_tick[1], (unsigned long)os_tick_get());
        printf("         It managed %lu heartbeats before going quiet, so it survived roughly\r\n",
               (unsigned long)test_mc_alive[1]);
        printf("         %lu ms of the run and then stopped - which names the window to look in.\r\n",
               (unsigned long)(test_mc_alive[1] * TEST_MC_HEARTBEAT_MS));
        printf("         (core 0, for comparison, last ran at tick %lu)\r\n",
               (unsigned long)test_mc_last_tick[0]);
        os_arch_soc_diagnose_cb();
    }
}

/*
 * ***********************************************************************************************************
 * Multi-core (SMP) stress - cross-core contention and wake integrity
 * ***********************************************************************************************************
 *
 * Everything above proved the kernel one subsystem at a time, on the core each helper was pinned
 * to. These push the SMP seams specifically, and each is built so a failure is exact rather than
 * approximate: handshakes make every cross-core wake 1:1, so a lost or duplicated wake shows up
 * as a miscounted value, and guarded counters come out exact only if the spinlock really excludes
 * two cores at once.
 *
 * These run with OS_CONFIG_MAX_USER_TASKS at 8: the test task, the two heartbeat workers parked
 * by test_multicore(), and up to five concurrent helpers below.
 */

#define TEST_SMP_NESTED_ITERATIONS   20000U
#define TEST_SMP_ATOMIC_ITERATIONS   40000U
#define TEST_SMP_PINGPONG_ROUNDS     500U
#define TEST_SMP_EVENT_ROUNDS        300U
#define TEST_SMP_QUEUE_ITEMS         300U
#define TEST_SMP_CHURN_CYCLES        40U
#define TEST_SMP_SUBMIT_EACH         16U
#define TEST_SMP_SOAK_ITERATIONS     150U
#define TEST_SMP_MIGRATION_SAMPLES   8U

/* Six dedicated task handles: every helper exits on its own (entry returns), so the handles come
 * back INACTIVE between sections and are safe to re-create. Never deleted cross-core, which is
 * OS_ERR_BUSY by design. */
OS_TASK_DEFINE(test_smp_a, 512U);
OS_TASK_DEFINE(test_smp_b, 512U);
OS_TASK_DEFINE(test_smp_c, 512U);
OS_TASK_DEFINE(test_smp_d, 512U);
OS_TASK_DEFINE(test_smp_e, 256U);
OS_TASK_DEFINE(test_smp_f, 256U);

/******************************************************************************************************/
/**
 * @brief Nested critical sections on both cores at once: the per-core nesting counters and the
 *        single cross-core spinlock must agree, or the counter below loses updates.
 */
static __IO uint32_t test_smp_nested_counter = 0U;
static __IO uint32_t test_smp_nested_seen[2] = { 0xFFFFFFFFU, 0xFFFFFFFFU };
static __IO uint32_t test_smp_nested_done[2] = { 0U, 0U };
static __IO uint32_t test_smp_nested_gate    = 0U;

static void test_smp_nested_entry(void *context)
{
    uint32_t slot = (uint32_t)(uintptr_t)context;
    uint32_t i;

    test_smp_nested_seen[slot] = os_arch_core_id_get();

    /* Both workers spin here so the two loops below are genuinely simultaneous - the same gate
     * pattern as test_multicore(). */
    while (test_smp_nested_gate == 0U)
    {
    }

    for (i = 0U; i < TEST_SMP_NESTED_ITERATIONS; i++)
    {
        os_critical_enter();
        os_critical_enter();
        test_smp_nested_counter = test_smp_nested_counter + 1U;
        os_critical_exit();
        os_critical_exit();
    }

    test_smp_nested_done[slot] = 1U;
}

static void test_smp_critical_nested(void)
{
    uint32_t waited;
    uint32_t expected = TEST_SMP_NESTED_ITERATIONS * 2U;

    test_print_section("Multi-core (SMP): nested critical sections contend across cores");

    test_smp_nested_counter = 0U;
    test_smp_nested_seen[0] = 0xFFFFFFFFU;
    test_smp_nested_seen[1] = 0xFFFFFFFFU;
    test_smp_nested_done[0] = 0U;
    test_smp_nested_done[1] = 0U;
    test_smp_nested_gate    = 0U;

    /* OS_TASK_PRIO_2, NOT TEST_PRIO_HIGH: these workers spin on the gate until it opens, and
     * the test task is the one that opens it - a higher-priority spinner on core 0 would starve
     * the very task that releases it. Equal priority round-robins, so both still run. */
    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_nested_entry, (void *)0U, OS_TASK_PRIO_2,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "core-0 nested-critical worker created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_b,
                     OS_TASK_CONFIG(test_smp_nested_entry, (void *)1U, OS_TASK_PRIO_2,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "core-1 nested-critical worker created");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "core-0 worker started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_b) == OS_ERR_NONE, "core-1 worker started");

    for (waited = 0U; (waited < TEST_MC_START_TIMEOUT_MS) &&
                      ((test_smp_nested_seen[0] == 0xFFFFFFFFU) || (test_smp_nested_seen[1] == 0xFFFFFFFFU)); waited++)
    {
        os_delay_ms(1U);
    }

    AHURA_TEST_CHECK((test_smp_nested_seen[0] == 0U) && (test_smp_nested_seen[1] == 1U),
                     "both workers ran on their pinned cores before the gate opened");

    test_smp_nested_gate = 1U;

    for (waited = 0U; (waited < TEST_MC_START_TIMEOUT_MS) &&
                      ((test_smp_nested_done[0] == 0U) || (test_smp_nested_done[1] == 0U)); waited++)
    {
        os_delay_ms(1U);
    }

    AHURA_TEST_CHECK((test_smp_nested_done[0] == 1U) && (test_smp_nested_done[1] == 1U),
                     "both workers finished %u nested guarded increments each",
                     (unsigned)TEST_SMP_NESTED_ITERATIONS);
    AHURA_TEST_CHECK(test_smp_nested_counter == expected,
                     "nested critical sections exclude across cores: counter is %lu, expected %lu",
                     (unsigned long)test_smp_nested_counter, (unsigned long)expected);
}

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief os_atomic_inc on one word from both cores at once. The port's atomics are LDREX/STREX,
 *        so this also proves the GLOBAL exclusive monitor works on this part - a lost update
 *        means the interconnect is not excluding between cores, which the kernel cannot fix.
 */
static os_atomic_t   test_smp_atomic_word   = OS_ATOMIC_INIT(0);
static __IO uint32_t test_smp_atomic_done[2] = { 0U, 0U };
static __IO uint32_t test_smp_atomic_gate    = 0U;

static void test_smp_atomic_entry(void *context)
{
    uint32_t slot = (uint32_t)(uintptr_t)context;
    uint32_t i;

    while (test_smp_atomic_gate == 0U)
    {
    }

    for (i = 0U; i < TEST_SMP_ATOMIC_ITERATIONS; i++)
    {
        (void)os_atomic_inc(&test_smp_atomic_word);
    }

    test_smp_atomic_done[slot] = 1U;
}

static void test_smp_atomic_contention(void)
{
    uint32_t waited;
    uint32_t expected = TEST_SMP_ATOMIC_ITERATIONS * 2U;

    test_print_section("Multi-core (SMP): os_atomic_inc contention across cores");

    test_smp_atomic_done[0] = 0U;
    test_smp_atomic_done[1] = 0U;
    test_smp_atomic_gate    = 0U;

    /* OS_TASK_PRIO_2 for the same reason as the nested-critical workers: they spin on the gate,
     * and only the test task can open it. */
    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_atomic_entry, (void *)0U, OS_TASK_PRIO_2,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "core-0 atomic worker created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_b,
                     OS_TASK_CONFIG(test_smp_atomic_entry, (void *)1U, OS_TASK_PRIO_2,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "core-1 atomic worker created");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "core-0 worker started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_b) == OS_ERR_NONE, "core-1 worker started");

    test_smp_atomic_gate = 1U;

    for (waited = 0U; (waited < TEST_MC_START_TIMEOUT_MS) &&
                      ((test_smp_atomic_done[0] == 0U) || (test_smp_atomic_done[1] == 0U)); waited++)
    {
        os_delay_ms(1U);
    }

    AHURA_TEST_CHECK((test_smp_atomic_done[0] == 1U) && (test_smp_atomic_done[1] == 1U),
                     "both workers finished %u increments each", (unsigned)TEST_SMP_ATOMIC_ITERATIONS);
    AHURA_TEST_CHECK(os_atomic_get(&test_smp_atomic_word) == (int32_t)expected,
                     "os_atomic_inc lost nothing across cores (%ld of %lu)",
                     (long)os_atomic_get(&test_smp_atomic_word), (unsigned long)expected);
}
#endif /* OS_CONFIG_ATOMIC_ENABLE */

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Task notifications ping-pong between two pinned tasks, one per core. Each side waits for
 *        its own next value before answering, so every cross-core wake must deliver exactly one
 *        value: any lost or duplicated wake breaks the sequence the instant it happens.
 */
static __IO bool     test_smp_notify_ok     = true;
static __IO uint32_t test_smp_notify_rounds = 0U;

static void test_smp_notify_a_entry(void *context)
{
    uint32_t expected = 1U;
    uint32_t round;
    uint32_t value;

    (void)context;

    /* Side A (core 0) receives 1, 3, 5, ... - one per round - and answers with the even value
     * that follows. */
    for (round = 0U; round < TEST_SMP_PINGPONG_ROUNDS; round++)
    {
        if (os_notify_wait(OS_WAIT_FOREVER, &value) != OS_ERR_NONE)
        {
            test_smp_notify_ok = false;
            return;
        }

        if (value != expected)
        {
            test_smp_notify_ok = false;
        }

        expected += 2U;

        if (os_notify_give(&test_smp_b, expected - 1U) != OS_ERR_NONE)
        {
            test_smp_notify_ok = false;
            return;
        }

        test_smp_notify_rounds++;
    }
}

static void test_smp_notify_b_entry(void *context)
{
    uint32_t expected;
    uint32_t round;
    uint32_t value;

    (void)context;

    /* Side B (core 1) opens with the first value, answers every even value with the next odd
     * one, and makes no trailing give - so neither side is ever left with a value pending after
     * the other has exited. */
    if (os_notify_give(&test_smp_a, 1U) != OS_ERR_NONE)
    {
        test_smp_notify_ok = false;
        return;
    }

    expected = 2U;

    for (round = 0U; round < TEST_SMP_PINGPONG_ROUNDS; round++)
    {
        if (os_notify_wait(OS_WAIT_FOREVER, &value) != OS_ERR_NONE)
        {
            test_smp_notify_ok = false;
            return;
        }

        if (value != expected)
        {
            test_smp_notify_ok = false;
        }

        expected += 2U;

        if (round < (TEST_SMP_PINGPONG_ROUNDS - 1U))
        {
            if (os_notify_give(&test_smp_a, expected - 1U) != OS_ERR_NONE)
            {
                test_smp_notify_ok = false;
                return;
            }
        }
    }
}

static void test_smp_notify_pingpong(void)
{
    test_print_section("Multi-core (SMP): task notifications ping-pong across cores");

    test_smp_notify_ok     = true;
    test_smp_notify_rounds = 0U;

    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_notify_a_entry, NULL, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "notification receiver pinned to core 0 created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_b,
                     OS_TASK_CONFIG(test_smp_notify_b_entry, NULL, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "notification sender pinned to core 1 created");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "receiver started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_b) == OS_ERR_NONE, "sender started");

    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_b, 2000U), "sender finished its rounds");
    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_a, 2000U), "receiver finished its rounds");

    AHURA_TEST_CHECK(test_smp_notify_rounds == TEST_SMP_PINGPONG_ROUNDS,
                     "the receiver consumed every one of the %u rounds exactly once (%lu)",
                     (unsigned)TEST_SMP_PINGPONG_ROUNDS, (unsigned long)test_smp_notify_rounds);
    AHURA_TEST_CHECK(test_smp_notify_ok,
                     "every value arrived exactly once, in order - no lost or duplicated wake");
}
#endif /* OS_CONFIG_NOTIFY_ENABLE */

#if (OS_CONFIG_SEM_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Binary-semaphore ping-pong between two pinned tasks, one per core. Each round is one
 *        take/give on each side, so the token crosses the IPI path twice - and the final token
 *        count proves the accounting exactly.
 */
static os_sem_t test_smp_sem_a;
static os_sem_t test_smp_sem_b;
static __IO uint32_t  test_smp_sem_rounds = 0U;

static void test_smp_sem_a_entry(void *context)
{
    uint32_t round;

    (void)context;

    for (round = 0U; round < TEST_SMP_PINGPONG_ROUNDS; round++)
    {
        if (os_sem_take(&test_smp_sem_a, OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            return;
        }

        test_smp_sem_rounds++;

        if (os_sem_give(&test_smp_sem_b) != OS_ERR_NONE)
        {
            return;
        }
    }
}

static void test_smp_sem_b_entry(void *context)
{
    uint32_t round;

    (void)context;

    for (round = 0U; round < TEST_SMP_PINGPONG_ROUNDS; round++)
    {
        if (os_sem_take(&test_smp_sem_b, OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            return;
        }

        if (os_sem_give(&test_smp_sem_a) != OS_ERR_NONE)
        {
            return;
        }
    }
}

static void test_smp_semaphore_pingpong(void)
{
    test_print_section("Multi-core (SMP): semaphore tokens ping-pong across cores");

    test_smp_sem_rounds = 0U;

    AHURA_TEST_CHECK(os_sem_init(&test_smp_sem_a, 0U, 1U) == OS_ERR_NONE, "semaphore A initialized empty");
    AHURA_TEST_CHECK(os_sem_init(&test_smp_sem_b, 0U, 1U) == OS_ERR_NONE, "semaphore B initialized empty");

    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_sem_a_entry, NULL, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "core-0 taker created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_b,
                     OS_TASK_CONFIG(test_smp_sem_b_entry, NULL, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "core-1 taker created");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "core-0 taker started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_b) == OS_ERR_NONE, "core-1 taker started");

    /* The one token that starts the machine. */
    AHURA_TEST_CHECK(os_sem_give(&test_smp_sem_a) == OS_ERR_NONE, "starting token given");

    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_b, 2000U), "core-1 taker finished its rounds");
    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_a, 2000U), "core-0 taker finished its rounds");

    AHURA_TEST_CHECK(test_smp_sem_rounds == TEST_SMP_PINGPONG_ROUNDS,
                     "every one of the %u rounds happened exactly once (%lu)",
                     (unsigned)TEST_SMP_PINGPONG_ROUNDS, (unsigned long)test_smp_sem_rounds);

    /* The token ledger must close exactly: A holds the one starting token back (its N-th take
     * consumed the N-th give, and the starter's token rides along), B is empty. */
    AHURA_TEST_CHECK(os_sem_take(&test_smp_sem_a, OS_WAIT_NOTHING) == OS_ERR_NONE,
                     "semaphore A ends with exactly the starting token");
    AHURA_TEST_CHECK(os_sem_take(&test_smp_sem_a, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                     "and no more than that");
    AHURA_TEST_CHECK(os_sem_take(&test_smp_sem_b, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                     "semaphore B ends empty - no token was lost or duplicated");
}
#endif /* OS_CONFIG_SEM_ENABLE */

#if (OS_CONFIG_QUEUE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Two producers (one per core) feed one consumer with exactly accounted items. Each item
 *        carries its producer id and per-producer sequence, so the consumer can reject any loss,
 *        duplication or reordering - and a capacity below the combined send rate forces the FULL
 *        path and backpressure through the cross-core wake.
 */
OS_QUEUE_DEFINE_STATIC_ATTR(test_smp_queue, uint32_t, 4, );

static __IO uint32_t test_smp_queue_expected[2] = { 1U, 1U };
static __IO bool     test_smp_queue_ok          = true;
static __IO uint32_t test_smp_queue_received    = 0U;

static void test_smp_queue_producer_entry(void *context)
{
    uint32_t id  = (uint32_t)(uintptr_t)context;
    uint32_t seq;

    for (seq = 1U; seq <= TEST_SMP_QUEUE_ITEMS; seq++)
    {
        uint32_t item = (id << 16) | seq;

        if (os_queue_send(&test_smp_queue, &item, OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            test_smp_queue_ok = false;
            return;
        }
    }
}

static void test_smp_queue_consumer_entry(void *context)
{
    uint32_t total = TEST_SMP_QUEUE_ITEMS * 2U;
    uint32_t count;

    (void)context;

    for (count = 0U; count < total; count++)
    {
        uint32_t item;
        uint32_t id;
        uint32_t seq;

        if (os_queue_receive(&test_smp_queue, &item, OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            test_smp_queue_ok = false;
            return;
        }

        id  = item >> 16;
        seq = item & 0xFFFFU;

        if ((id > 1U) || (seq != test_smp_queue_expected[id]))
        {
            test_smp_queue_ok = false;
        }

        test_smp_queue_expected[id]++;
        test_smp_queue_received++;
    }
}

static void test_smp_queue_accounting(void)
{
    uint32_t ignored = 0U;

    test_print_section("Multi-core (SMP): two producers, one consumer, exact item accounting");

    test_smp_queue_expected[0] = 1U;
    test_smp_queue_expected[1] = 1U;
    test_smp_queue_ok          = true;
    test_smp_queue_received    = 0U;

    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_queue_producer_entry, (void *)0U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "producer on core 0 created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_b,
                     OS_TASK_CONFIG(test_smp_queue_producer_entry, (void *)1U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "producer on core 1 created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_c,
                     OS_TASK_CONFIG(test_smp_queue_consumer_entry, NULL, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "consumer on core 0 created");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "core-0 producer started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_b) == OS_ERR_NONE, "core-1 producer started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_c) == OS_ERR_NONE, "consumer started");

    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_c, 3000U), "consumer drained every item");
    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_a, 3000U), "core-0 producer finished");
    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_b, 3000U), "core-1 producer finished");

    AHURA_TEST_CHECK(test_smp_queue_received == (TEST_SMP_QUEUE_ITEMS * 2U),
                     "the consumer received exactly %u items (%lu)",
                     (unsigned)(TEST_SMP_QUEUE_ITEMS * 2U), (unsigned long)test_smp_queue_received);
    AHURA_TEST_CHECK(test_smp_queue_ok,
                     "every item arrived exactly once, per-producer sequences intact across cores");
    AHURA_TEST_CHECK(os_queue_receive(&test_smp_queue, &ignored, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                     "the queue ends empty - nothing lost, nothing left behind");
}
#endif /* OS_CONFIG_QUEUE_ENABLE */

#if (OS_CONFIG_EVENT_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Event-bit handshake across cores: the setter on core 1 sets bit 1 and waits for bit 2,
 *        the waiter on core 0 consumes bit 1 and answers with bit 2. Clear-on-exit makes each
 *        round 1:1, so a lost wake stalls and a duplicated one breaks the count.
 */
static os_event_t    test_smp_event;
static __IO bool     test_smp_event_ok     = true;
static __IO uint32_t test_smp_event_rounds = 0U;

static void test_smp_event_waiter_entry(void *context)
{
    uint32_t round;

    (void)context;

    for (round = 0U; round < TEST_SMP_EVENT_ROUNDS; round++)
    {
        uint32_t matched = 0U;

        if (os_event_wait_bits(&test_smp_event, 0x01U, true, true, &matched,
                               OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            test_smp_event_ok = false;
            return;
        }

        if (matched != 0x01U)
        {
            test_smp_event_ok = false;
        }

        test_smp_event_rounds++;

        if (os_event_set_bits(&test_smp_event, 0x02U) != OS_ERR_NONE)
        {
            test_smp_event_ok = false;
            return;
        }
    }
}

static void test_smp_event_setter_entry(void *context)
{
    uint32_t round;

    (void)context;

    for (round = 0U; round < TEST_SMP_EVENT_ROUNDS; round++)
    {
        uint32_t matched = 0U;

        if (os_event_wait_bits(&test_smp_event, 0x02U, true, true, &matched,
                               OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            test_smp_event_ok = false;
            return;
        }

        if (matched != 0x02U)
        {
            test_smp_event_ok = false;
        }

        if (os_event_set_bits(&test_smp_event, 0x01U) != OS_ERR_NONE)
        {
            test_smp_event_ok = false;
            return;
        }
    }
}

static void test_smp_event_pingpong(void)
{
    test_print_section("Multi-core (SMP): event bits hand off across cores");

    test_smp_event_ok     = true;
    test_smp_event_rounds = 0U;

    AHURA_TEST_CHECK(os_event_init(&test_smp_event) == OS_ERR_NONE, "event group initialized");

    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_event_waiter_entry, NULL, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "bit waiter pinned to core 0 created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_b,
                     OS_TASK_CONFIG(test_smp_event_setter_entry, NULL, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "bit setter pinned to core 1 created");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "waiter started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_b) == OS_ERR_NONE, "setter started");

    /* The setter's first wait needs the opening bit. */
    AHURA_TEST_CHECK(os_event_set_bits(&test_smp_event, 0x02U) == OS_ERR_NONE, "opening bit set");

    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_b, 2000U), "setter finished its rounds");
    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_a, 2000U), "waiter finished its rounds");

    AHURA_TEST_CHECK(test_smp_event_rounds == TEST_SMP_EVENT_ROUNDS,
                     "every one of the %u rounds was consumed exactly once (%lu)",
                     (unsigned)TEST_SMP_EVENT_ROUNDS, (unsigned long)test_smp_event_rounds);
    AHURA_TEST_CHECK(test_smp_event_ok,
                     "each wait matched exactly the expected bit - no lost or duplicated wake");
}
#endif /* OS_CONFIG_EVENT_ENABLE */

/******************************************************************************************************/
/**
 * @brief Affinity migration: a task created pinned to core 1 is re-pinned to core 0 while
 *        BLOCKED, and its next wake must dispatch it on the core the new mask names.
 */
static __IO uint32_t test_smp_migration_phase[2][TEST_SMP_MIGRATION_SAMPLES];
static __IO uint32_t test_smp_migration_done[2]  = { 0U, 0U };

static void test_smp_migration_entry(void *context)
{
    uint32_t value = 0U;
    uint32_t i;

    (void)context;

    /* Phase 0: created pinned to core 1. */
    if (os_notify_wait(OS_WAIT_FOREVER, &value) != OS_ERR_NONE)
    {
        return;
    }

    for (i = 0U; i < TEST_SMP_MIGRATION_SAMPLES; i++)
    {
        test_smp_migration_phase[0][i] = os_arch_core_id_get();
    }

    test_smp_migration_done[0] = 1U;

    /* Phase 1: the affinity has been moved while this task was blocked, so this wake must land
     * on the core the new mask names - that dispatch decision is what is under test. */
    if (os_notify_wait(OS_WAIT_FOREVER, &value) != OS_ERR_NONE)
    {
        return;
    }

    for (i = 0U; i < TEST_SMP_MIGRATION_SAMPLES; i++)
    {
        test_smp_migration_phase[1][i] = os_arch_core_id_get();
    }

    test_smp_migration_done[1] = 1U;
}

static void test_smp_migration(void)
{
    uint32_t waited;
    uint32_t i;
    bool     phase0_ok;
    bool     phase1_ok;

    test_print_section("Multi-core (SMP): affinity change migrates a blocked task");

    test_smp_migration_done[0] = 0U;
    test_smp_migration_done[1] = 0U;

    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_migration_entry, NULL, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "migrating task created pinned to core 1");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "migrating task started");

    AHURA_TEST_CHECK(os_notify_give(&test_smp_a, 1U) == OS_ERR_NONE, "phase 0 released");

    for (waited = 0U; (waited < TEST_MC_START_TIMEOUT_MS) && (test_smp_migration_done[0] == 0U); waited++)
    {
        os_delay_ms(1U);
    }

    phase0_ok = true;
    for (i = 0U; i < TEST_SMP_MIGRATION_SAMPLES; i++)
    {
        if (test_smp_migration_phase[0][i] != 1U) { phase0_ok = false; }
    }
    AHURA_TEST_CHECK(phase0_ok, "every phase-0 sample ran on core 1, as pinned");

    AHURA_TEST_CHECK(os_task_core_affinity_set(&test_smp_a, OS_TASK_CORE(0)) == OS_ERR_NONE,
                     "affinity moved to core 0 while the task was blocked");

    AHURA_TEST_CHECK(os_notify_give(&test_smp_a, 2U) == OS_ERR_NONE, "phase 1 released");

    for (waited = 0U; (waited < TEST_MC_START_TIMEOUT_MS) && (test_smp_migration_done[1] == 0U); waited++)
    {
        os_delay_ms(1U);
    }

    AHURA_TEST_CHECK(test_wait_inactive(&test_smp_a, 1000U), "migrating task terminated cleanly");

    phase1_ok = true;
    for (i = 0U; i < TEST_SMP_MIGRATION_SAMPLES; i++)
    {
        if (test_smp_migration_phase[1][i] != 0U) { phase1_ok = false; }
    }
    AHURA_TEST_CHECK(phase1_ok, "every phase-1 sample ran on core 0 - the wake honoured the new mask");
}

/******************************************************************************************************/
/**
 * @brief os_kernel_lock is per-core by design: core 0 holding it must not delay core 1's own
 *        scheduler by one tick. The heartbeat workers parked by test_multicore() are the witness.
 */
static void test_smp_lock_independent(void)
{
    uint32_t before;
    uint32_t after;
    uint32_t start;

    test_print_section("Multi-core (SMP): kernel lock on one core never stops the other");

    before = test_mc_alive[1];

    os_kernel_lock();
    start = os_tick_get();
    while ((os_tick_get() - start) < 150U)
    {
    }
    os_kernel_unlock();

    after = test_mc_alive[1];

    AHURA_TEST_CHECK(after > before,
                     "core 1 kept scheduling while core 0 held the kernel lock for 150 ms (%lu beats)",
                     (unsigned long)(after - before));
}

/******************************************************************************************************/
/**
 * @brief Both cores churn create/start/exit on their own worker tasks at once. Each helper exits
 *        immediately, so every cycle exercises the shared task table and ready lists from two
 *        cores against each other.
 */
static __IO uint32_t test_smp_churn_done[2] = { 0U, 0U };
static __IO uint32_t test_smp_churn_errs[2] = { 0U, 0U };
static __IO uint32_t test_smp_churn_runs    = 0U;

static void test_smp_churn_worker_entry(void *context)
{
    (void)context;
    test_smp_churn_runs++;
}

static void test_smp_churn_entry(void *context)
{
    uint32_t  slot      = (uint32_t)(uintptr_t)context;
    os_task_t *transient = (slot == 0U) ? &test_smp_e : &test_smp_f;
    uint32_t  mask      = (slot == 0U) ? OS_TASK_CORE(0) : OS_TASK_CORE(1);
    uint32_t  i;

    for (i = 0U; i < TEST_SMP_CHURN_CYCLES; i++)
    {
        if (os_task_create(transient,
                           OS_TASK_CONFIG(test_smp_churn_worker_entry, NULL, TEST_PRIO_HIGH,
                                          mask)) != OS_ERR_NONE)
        {
            test_smp_churn_errs[slot]++;
            return;
        }

        if (os_task_start(transient) != OS_ERR_NONE)
        {
            test_smp_churn_errs[slot]++;
            return;
        }

        /* The worker exits on its own the moment it runs; no cross-core delete is ever needed. */
        while (os_task_state_get(transient) != OS_TASK_STATE_INACTIVE)
        {
        }
    }

    test_smp_churn_done[slot] = 1U;
}

static void test_smp_task_churn(void)
{
    uint32_t waited;

    test_print_section("Multi-core (SMP): task create/start/exit churn on both cores at once");

    test_smp_churn_done[0] = 0U;
    test_smp_churn_done[1] = 0U;
    test_smp_churn_errs[0] = 0U;
    test_smp_churn_errs[1] = 0U;
    test_smp_churn_runs    = 0U;

    AHURA_TEST_CHECK(os_task_create(&test_smp_c,
                     OS_TASK_CONFIG(test_smp_churn_entry, (void *)0U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "core-0 churner created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_d,
                     OS_TASK_CONFIG(test_smp_churn_entry, (void *)1U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "core-1 churner created");

    AHURA_TEST_CHECK(os_task_start(&test_smp_c) == OS_ERR_NONE, "core-0 churner started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_d) == OS_ERR_NONE, "core-1 churner started");

    for (waited = 0U; (waited < 3000U) &&
                      ((test_smp_churn_done[0] == 0U) || (test_smp_churn_done[1] == 0U)); waited++)
    {
        os_delay_ms(1U);
    }

    AHURA_TEST_CHECK((test_smp_churn_done[0] == 1U) && (test_smp_churn_done[1] == 1U),
                     "both churners finished %u create/start/exit cycles each",
                     (unsigned)TEST_SMP_CHURN_CYCLES);
    AHURA_TEST_CHECK((test_smp_churn_errs[0] == 0U) && (test_smp_churn_errs[1] == 0U),
                     "no create or start failed on either core");
    AHURA_TEST_CHECK(test_smp_churn_runs == (TEST_SMP_CHURN_CYCLES * 2U),
                     "every spawned worker ran exactly once (%lu of %u)",
                     (unsigned long)test_smp_churn_runs, (unsigned)(TEST_SMP_CHURN_CYCLES * 2U));
}

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Deferred calls submitted from both cores at once. The timer service task (pinned to
 *        core 0) delivers every one of them, so the callback itself records which core it ran on.
 */
static void test_smp_submit_cb(void *context, uint32_t value);

OS_TIMER_DEFINE_SUBMIT(test_smp_pool, 24U, 0U, test_smp_submit_cb);

static __IO uint32_t test_smp_submit_runs       = 0U;
static __IO bool     test_smp_submit_ok         = true;
static __IO uint32_t test_smp_submit_done[2]    = { 0U, 0U };

static void test_smp_submit_cb(void *context, uint32_t value)
{
    (void)context;

    if ((os_arch_core_id_get() != 0U) || (value >= (TEST_SMP_SUBMIT_EACH * 2U)))
    {
        test_smp_submit_ok = false;
    }

    test_smp_submit_runs++;
}

static void test_smp_submit_entry(void *context)
{
    uint32_t slot = (uint32_t)(uintptr_t)context;
    uint32_t i;

    for (i = 0U; i < TEST_SMP_SUBMIT_EACH; i++)
    {
        if (os_timer_submit(&test_smp_pool, NULL, (slot * TEST_SMP_SUBMIT_EACH) + i) != OS_ERR_NONE)
        {
            test_smp_submit_ok = false;
            test_smp_submit_done[slot] = 1U;
            return;
        }

        /* Let the timer task drain between submissions, so the pool - smaller than the two
         * sides' combined burst on purpose - rejects nothing through backpressure. */
        os_task_yield();
    }

    test_smp_submit_done[slot] = 1U;
}

static void test_smp_deferred_submit(void)
{
    uint32_t waited;

    test_print_section("Multi-core (SMP): deferred calls submitted from both cores");

    test_smp_submit_runs    = 0U;
    test_smp_submit_ok      = true;
    test_smp_submit_done[0] = 0U;
    test_smp_submit_done[1] = 0U;

    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_submit_entry, (void *)0U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "core-0 submitter created");
    AHURA_TEST_CHECK(os_task_create(&test_smp_b,
                     OS_TASK_CONFIG(test_smp_submit_entry, (void *)1U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "core-1 submitter created");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "core-0 submitter started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_b) == OS_ERR_NONE, "core-1 submitter started");

    for (waited = 0U; (waited < 3000U) &&
                      ((test_smp_submit_done[0] == 0U) || (test_smp_submit_done[1] == 0U)); waited++)
    {
        os_delay_ms(1U);
    }

    os_delay_ms(100U); /* let the last queued calls drain */

    AHURA_TEST_CHECK((test_smp_submit_done[0] == 1U) && (test_smp_submit_done[1] == 1U),
                     "both submitters queued %u calls each", (unsigned)TEST_SMP_SUBMIT_EACH);
    AHURA_TEST_CHECK(test_smp_submit_runs == (TEST_SMP_SUBMIT_EACH * 2U),
                     "every deferred call ran exactly once (%lu of %u)",
                     (unsigned long)test_smp_submit_runs, (unsigned)(TEST_SMP_SUBMIT_EACH * 2U));
    AHURA_TEST_CHECK(test_smp_submit_ok,
                     "and each one ran on the core-0 timer task, carrying its own value");
}
#endif /* OS_CONFIG_TIMER_ENABLE */

#if (OS_CONFIG_SEM_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_ATOMIC_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief A mixed workload from four tasks, two per core: guarded increments, atomic increments,
 *        a semaphore give/take pair and a queue round-trip every iteration, with hard exact
 *        accounting at the end.
 */
static __IO uint32_t test_smp_soak_guarded = 0U;
static os_atomic_t  test_smp_soak_atomic   = OS_ATOMIC_INIT(0);
static os_sem_t test_smp_soak_sem;
OS_QUEUE_DEFINE_STATIC_ATTR(test_smp_soak_queue, uint32_t, 4, );
static __IO uint32_t test_smp_soak_done[4] = { 0U, 0U, 0U, 0U };
static __IO uint32_t test_smp_soak_seen[4] = { 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU };
static __IO bool     test_smp_soak_ok      = true;

static void test_smp_soak_entry(void *context)
{
    uint32_t slot = (uint32_t)(uintptr_t)context;
    uint32_t item = slot + 1U;
    uint32_t i;

    test_smp_soak_seen[slot] = os_arch_core_id_get();

    for (i = 0U; i < TEST_SMP_SOAK_ITERATIONS; i++)
    {
        uint32_t received = 0U;

        os_critical_enter();
        test_smp_soak_guarded = test_smp_soak_guarded + 1U;
        os_critical_exit();

        (void)os_atomic_inc(&test_smp_soak_atomic);

        if (os_sem_give(&test_smp_soak_sem) != OS_ERR_NONE)
        {
            test_smp_soak_ok = false;
            return;
        }

        if (os_sem_take(&test_smp_soak_sem, OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            test_smp_soak_ok = false;
            return;
        }

        if (os_queue_send(&test_smp_soak_queue, &item, OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            test_smp_soak_ok = false;
            return;
        }

        if (os_queue_receive(&test_smp_soak_queue, &received, OS_WAIT_FOREVER) != OS_ERR_NONE)
        {
            test_smp_soak_ok = false;
            return;
        }

        if (received != item)
        {
            test_smp_soak_ok = false;
        }
    }

    test_smp_soak_done[slot] = 1U;
}

static void test_smp_soak_mixed(void)
{
    uint32_t waited;
    uint32_t expected = TEST_SMP_SOAK_ITERATIONS * 4U;
    uint32_t ignored  = 0U;

    test_print_section("Multi-core (SMP): mixed workload soak, four tasks across both cores");

    test_smp_soak_guarded = 0U;
    test_smp_soak_ok      = true;

    for (waited = 0U; waited < 4U; waited++)
    {
        test_smp_soak_done[waited] = 0U;
        test_smp_soak_seen[waited] = 0xFFFFFFFFU;
    }

    AHURA_TEST_CHECK(os_sem_init(&test_smp_soak_sem, 0U, 1U) == OS_ERR_NONE, "soak semaphore initialized");

    AHURA_TEST_CHECK(os_task_create(&test_smp_a,
                     OS_TASK_CONFIG(test_smp_soak_entry, (void *)0U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "soak worker 0 created on core 0");
    AHURA_TEST_CHECK(os_task_create(&test_smp_b,
                     OS_TASK_CONFIG(test_smp_soak_entry, (void *)1U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(0))) == OS_ERR_NONE,
                     "soak worker 1 created on core 0");
    AHURA_TEST_CHECK(os_task_create(&test_smp_c,
                     OS_TASK_CONFIG(test_smp_soak_entry, (void *)2U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "soak worker 2 created on core 1");
    AHURA_TEST_CHECK(os_task_create(&test_smp_d,
                     OS_TASK_CONFIG(test_smp_soak_entry, (void *)3U, TEST_PRIO_HIGH,
                                    OS_TASK_CORE(1))) == OS_ERR_NONE,
                     "soak worker 3 created on core 1");

    AHURA_TEST_CHECK(os_task_start(&test_smp_a) == OS_ERR_NONE, "worker 0 started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_b) == OS_ERR_NONE, "worker 1 started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_c) == OS_ERR_NONE, "worker 2 started");
    AHURA_TEST_CHECK(os_task_start(&test_smp_d) == OS_ERR_NONE, "worker 3 started");

    for (waited = 0U; (waited < 5000U) &&
                      ((test_smp_soak_done[0] == 0U) || (test_smp_soak_done[1] == 0U) ||
                       (test_smp_soak_done[2] == 0U) || (test_smp_soak_done[3] == 0U)); waited++)
    {
        os_delay_ms(1U);
    }

    AHURA_TEST_CHECK((test_smp_soak_done[0] == 1U) && (test_smp_soak_done[1] == 1U) &&
                     (test_smp_soak_done[2] == 1U) && (test_smp_soak_done[3] == 1U),
                     "all four workers finished %u iterations each", (unsigned)TEST_SMP_SOAK_ITERATIONS);
    AHURA_TEST_CHECK((test_smp_soak_seen[0] == 0U) && (test_smp_soak_seen[1] == 0U) &&
                     (test_smp_soak_seen[2] == 1U) && (test_smp_soak_seen[3] == 1U),
                     "each worker ran on the core it was pinned to");
    AHURA_TEST_CHECK(test_smp_soak_guarded == expected,
                     "guarded counter is exact across four tasks on two cores (%lu of %lu)",
                     (unsigned long)test_smp_soak_guarded, (unsigned long)expected);
    AHURA_TEST_CHECK(os_atomic_get(&test_smp_soak_atomic) == (int32_t)expected,
                     "atomic counter is exact too (%ld of %lu)",
                     (long)os_atomic_get(&test_smp_soak_atomic), (unsigned long)expected);
    AHURA_TEST_CHECK(os_sem_take(&test_smp_soak_sem, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                     "the semaphore ended empty");
    AHURA_TEST_CHECK(os_queue_receive(&test_smp_soak_queue, &ignored, OS_WAIT_NOTHING) == OS_ERR_EMPTY,
                     "the queue ended empty");
    AHURA_TEST_CHECK(test_smp_soak_ok,
                     "every queue round-trip returned its own item and every call succeeded");
}
#endif /* SEM && QUEUE && ATOMIC */

#endif /* OS_CONFIG_CORE_COUNT > 1U */

/******************************************************************************************************/
static void test_unsupported_features(void)
{
    test_print_section("Multi-core / TrustZone / Tickless (config-gated, informational)");

#if (OS_CONFIG_CORE_COUNT > 1U)
    printf("  [INFO] multi-core APIs compiled in (OS_CONFIG_CORE_COUNT=%u) - exercised above\r\n",
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
    printf("  [INFO] tickless idle enabled - not functionally wired in yet (doc/porting.md)\r\n");
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

#if (OS_CONFIG_SEM_ENABLE == 1U)
static os_sem_t os_test_reg_sem;
static __IO uint32_t  os_test_reg_order   = 0U;
static __IO uint32_t  os_test_reg_a_order = 0U;
static __IO os_err_t os_test_reg_a_st    = OS_ERR_ERROR;
static __IO os_err_t os_test_reg_b_st    = OS_ERR_ERROR;

/******************************************************************************************************/
static void test_reg_waiter_b(void *context)
{
    (void)context;
    os_test_reg_b_st = os_sem_take(&os_test_reg_sem, 400U);
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

    if (os_mutex_lock(&os_test_reg_mutex, 200U) != OS_ERR_NONE)
    {
        return;
    }

    os_test_reg_a_st    = os_sem_take(&os_test_reg_sem, 400U);
    os_test_reg_a_order = ++os_test_reg_order;

    (void)os_mutex_unlock(&os_test_reg_mutex);
}

/* Highest priority: contends the mutex purely to trigger the inheritance boost. */
/******************************************************************************************************/
static void test_reg_booster_entry(void *context)
{
    (void)context;

    if (os_mutex_lock(&os_test_reg_mutex, 400U) == OS_ERR_NONE)
    {
        (void)os_mutex_unlock(&os_test_reg_mutex);
    }
}
#endif /* OS_CONFIG_MUTEX_ENABLE */
#endif /* OS_CONFIG_SEM_ENABLE */

#if (OS_CONFIG_TIMER_ENABLE == 1U)
/******************************************************************************************************/
static void test_reg_timer_cb(void *context, uint32_t value)
{
    (void)value;
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
    os_err_t          status;

    test_print_section("Task Priority (get / set)");

    status = os_task_create(&worker, TEST_TASK_CONFIG(test_worker_entry, NULL, OS_TASK_PRIO_2));
    AHURA_TEST_CHECK(status == OS_ERR_NONE, "worker created at priority 2");

    AHURA_TEST_CHECK(os_task_priority_get(&worker, &priority) == OS_ERR_NONE, "priority read back");
    AHURA_TEST_CHECK(priority == OS_TASK_PRIO_2, "it reports what creation asked for (%u)",
                      (unsigned)priority);

    AHURA_TEST_CHECK(os_task_priority_set(&worker, OS_TASK_PRIO_5) == OS_ERR_NONE, "priority raised to 5");
    (void)os_task_priority_get(&worker, &priority);
    AHURA_TEST_CHECK(priority == OS_TASK_PRIO_5, "the new priority is what comes back (%u)",
                      (unsigned)priority);

    /* The suite's own task must still be able to read and restore its own priority. */
    /* NULL means the calling task across the task API, so the one call that used to refuse it
     * must now accept it too. */
#if (OS_CONFIG_CORE_COUNT > 1U)
    AHURA_TEST_CHECK(os_task_core_affinity_set(NULL, 0U) == OS_ERR_NONE,
                      "os_task_core_affinity_set(NULL) targets the calling task");
#endif
    AHURA_TEST_CHECK(os_task_state_get(NULL) == OS_TASK_STATE_RUNNING,
                      "os_task_state_get(NULL) reports the calling task as RUNNING");

    AHURA_TEST_CHECK(os_task_priority_get(NULL, &priority) == OS_ERR_NONE,
                      "NULL means the calling task");
    AHURA_TEST_CHECK(priority == (os_task_priority_t)OS_CONFIG_TEST_PRIORITY,
                      "and reports the suite's own configured priority (%u)", (unsigned)priority);

    /* Levels outside the user range belong to the idle task and the kernel's service tasks. */
    AHURA_TEST_CHECK(os_task_priority_set(&worker, (os_task_priority_t)0U) == OS_ERR_INVALID_ARG,
                      "priority 0 (idle) is refused");
    AHURA_TEST_CHECK(os_task_priority_set(&worker, OS_TASK_PRIO_MAX) == OS_ERR_INVALID_ARG,
                      "OS_TASK_PRIO_MAX (kernel service level) is refused");
    AHURA_TEST_CHECK(os_task_priority_get(&worker, NULL) == OS_ERR_INVALID_ARG,
                      "a NULL output pointer is refused");

    /* A refused set must not have changed anything. */
    (void)os_task_priority_get(&worker, &priority);
    AHURA_TEST_CHECK(priority == OS_TASK_PRIO_5, "a refused set left the priority alone (%u)",
                      (unsigned)priority);

    (void)os_task_delete(&worker);

    {
        os_task_t stale = { 0 };

        AHURA_TEST_CHECK(os_task_priority_get(&stale, &priority) == OS_ERR_INVALID_ARG,
                          "an unknown handle is refused by get");
        AHURA_TEST_CHECK(os_task_priority_set(&stale, OS_TASK_PRIO_3) == OS_ERR_INVALID_ARG,
                          "an unknown handle is refused by set");
    }

    /* A change takes effect at once, not at the next dispatch. The helper runs to completion and
     * exits on its own, so it can be raised above this task without being able to starve it. */
    os_test_prio_ran = 0U;
    (void)os_task_create(&worker, TEST_TASK_CONFIG(test_prio_entry, NULL, TEST_PRIO_LOW));
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
    const size_t capacity = sizeof(os_test_queue_queue_buf) / sizeof(os_test_queue_queue_buf[0]);
    uint32_t     item     = 0U;
    size_t       sent     = 0U;
    bool         held     = true;

    test_print_section("Queue Accounting (count / free)");

    (void)os_queue_cleanup(&os_test_queue);   /* empties it; the array is not the queue's to free */

    AHURA_TEST_CHECK(os_queue_count_get(&os_test_queue) == 0U, "an emptied queue holds nothing");
    AHURA_TEST_CHECK(os_queue_free_get(&os_test_queue) == capacity,
                      "and reports its whole capacity free (%lu)", (unsigned long)capacity);

    while (os_queue_send(&os_test_queue, &item, OS_WAIT_NOTHING) == OS_ERR_NONE)
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
    while (os_queue_receive(&os_test_queue, &item, OS_WAIT_NOTHING) == OS_ERR_NONE)
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

#if (OS_CONFIG_SEM_ENABLE == 1U)
    /* An unconsumed wake is handed on, not lost with the task that never used it. */
    {
        os_test_reg_b_st = OS_ERR_ERROR;
        (void)os_sem_init(&os_test_reg_sem, 0U, 2U);

        (void)os_task_create(&helper, TEST_TASK_CONFIG(test_reg_waiter_b, NULL, TEST_PRIO_LOW));
        (void)os_task_start(&helper);
        (void)os_task_create(&helper2, TEST_TASK_CONFIG(test_reg_waiter_b, NULL, TEST_PRIO_LOW));
        (void)os_task_start(&helper2);
        os_delay_ms(20U);   /* both are below this task, so they queue while it sleeps */

        /* The window: give() wakes the first waiter, the lock stops it running, and the pause
         * removes it before it can consume the token. */
        os_kernel_lock();
        (void)os_sem_give(&os_test_reg_sem);
        (void)os_task_pause(&helper);
        os_kernel_unlock();

        os_delay_ms(40U);
        AHURA_TEST_CHECK(os_test_reg_b_st == OS_ERR_NONE, "paused task's wake passed to the next waiter");
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
        os_test_reg_a_st    = OS_ERR_ERROR;
        os_test_reg_b_st    = OS_ERR_ERROR;

        (void)os_sem_init(&os_test_reg_sem, 0U, 2U);
        (void)os_mutex_init(&os_test_reg_mutex);

        (void)os_task_create(&helper, TEST_TASK_CONFIG(test_reg_boosted_entry, NULL, OS_TASK_PRIO_1));
        (void)os_task_start(&helper);
        os_delay_ms(20U);   /* holds the mutex, now queued on the semaphore */

        /* Higher priority, so the waiter list puts it AHEAD of the low-priority holder. */
        (void)os_task_create(&helper2, TEST_TASK_CONFIG(test_reg_waiter_b, NULL, OS_TASK_PRIO_2));
        (void)os_task_start(&helper2);
        os_delay_ms(20U);

        /* Contending the mutex boosts its owner above that waiter. */
        (void)os_task_create(&helper3, TEST_TASK_CONFIG(test_reg_booster_entry, NULL, OS_TASK_PRIO_3));
        (void)os_task_start(&helper3);
        os_delay_ms(20U);

        (void)os_sem_give(&os_test_reg_sem);
        os_delay_ms(30U);

        AHURA_TEST_CHECK(os_test_reg_a_order == 1U, "boosted waiter woken first (order=%lu)",
                          (unsigned long)os_test_reg_a_order);

        (void)os_sem_give(&os_test_reg_sem);
        AHURA_TEST_CHECK(test_wait_inactive(&helper, 500U) && test_wait_inactive(&helper2, 500U) &&
                          test_wait_inactive(&helper3, 500U), "all three helpers finished");
    }
#endif /* OS_CONFIG_MUTEX_ENABLE */
#endif /* OS_CONFIG_SEM_ENABLE */

#if (OS_CONFIG_TIMER_ENABLE == 1U)
    /* Restart must discard an expiry the tick noted but the timer task has not drained. */
    {
        os_test_oneshot_fired = 0U;
        (void)os_timer_period_set(&os_test_timer_oneshot, 30U);
        (void)os_timer_start(&os_test_timer_oneshot, NULL, 0U);

        /* The lock keeps the timer service task off the CPU while the tick keeps running, so the
         * expiry is flagged and left undrained - the state a restart used to inherit. */
        os_kernel_lock();
        os_delay_ms(45U);   /* busy-waits under the lock; ticks still arrive */
        (void)os_timer_restart(&os_test_timer_oneshot, NULL, 0U);
        os_kernel_unlock();

        os_delay_ms(15U);
        AHURA_TEST_CHECK(os_test_oneshot_fired == 0U, "restart dropped the undrained expiry");
        os_delay_ms(40U);
        AHURA_TEST_CHECK(os_test_oneshot_fired == 1U, "and fired a full period later");
        (void)os_timer_stop(&os_test_churn_timer);
    }

    /* Stopping a started timer must return its registry slot for someone else to take. */
    {
        uint32_t index;

        for (index = 0U; index < TEST_TIMER_SET; index++)
        {
            /* Repointed at this section's own callback, which is what os_timer_callback_set is
             * for now that a timer's job is otherwise fixed where it is defined. Far longer than
             * this section runs, so none of them can fire and disturb it. */
            (void)os_timer_callback_set(os_test_tflood[index], test_reg_timer_cb);
            (void)os_timer_period_set(os_test_tflood[index], 60000U);
            (void)os_timer_start(os_test_tflood[index], NULL, 0U);
        }

        (void)os_timer_period_set(&os_test_tflood_extra, 60000U);
        AHURA_TEST_CHECK(os_timer_start(&os_test_tflood_extra, NULL, 0U) == OS_ERR_NONE,
                          "starting one more than the working set is fine");

        (void)os_timer_stop(os_test_tflood[0]);
        AHURA_TEST_CHECK(os_timer_start(os_test_tflood[0], NULL, 0U) == OS_ERR_NONE,
                          "and a stopped timer re-enters the running list cleanly");

        (void)os_timer_stop(&os_test_tflood_extra);
        for (index = 0U; index < TEST_TIMER_SET; index++)
        {
            (void)os_timer_stop(os_test_tflood[index]);
            (void)os_timer_callback_set(os_test_tflood[index], test_tflood_cb);
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
    /* Version first: a log pasted into a bug report has to say which kernel produced it, and the
     * banner is the one line that always survives the copy/paste. OS_VERSION_STRING is a string
     * literal, so it concatenates here rather than costing a format argument. */
    printf("\r\n========================================\r\n");
    printf(" Ahura RTOS v" OS_VERSION_STRING " self-test starting...\r\n");
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
#if (OS_CONFIG_SEM_ENABLE == 1U)
    test_semaphore();
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    test_queue();
    test_queue_define_and_dynamic();
#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
    test_atomic();
#endif
#endif
#if (OS_CONFIG_MSG_ENABLE == 1U)
    test_msg();
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
    test_event_group();
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    test_timer();
#endif
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    test_timer_isr();
    test_timer_pool();
    test_timer_real_world();
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
#if (OS_CONFIG_MUTEX_ENABLE == 1U) && (OS_CONFIG_SEM_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && \
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
#if (OS_CONFIG_SEM_ENABLE == 1U)
    test_stress_semaphore_pingpong();
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    test_stress_notify_storm();
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
    test_stress_event_bit_storm();
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
#if (OS_CONFIG_CORE_COUNT > 1U)
    /* Multi-core comes LAST, on purpose. Every section above exercises the kernel one subsystem
     * at a time, and that is the foundation the SMP questions stand on: once the whole
     * single-core surface has passed, the suite asks whether a second core starts, reports its
     * own id, honours affinity and shares the spinlock correctly - and then drives the cross-core
     * seams hard (contention, wake integrity, migration, churn, a mixed soak) before watching
     * both parked workers for several heartbeats to prove the second core SURVIVED the whole
     * run, not just its own section. */
    test_multicore();
    test_smp_critical_nested();
#if (OS_CONFIG_ATOMIC_ENABLE == 1U)
    test_smp_atomic_contention();
#endif
#if (OS_CONFIG_NOTIFY_ENABLE == 1U)
    test_smp_notify_pingpong();
#endif
#if (OS_CONFIG_SEM_ENABLE == 1U)
    test_smp_semaphore_pingpong();
#endif
#if (OS_CONFIG_QUEUE_ENABLE == 1U)
    test_smp_queue_accounting();
#endif
#if (OS_CONFIG_EVENT_ENABLE == 1U)
    test_smp_event_pingpong();
#endif
    test_smp_migration();
    test_smp_lock_independent();
    test_smp_task_churn();
#if (OS_CONFIG_TIMER_ENABLE == 1U)
    test_smp_deferred_submit();
#endif
#if (OS_CONFIG_SEM_ENABLE == 1U) && (OS_CONFIG_QUEUE_ENABLE == 1U) && (OS_CONFIG_ATOMIC_ENABLE == 1U)
    test_smp_soak_mixed();
#endif
    test_multicore_watch(TEST_MC_WATCH_MS, "at the end of the whole run");
#endif
    test_unsupported_features();

    /* Repeated from the banner on purpose: the result block is what gets screenshotted or pasted
     * on its own, and a pass count means nothing without the version that produced it. */
    printf("\r\n========================================\r\n");
    printf(" Ahura RTOS v" OS_VERSION_STRING "\r\n");
    printf(" RESULT: %lu passed, %lu failed (of %lu checks)\r\n", (unsigned long)os_test_pass_count,
           (unsigned long)os_test_fail_count, (unsigned long)(os_test_pass_count + os_test_fail_count));
    printf("%s\r\n", (os_test_fail_count == 0U) ? " ALL RTOS FEATURES VERIFIED OK" : " SOME CHECKS FAILED - see log above");
    printf("========================================\r\n");

    /* Last, so the timings are the final thing on the console and are not interleaved with
     * PASS/FAIL lines: benchmarks report numbers, they do not pass or fail. */
    test_benchmarks();
    printf("\r\n");
}
