/**
 * @file ahura.h
 * @brief Ahura kernel umbrella public API.
 *
 * Laid out in two parts, most important first:
 *
 *   PART 1  ALWAYS AVAILABLE - types, task/time/critical-section API and the
 *           intrusive list. No configuration option removes any of it, so
 *           anything declared here can be used unconditionally.
 *   PART 2  CONFIGURABLE - one group per OS_CONFIG_ option, in the same order
 *           the options appear in os_config.h. Each group sits behind exactly
 *           one guard covering its types, macros and functions together, so a
 *           disabled feature takes its whole API surface with it.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef AHURA_H
#define AHURA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* os_arch_port.h includes and validates the application's os_config.h
 * (copy kernel/template/os_config.h, see README "Configuration"). */
#include "os_arch_port.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Portability
 * ***********************************************************************************************************
*/

/* The compile-time assertion, spelled the way the language in use spells it.
 *
 * The kernel is C11, where it is _Static_assert. C++ has had static_assert as a keyword since
 * C++11 and does not declare _Static_assert at all, so a C++ application including this header
 * would fail on the assertions below and inside OS_QUEUE_DEFINE_BUFFER, OS_TIMER_DEFINE and
 * OS_DEFERRED_POOL_DEFINE. The extern "C" block above keeps the LINKAGE right and says nothing
 * about the syntax, which is why the guard alone was not enough. Both forms take the same two
 * arguments, so this is a rename and nothing more. */
#ifdef __cplusplus
#define OS_STATIC_ASSERT(condition, message)    static_assert(condition, message)
#else
#define OS_STATIC_ASSERT(condition, message)    _Static_assert(condition, message)
#endif

/*
 * ***********************************************************************************************************
 * Kernel version
 * ***********************************************************************************************************
*/

/* MAJOR.MINOR.PATCH, semantic versioning: MAJOR for a breaking API change, MINOR for additions that
 * keep existing code compiling, PATCH for fixes that change no interface.
 *
 * Plain ints, deliberately: no U suffix, because OS_VERSION_STRING stringifies these very tokens and
 * a suffix would come out literally as "0U.0U.0U". They are only ever compared against small
 * constants, so nothing here needs the unsigned type. */
#define OS_VERSION_MAJOR                0
#define OS_VERSION_MINOR                0
#define OS_VERSION_PATCH                0

/* Two steps, and both are necessary. # freezes its argument before macro expansion, so a
 * single-step version of this would yield "OS_VERSION_MAJOR" instead of "0"; the outer macro exists
 * only to force the argument through one expansion first. */
#define OS_VERSION_STRINGIFY_(value)    #value
#define OS_VERSION_STRINGIFY(value)     OS_VERSION_STRINGIFY_(value)

/* "0.0.0" - a string literal, so it concatenates with adjacent literals:
 *     OS_LOG_INFO("Ahura " OS_VERSION_STRING " starting"); */
#define OS_VERSION_STRING               OS_VERSION_STRINGIFY(OS_VERSION_MAJOR) "." \
                                        OS_VERSION_STRINGIFY(OS_VERSION_MINOR) "." \
                                        OS_VERSION_STRINGIFY(OS_VERSION_PATCH)

/* One ordered integer, so application code can gate on a kernel version at compile time:
 *     #if (OS_VERSION >= OS_VERSION_MAKE(1, 2, 0))
 * Eight bits per field. Past 255 it is the scheme that wants revisiting, not the number. */
#define OS_VERSION_MAKE(major, minor, patch) \
    ((((major) & 0xFF) << 16) | (((minor) & 0xFF) << 8) | ((patch) & 0xFF))

#define OS_VERSION                      OS_VERSION_MAKE(OS_VERSION_MAJOR, \
                                                        OS_VERSION_MINOR, \
                                                        OS_VERSION_PATCH)

/*
 * ***********************************************************************************************************
 * PART 1 - ALWAYS AVAILABLE (no configuration option removes any of this)
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * Status codes and task types
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Common status code for Ahura kernel APIs.
 */
typedef enum
{
    OS_ERR_NONE        = 0, /**< Operation succeeded.                          */
    OS_ERR_ERROR       = 1, /**< Generic failure.                              */
    OS_ERR_INVALID_ARG = 2, /**< A required argument was invalid or NULL.      */
    OS_ERR_EMPTY       = 3, /**< Object holds no items/tokens.                 */
    OS_ERR_FULL        = 4, /**< Object cannot accept more items/tokens.       */
    OS_ERR_BUSY        = 5, /**< Object unavailable without blocking.          */
    OS_ERR_TIMEOUT     = 6, /**< Wait aborted because the timeout elapsed.     */
    OS_ERR_NOT_OWNER   = 7, /**< Caller does not own the object.               */
    OS_ERR_NO_MEMORY   = 8, /**< Kernel heap could not satisfy the request.    */
    OS_ERR_ISR         = 9, /**< Task-only call made from interrupt context.   */

} os_err_t;

/******************************************************************************************************/
/**
 * @brief Task lifecycle state.
 */
typedef enum
{
    OS_TASK_STATE_INACTIVE  = 0, /**< Not created / deleted.                    */
    OS_TASK_STATE_READY     = 1, /**< Runnable, waiting for the CPU.            */
    OS_TASK_STATE_RUNNING   = 2, /**< Currently executing.                      */
    OS_TASK_STATE_BLOCKED   = 3, /**< Waiting for a delay/timeout to expire.    */
    OS_TASK_STATE_SUSPENDED = 4, /**< Paused until os_task_start is called.     */

} os_task_state_t;

/******************************************************************************************************/
/**
 * @brief Task entry function signature.
 */
typedef void (*os_task_entry_t)(void *context);

/******************************************************************************************************/
/**
 * @brief What a task is called and where its stack lives.
 *
 * The half of a task's definition that belongs to the handle rather than to what the task does:
 * OS_TASK_DEFINE fills one of these in at compile time and points the handle at it, which is why
 * os_task_create needs neither a name nor a stack. Const, so it costs flash rather than RAM.
 */
typedef struct
{
    const char *name;
    void       *stack_memory;
    size_t     stack_bytes;

} os_task_storage_t;

/******************************************************************************************************/
/**
 * @brief Public task handle object. Declare one with OS_TASK_DEFINE, never by hand: os_task_create
 *        refuses a handle whose storage the macro has not filled in.
 */
typedef struct
{
    uint32_t                id;
    const os_task_storage_t *storage;

} os_task_t;

/******************************************************************************************************/
/**
 * @brief Task creation parameters: what the task does, as opposed to what it is called and where
 *        its stack lives, which the handle already carries. Built with OS_TASK_CONFIG.
 */
typedef struct
{
    os_task_entry_t entry;
    void            *context;
    uint32_t        priority;
    uint32_t        core_affinity; /**< Bitmask of cores the task may run on;
                                        OS_TASK_CORE_ANY (0) = any core.
                                        Ignored on single-core builds. */

} os_task_config_t;

/*
 * ***********************************************************************************************************
 * Timeouts, task priorities and core affinity
 * ***********************************************************************************************************
*/

/** Timeout value: wait forever (never time out). */
#define OS_WAIT_FOREVER         0xFFFFFFFFU

/** Timeout value: do not wait, fail immediately when unavailable. */
#define OS_WAIT_NOTHING         0U

/** Every task priority level the scheduler has, one name per level, value N for level N.
 *
 *  The range an application may ask for is exactly OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_30_HIGHEST,
 *  and those two names ARE the limits - there is no separate pair of range macros to keep in step
 *  with them. The two levels outside that range are both kernel-owned and both named here, so the
 *  enum describes the whole scheduler rather than only the part applications touch:
 *
 *    OS_TASK_PRIO_IDLE  (0)   The idle task, one per scheduling core, created by os_init(). It is
 *                             the empty-ready-bitmap fallback and must be the only thing at this
 *                             level, or the scheduler could pick a real task when it means to idle.
 *    OS_TASK_PRIO_MAX   (31)  Above every user task, so the kernel's service tasks have a level
 *                             nothing else can claim. Where OS_CONFIG_TIMER_PRIORITY puts the
 *                             timer service by default, though it may be lowered into the user
 *                             range.
 *
 *  os_task_create() and os_task_priority_set() reject both, returning OS_ERR_INVALID_ARG.
 *
 *  Safe to enumerate directly like this because the number of levels is a fixed kernel constant
 *  (not application-configurable), so the range never changes. Using a name is a style choice:
 *  a plain number works the same, since os_task_config_t.priority is a plain uint32_t.
 *
 *  One thing a name cannot do is survive the preprocessor. An enum constant is not a macro, so
 *  #if reads it as 0 - which is why a configured priority written as a name must be checked with
 *  _Static_assert rather than #if (see os_timer.c), and why application code should not test one
 *  in #if either. */
typedef enum
{
    /* Kernel-owned, below every user task. */
    OS_TASK_PRIO_IDLE       = 0U,

    OS_TASK_PRIO_1_LOWEST   = 1U,
    OS_TASK_PRIO_1          = 1U,
    OS_TASK_PRIO_2          = 2U,
    OS_TASK_PRIO_3          = 3U,
    OS_TASK_PRIO_4          = 4U,
    OS_TASK_PRIO_5          = 5U,
    OS_TASK_PRIO_6          = 6U,
    OS_TASK_PRIO_7          = 7U,
    OS_TASK_PRIO_8          = 8U,
    OS_TASK_PRIO_9          = 9U,
    OS_TASK_PRIO_10         = 10U,
    OS_TASK_PRIO_11         = 11U,
    OS_TASK_PRIO_12         = 12U,
    OS_TASK_PRIO_13         = 13U,
    OS_TASK_PRIO_14         = 14U,
    OS_TASK_PRIO_15         = 15U,
    OS_TASK_PRIO_16         = 16U,
    OS_TASK_PRIO_17         = 17U,
    OS_TASK_PRIO_18         = 18U,
    OS_TASK_PRIO_19         = 19U,
    OS_TASK_PRIO_20         = 20U,
    OS_TASK_PRIO_21         = 21U,
    OS_TASK_PRIO_22         = 22U,
    OS_TASK_PRIO_23         = 23U,
    OS_TASK_PRIO_24         = 24U,
    OS_TASK_PRIO_25         = 25U,
    OS_TASK_PRIO_26         = 26U,
    OS_TASK_PRIO_27         = 27U,
    OS_TASK_PRIO_28         = 28U,
    OS_TASK_PRIO_29         = 29U,
    OS_TASK_PRIO_30         = 30U,
    OS_TASK_PRIO_30_HIGHEST = 30U,  /**< Highest a user task may request. */

    /* Kernel-owned, above every user task: os_task_create rejects it, and it is what
     * OS_CONFIG_TIMER_PRIORITY defaults to. */
    OS_TASK_PRIO_MAX        = 31U

} os_task_priority_t;

/* The user range must sit exactly between the two kernel-owned levels, with no gap on either side.
 * A gap would mean a level no task could ever occupy - wasted ready-list and bitmap space - and an
 * overlap would put a user task where the idle fallback or a service task belongs. Checked rather
 * than derived so that renumbering one end without the other fails to build. */
OS_STATIC_ASSERT(((uint32_t)OS_TASK_PRIO_1_LOWEST == ((uint32_t)OS_TASK_PRIO_IDLE + 1U)) &&
               ((uint32_t)OS_TASK_PRIO_30_HIGHEST == ((uint32_t)OS_TASK_PRIO_MAX - 1U)),
               "the user priority range must be contiguous with OS_TASK_PRIO_IDLE and OS_TASK_PRIO_MAX");

/* The ready bitmap is one 32-bit word, one bit per level, so the top level has to fit in it. */
OS_STATIC_ASSERT((uint32_t)OS_TASK_PRIO_MAX < 32U,
               "OS_TASK_PRIO_MAX must fit the 32-bit ready bitmap");

/** Core affinity: the task may run on any core - the empty mask, so no core is
 *  named and none is excluded. What the kernel's own idle, timer and log tasks
 *  use, and what a single-core OS_TASK_CONFIG fills in for you. */
#define OS_TASK_CORE_ANY        0U

/** Core affinity: the task may run only on core n. Combine with | for a set
 *  of allowed cores. Cores are numbered from 0, so a dual-core part is
 *  OS_TASK_CORE(0) and OS_TASK_CORE(1). */
#define OS_TASK_CORE(n)         (1UL << (n))

/*
 * ***********************************************************************************************************
 * Tick conversion
 * ***********************************************************************************************************
*/

/** Clamp a 64-bit tick count into the uint32_t tick range, one short of the OS_WAIT_FOREVER
 *  sentinel - the same saturation the kernel applies internally to every timeout it converts.
 *  A duration too large for the tick range is a duration the caller cannot have, but truncating
 *  it turns it into a small, plausible-looking one (and, at the sentinel, into "wait forever"),
 *  which no caller can detect. This is what OS_TICKS_FROM_MS below is built on; it expands
 *  its argument twice, so pass a value rather than an expression with side effects. */
#define OS_TICKS_SATURATE(ticks) ((uint32_t)(((ticks) >= (uint64_t)OS_WAIT_FOREVER) ? \
                                             ((uint64_t)OS_WAIT_FOREVER - 1ULL) : (ticks)))

#define OS_TICKS_FROM_MS(ms)    OS_TICKS_SATURATE((((uint64_t)(ms) * (uint64_t)OS_CONFIG_TICK_HZ) + 999ULL) / 1000ULL)

/*
 * ***********************************************************************************************************
 * Task declaration macros
 * ***********************************************************************************************************
*/

/* 8-byte task stack alignment. Order matters: armclang also defines __clang__,
 * and clang also defines __GNUC__, so the most specific test has to come first
 * or the later branches are dead code. */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)
#define OS_STACK_ALIGNED        __attribute__((aligned(8)))   /* Arm Compiler 6 (armclang) */
#elif defined(__clang__)
#define OS_STACK_ALIGNED        __attribute__((aligned(8)))   /* LLVM clang                */
#elif defined(__GNUC__)
#define OS_STACK_ALIGNED        __attribute__((aligned(8)))   /* GNU GCC                   */
#else
#define OS_STACK_ALIGNED
#endif

/** Define a task: its handle, its stack, and the storage descriptor tying the two together.
 *
 *  The handle is plain "task_name"; the stack gets the decorated "task_name_STACK", which nothing
 *  should name by hand. stack_size is in bytes, rounded up to a multiple of 8, and must be at
 *  least OS_CONFIG_MIN_STACK_SIZE.
 *
 *      OS_TASK_DEFINE(worker, 512U);
 *      status = os_task_create(&worker, OS_TASK_CONFIG(worker_entry, NULL, OS_TASK_PRIO_1));
 *
 *  Name and stack live in the handle rather than in os_task_create's arguments, so handing one
 *  task's handle another task's stack is no longer expressible. (Parameters are task_name and
 *  stack_size, not name and stack_bytes: a parameter named after a struct field would be
 *  substituted inside the initializers below.) */
#define OS_TASK_DEFINE(task_name, stack_size)                                        \
    static uint8_t task_name##_STACK[(((stack_size) + 7U) & ~7U)] OS_STACK_ALIGNED;  \
    static const os_task_storage_t task_name##_STORAGE = {                           \
        .name         = #task_name,                                                  \
        .stack_memory = (void *)(task_name##_STACK),                                 \
        .stack_bytes  = sizeof(task_name##_STACK)                                    \
    };                                                                               \
    static os_task_t task_name = { .storage = &task_name##_STORAGE }

/** OS_TASK_DEFINE, plus attributes on the stack - for when WHERE the stack lives matters as much
 *  as how big it is: fast on-chip RAM (DTCM, CCM), a no-init section that survives a reset, a
 *  region an MPU covers, an address the linker script pins. OS_TASK_DEFINE puts its stack in
 *  ordinary .bss with no way to say otherwise; this puts whatever you pass on that same array.
 *
 *      OS_TASK_DEFINE_ATTR(rx_task, 1024U, __attribute__((section(".dtcm"))));
 *      status = os_task_create(&rx_task, OS_TASK_CONFIG(rx_entry, NULL, OS_TASK_PRIO_3));
 *
 *  Identical to OS_TASK_DEFINE in every other way - same handle, same rounding to a multiple of
 *  8, same OS_STACK_ALIGNED already applied, so an attribute that only names a section cannot
 *  silently cost the stack its alignment. The named section still has to exist in the linker
 *  script; nothing here can create it.
 *
 *  Variadic on purpose: attributes are taken as the rest of the line, so several may be given
 *  (__attribute__((aligned(32))) __attribute__((section(".noinit")))) whatever commas they
 *  contain. */
#define OS_TASK_DEFINE_ATTR(task_name, stack_size, ...)                              \
    static uint8_t task_name##_STACK[(((stack_size) + 7U) & ~7U)]                    \
        OS_STACK_ALIGNED __VA_ARGS__;                                                \
    static const os_task_storage_t task_name##_STORAGE = {                           \
        .name         = #task_name,                                                  \
        .stack_memory = (void *)(task_name##_STACK),                                 \
        .stack_bytes  = sizeof(task_name##_STACK)                                    \
    };                                                                               \
    static os_task_t task_name = { .storage = &task_name##_STORAGE }

/** Task behaviour for os_task_create: what the task runs, with what, and at what priority. What it
 *  is called and where its stack lives came from OS_TASK_DEFINE, so neither appears here.
 *  Two forms, and the signature of each is the SAME whatever OS_CONFIG_CORE_COUNT is:
 *
 *  ONE macro, whose parameter list follows OS_CONFIG_CORE_COUNT:
 *
 *    CORE_COUNT == 1   OS_TASK_CONFIG(entry, context, priority)
 *    CORE_COUNT  > 1   OS_TASK_CONFIG(entry, context, priority, core_affinity)
 *
 *  On a single core there is nothing to decide, so affinity does not appear at all. The moment
 *  there is more than one, every task states where it runs - "anywhere" included, written as
 *  OS_TASK_CORE_ANY rather than inherited by silence. Placement is the whole design question on
 *  SMP: which core owns the ISR-adjacent work, what must not migrate, what may float. A task that
 *  never says is far more often one nobody thought about than one that genuinely does not care.
 *
 *  The cost is deliberate and worth stating plainly: raising OS_CONFIG_CORE_COUNT from 1 does not
 *  merely recompile. Every OS_TASK_CONFIG in the application stops compiling until it is given an
 *  affinity, which makes the move a real port rather than a config edit. That compile error at
 *  every creation site IS the feature - it forces each placement decision to be made once and on
 *  purpose, instead of defaulting to ANY everywhere and surfacing later as a performance problem
 *  with nothing pointing at its cause.
 *
 *  Pinning therefore stays explicit and greppable, and os_task_core_affinity_set can still change
 *  it at runtime either way. core_affinity is a bitmask (OS_TASK_CORE(n), OR-combinable;
 *  OS_TASK_CORE_ANY = any core), and bits naming cores beyond OS_CONFIG_CORE_COUNT fail with
 *  OS_ERR_INVALID_ARG rather than being ignored. Initialized POSITIONALLY on purpose: the
 *  parameters here are named after the fields they fill, and a designated initializer would have
 *  the preprocessor substitute inside ".entry" and ".priority", turning them into whatever the
 *  caller passed. */
#if (OS_CONFIG_CORE_COUNT == 1U)
#define OS_TASK_CONFIG(entry, context, priority) \
    &(os_task_config_t) { \
        (entry), \
        (context), \
        (priority), \
        OS_TASK_CORE_ANY \
    }
#else
#define OS_TASK_CONFIG(entry, context, priority, core_affinity) \
    &(os_task_config_t) { \
        (entry), \
        (context), \
        (priority), \
        (core_affinity) \
    }
#endif

/*
 * ***********************************************************************************************************
 * Kernel lifecycle
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize kernel subsystems. Call once before any other kernel API.
 */
void os_init(void);

/******************************************************************************************************/
/**
 * @brief Start the scheduler and switch to task context. Does not return.
 */
void os_start(void);

/******************************************************************************************************/
/**
 * @brief Return true once the scheduler has been started.
 */
bool os_kernel_is_running(void);

/******************************************************************************************************/
/**
 * @brief Default application task body (see OS_CONFIG_MAIN_TASK_* in os_config.h). os_init()
 *        creates and starts this task automatically, so the application must define it: copy
 *        template/os_main.c into the project as os_main.c (see README "Default application
 *        task"). The kernel ships no stub, so a missing definition is a link error rather than
 *        a task that silently does nothing. Not a "_cb" hook: this is where the application's
 *        own code runs, not a kernel query for platform behavior.
 *
 *        Not referenced at all when OS_CONFIG_TEST_ENABLE is 1: the self-test suite runs alone
 *        in that build (see README "Self-test suite"), so no os_main.c is needed there.
 */
void os_main(void);

/*
 * ***********************************************************************************************************
 * Tasks
 * ***********************************************************************************************************
 *
 * A NULL task handle means THIS TASK, wherever one is accepted below: os_task_pause, os_task_delete,
 * os_task_priority_set/get, os_task_state_get, os_task_core_affinity_set, os_task_stack_watermark_get
 * and os_notify_give. It saves a task from keeping a handle to itself just to act on itself, and it
 * is the same shorthand FreeRTOS uses.
 *
 * The exceptions are the two calls where "this task" cannot mean anything: os_task_create needs
 * somewhere to write the new handle, and os_task_start needs a task that is not already running.
 *
 * os_task_pause and os_task_delete are TASK-ONLY whatever handle they are given: both can act on
 * the running task, and an interrupt must not tear down the context it is about to return to.
 *
 * NULL is refused with OS_ERR_INVALID_ARG from an ISR, and before the scheduler dispatches its
 * first task. There is no calling task in either case - inside an interrupt the running task is
 * merely whichever one was preempted, and pausing, deleting or re-prioritising that by accident is
 * exactly the kind of bug this shorthand must not create.
*/

/******************************************************************************************************/
/**
 * @brief Create a task; priority must be OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_30_HIGHEST.
 */
os_err_t os_task_create(os_task_t *task, const os_task_config_t *config);

/******************************************************************************************************/
/**
 * @brief Start a created task (make it ready to run).
 */
os_err_t os_task_start(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Pause a task (NULL means current running task). OS_ERR_BUSY for the idle task and for
 *        the kernel's own service tasks (timer, log).
 */
os_err_t os_task_pause(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Delete a task and release its TCB slot (NULL means current running task). OS_ERR_BUSY
 *        for the idle task and for the kernel's own service tasks (timer, log).
 */
os_err_t os_task_delete(os_task_t *task);

/******************************************************************************************************/
/**
 * @brief Yield the processor to another ready task of equal or higher priority.
 */
void os_task_yield(void);

/******************************************************************************************************/
/**
 * @brief Change a task's priority (NULL means the calling task); takes effect immediately, including
 *        for a task already queued on a mutex, semaphore, queue or event. Accepts only
 *        OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_30_HIGHEST; OS_ERR_BUSY for the idle task and the
 *        kernel's service tasks. A priority-inheritance boost in force is kept - the new value
 *        becomes the base the task returns to.
 */
os_err_t os_task_priority_set(os_task_t *task, os_task_priority_t priority);

/******************************************************************************************************/
/**
 * @brief Get a task's priority (NULL means the calling task): the priority the application set,
 *        not a priority-inheritance boost that may be in force right now.
 */
os_err_t os_task_priority_get(const os_task_t *task, os_task_priority_t *priority_out);

/******************************************************************************************************/
/**
 * @brief Get the current state of a task (NULL means current running task).
 */
os_task_state_t os_task_state_get(const os_task_t *task);

/*
 * ***********************************************************************************************************
 * Time and delays
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Get the kernel tick counter (wraps at 32 bits).
 */
uint32_t os_tick_get(void);

/******************************************************************************************************/
/**
 * @brief Advance the kernel clock by one tick. Call this - and nothing else - from the tick
 *        interrupt, OS_CONFIG_TICK_HZ times per second.
 *
 * This is the one kernel function an application is REQUIRED to call from an ISR, which is why it
 * is public rather than internal. Which ISR depends on OS_CONFIG_TICK_SOURCE:
 *
 *   SYSTICK (default)  The port programs SysTick, and the application routes the vector:
 *
 *                          void SysTick_Handler(void) { os_tick_handler(); }
 *
 *                      Nothing else belongs in that handler. In particular, on STM32 do not also
 *                      call HAL_IncTick() - move the HAL's timebase to a spare TIM instead, or the
 *                      two fight over the same interrupt (see the kernel README).
 *
 *   EXTERNAL           The application's own timer ISR calls it, having started that timer in
 *                      os_arch_tick_init_cb(). See template/os_cb.c.
 *
 * Give the tick interrupt the lowest priority the device offers. It drives preemption, so it
 * should never itself preempt an application interrupt.
 */
void os_tick_handler(void);

/*
 * The three delays return nothing. A delay either waits or the request was one the platform
 * cannot express - an unreadable CPU clock, or a duration too long for a 32-bit tick count - and
 * both of those are programming or configuration errors that OS_ASSERT reports where they happen,
 * rather than a status every call site would have to cast away.
 */

/******************************************************************************************************/
/**
 * @brief Block the calling task for the requested milliseconds (busy-waits before os_start).
 *        OS_WAIT_FOREVER parks the calling task permanently (never returns).
 */
void os_delay_ms(uint32_t milliseconds);

/******************************************************************************************************/
/**
 * @brief Busy-wait for the requested microseconds (precise, does not yield).
 */
void os_delay_us(uint32_t microseconds);

/*
 * ***********************************************************************************************************
 * Critical sections
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Enter a critical section (disables interrupts, supports nesting).
 */
void os_critical_enter(void);

/******************************************************************************************************/
/**
 * @brief Exit a critical section (re-enables interrupts at outermost level).
 */
void os_critical_exit(void);

/*
 * ***********************************************************************************************************
 * Scheduler lock
 * ***********************************************************************************************************
 *
 * The other preemption barrier, and the cheaper one when what you are guarding against is another
 * TASK. Pick by what shares the data:
 *   task <-> task   os_kernel_lock; interrupt latency is unaffected.
 *   task <-> ISR    os_critical_enter (or an atomic) - a scheduler lock excludes no interrupt.
 *   core <-> core   os_critical_enter, whose outermost level takes the cross-core spinlock.
 *
 * Both nest, and neither may be held across a blocking call.
*/

/******************************************************************************************************/
/**
 * @brief Defer context switches on the calling core, leaving interrupts enabled (nesting counted).
 *        Blocking calls degrade to non-blocking while held; a no-op from an ISR.
 */
void os_kernel_lock(void);

/******************************************************************************************************/
/**
 * @brief Release one level of scheduler lock, taking any switch deferred while it was held.
 */
void os_kernel_unlock(void);

/******************************************************************************************************/
/**
 * @brief Whether the calling core currently has its scheduler locked (ISR-safe).
 */
bool os_kernel_is_locked(void);

/*
 * ***********************************************************************************************************
 * Intrusive list
 * ***********************************************************************************************************
 *
 * Always available: the scheduler and the blocking primitives run on these
 * lists, so the list module cannot be configured out. Declared before PART 2
 * because the kernel objects there embed waiter lists.
*/

/******************************************************************************************************/
/**
 * @brief Intrusive list node object.
 */
typedef struct os_list_node
{
    struct os_list_node *next;
    struct os_list_node *prev;

} os_list_node_t;

/******************************************************************************************************/
/**
 * @brief Intrusive list container.
 */
typedef struct
{
    os_list_node_t *head;
    os_list_node_t *tail;

} os_list_t;

/******************************************************************************************************/
/**
 * @brief Initialize list container.
 */
void os_list_init(os_list_t *list);

/******************************************************************************************************/
/**
 * @brief Check whether list is empty.
 */
bool os_list_is_empty(const os_list_t *list);

/******************************************************************************************************/
/**
 * @brief Push node at list tail.
 */
void os_list_push_back(os_list_t *list, os_list_node_t *node);

/******************************************************************************************************/
/**
 * @brief Pop one node from list head.
 */
os_list_node_t* os_list_pop_front(os_list_t *list);

/******************************************************************************************************/
/**
 * @brief Remove a node from anywhere in the list (detached nodes are ignored).
 */
void os_list_remove(os_list_t *list, os_list_node_t *node);

/******************************************************************************************************/
/**
 * @brief Insert a node before the given position (NULL position appends at the tail).
 */
void os_list_insert_before(os_list_t *list, os_list_node_t *position, os_list_node_t *node);

/*
 * ***********************************************************************************************************
 * PART 2 - CONFIGURABLE (each group compiles away with its OS_CONFIG_ option)
 * ***********************************************************************************************************
 *
 * Same order as the option list in os_config.h, one guard per group.
*/

/*
 * ***********************************************************************************************************
 * Mutex              - OS_CONFIG_MUTEX_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_MUTEX_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Mutex object.
 */
typedef struct
{
    bool           locked;     /**< True while held.                                  */
    uint32_t       owner_id;   /**< Task id of the holder, 0 when free/unknown owner. */
    os_list_t      waiters;    /**< Tasks blocked waiting for the mutex.              */
    os_list_node_t owner_node; /**< Links into the owner's owned-mutex list (priority inheritance). */

} os_mutex_t;

/******************************************************************************************************/
/**
 * @brief Initialize a mutex object.
 */
os_err_t os_mutex_init(os_mutex_t *mutex);

/******************************************************************************************************/
/**
 * @brief Acquire a mutex, waiting up to timeout_ms when contended.
 */
os_err_t os_mutex_lock(os_mutex_t *mutex, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Release a mutex object (only the owner may unlock).
 */
os_err_t os_mutex_unlock(os_mutex_t *mutex);

#endif /* OS_CONFIG_MUTEX_ENABLE */

/*
 * ***********************************************************************************************************
 * Semaphore          - OS_CONFIG_SEMAPHORE_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_SEMAPHORE_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Semaphore object.
 */
typedef struct
{
    uint32_t  count;
    uint32_t  max_count;
    os_list_t waiters; /**< Tasks blocked waiting for a token. */

} os_semaphore_t;

/******************************************************************************************************/
/**
 * @brief Initialize a semaphore object.
 */
os_err_t os_semaphore_init(os_semaphore_t *semaphore, uint32_t initial_count, uint32_t max_count);

/******************************************************************************************************/
/**
 * @brief Give one token to semaphore (ISR-safe, never blocks).
 */
os_err_t os_semaphore_give(os_semaphore_t *semaphore);

/******************************************************************************************************/
/**
 * @brief Take one token from semaphore, waiting up to timeout_ms when empty.
 */
os_err_t os_semaphore_take(os_semaphore_t *semaphore, uint32_t timeout_ms);

#endif /* OS_CONFIG_SEMAPHORE_ENABLE */

/*
 * ***********************************************************************************************************
 * Queue              - OS_CONFIG_QUEUE_ENABLE
 * ***********************************************************************************************************
 *
 * A queue is an object plus an item buffer. Which macro declares it decides where that buffer
 * comes from, and that is the only difference between the three kinds:
 *
 *   STATIC    OS_QUEUE_DEFINE_STATIC(sensor_q, sample_t, 8);
 *             / the macro declares the buffer too; usable where it stands
 *
 *   BUFFER    static sample_t dma_area[8] __attribute__((section(".dma")));
 *             OS_QUEUE_DEFINE_BUFFER(rx_q, dma_area);
 *             / you declare the buffer, for storage the line above cannot
 *               express; still usable where it stands
 *
 *   DYNAMIC   OS_QUEUE_DEFINE_DYNAMIC(log_q);
 *             os_queue_init_dynamic(&log_q, item_size, capacity);
 *             / file scope declares only the object; the call obtains the buffer
 *
 * Only the dynamic kind has an init call; the other two are initialized where they are declared
 * and take no geometry, since both values are read off the array. Every call after that is the
 * same for all three, teardown included.
*/

#if (OS_CONFIG_QUEUE_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Queue object.
 */
typedef struct
{
    uint8_t   *buffer;
    size_t    item_size;
    size_t    capacity;
    size_t    head;
    size_t    tail;
    size_t    count;
    os_list_t send_waiters;    /**< Tasks blocked because the queue is full.  */
    os_list_t receive_waiters; /**< Tasks blocked because the queue is empty. */
    bool      buffer_owned;    /**< Buffer came from os_queue_init_dynamic: os_queue_cleanup frees it. */

} os_queue_t;

/* --- Compile-time storage: the geometry is read off the array --------------------------------- */

/** Compile-time initializer binding a queue object to an item array. Shared by the two macros
 *  below so both derive the geometry the same way and cannot drift apart.
 *
 *  Everything omitted here - head, tail, count, the two waiter lists, buffer_owned - is
 *  zero-initialized by the C rules for objects with static storage duration, which is exactly the
 *  empty queue with empty waiter lists an init call would otherwise write at run time. That is why
 *  a queue defined this way is usable where it stands, with nothing to call and no status to check.
 *
 *  Its only parameter is "array" on purpose: a macro parameter named after a struct field would be
 *  substituted inside the designated initializers, turning ".capacity" into ".8" and failing to
 *  compile. Nothing here may be called buffer, item_size or capacity. */
#define OS_QUEUE_INITIALIZER(array)                            \
    {                                                          \
        .buffer    = (uint8_t *)(array),                       \
        .item_size = sizeof((array)[0]),                       \
        .capacity  = (sizeof(array) / sizeof((array)[0])),     \
    }

/** Compile-time proof that "array" is an array and not a pointer to one. sizeof() on a pointer
 *  would derive a nonsense capacity in silence, and every send past the first would then run off
 *  the end of the storage - the exact failure deriving the geometry exists to prevent, so it is
 *  worth refusing to build over. Evaluates to a permitted 1 on compilers without the builtin,
 *  where the check simply does not run. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__cplusplus)
#define OS_QUEUE_IS_ARRAY(array) \
    (!__builtin_types_compatible_p(__typeof__(array), __typeof__(&(array)[0])))
#else
/* C++ lands here too: __builtin_types_compatible_p is a C-only builtin, so the check does not run
 * there and the macro permits the definition, exactly as on a non-GNU compiler. */
#define OS_QUEUE_IS_ARRAY(array) 1
#endif

/** Define a queue with statically allocated storage, ready to use where it stands. The queue
 *  object is declared as plain "name" (what every os_queue_* call takes the address of), and the
 *  backing array gets the decorated "name_BUFFER", which nothing should name by hand.
 *
 *  No init call to pair it with, and no geometry to pass: both values are read off the array, so
 *  they cannot disagree with the storage that exists. "type" is the item type, not a byte count,
 *  so the buffer is typed and the compiler checks what goes into it.
 *
 *      OS_QUEUE_DEFINE_STATIC(sensor_q, sensor_sample_t, 8);
 *      status = os_queue_send(&sensor_q, &sample, 10U);
 *
 *  Both objects are static, so this belongs at file scope. Use OS_QUEUE_DEFINE_BUFFER to place the
 *  buffer yourself, or OS_QUEUE_DEFINE_DYNAMIC for a run-time geometry. */
#define OS_QUEUE_DEFINE_STATIC(name, type, item_count)    \
    static type       name##_BUFFER[(item_count)];        \
    static os_queue_t name = OS_QUEUE_INITIALIZER(name##_BUFFER)

/** Define a queue over an item array you declared yourself, ready to use where it stands. The
 *  escape hatch for storage OS_QUEUE_DEFINE_STATIC cannot express - a named linker section,
 *  DMA-capable RAM, a particular alignment - since the array is yours to attribute however the
 *  platform needs.
 *
 *      static sample_t dma_area[8] __attribute__((section(".dma_buffers")));
 *      OS_QUEUE_DEFINE_BUFFER(rx_q, dma_area);
 *      ...
 *      status = os_queue_send(&rx_q, &sample, 10U);
 *
 *  The array must be declared above this, at file scope, and must be an array rather than a
 *  pointer to one - the line below refuses to compile otherwise. Item size and capacity come from
 *  it, so there is nothing to keep in step by hand. */
#define OS_QUEUE_DEFINE_BUFFER(name, array)                                                 \
    OS_STATIC_ASSERT(OS_QUEUE_IS_ARRAY(array),                                                \
                   "OS_QUEUE_DEFINE_BUFFER needs the item array itself, not a pointer");    \
    static os_queue_t name = OS_QUEUE_INITIALIZER(array)

/* --- Dynamic storage: the item buffer comes from the kernel heap ------------------------------ */

/** Define the object for a queue whose item buffer comes from the kernel heap. Only the object is
 *  declared here - os_queue_init_dynamic(), called in code, is what obtains the buffer - so this
 *  says at the declaration site which kind of queue "name" is, and keeps the queue object out of
 *  the allocation so its lifetime stays obvious and a failed init leaves nothing to clean up.
 *
 *      OS_QUEUE_DEFINE_DYNAMIC(rx_q);
 *      ...
 *      status = os_queue_init_dynamic(&rx_q, sizeof(sample_t), capacity_from_config);
 *
 *  Zero-initialized like any static object, which is the state os_queue_init_dynamic() expects. */
#define OS_QUEUE_DEFINE_DYNAMIC(name) \
    static os_queue_t name

#if (OS_CONFIG_ALLOC_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Initialize a queue over an item buffer allocated from the kernel heap, for a geometry
 *        not known until run time. Only the buffer is allocated; the queue object itself is the
 *        caller's, and os_queue_cleanup releases what this obtained.
 */
os_err_t os_queue_init_dynamic(os_queue_t *queue, size_t item_size, size_t capacity);
#endif /* OS_CONFIG_ALLOC_ENABLE */

/* --- Operations: identical for every storage kind --------------------------------------------- */

/******************************************************************************************************/
/**
 * @brief Send one item into queue, waiting up to timeout_ms when full.
 */
os_err_t os_queue_send(os_queue_t *queue, const void *item, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Receive one item from queue, waiting up to timeout_ms when empty.
 */
os_err_t os_queue_receive(os_queue_t *queue, void *item_out, uint32_t timeout_ms);

/******************************************************************************************************/
/**
 * @brief Get current queue item count.
 */
size_t os_queue_count_get(const os_queue_t *queue);

/******************************************************************************************************/
/**
 * @brief Get the number of item slots the queue can still accept (capacity minus count).
 */
size_t os_queue_free_get(const os_queue_t *queue);

/******************************************************************************************************/
/**
 * @brief Tear down a queue of any kind: empty it, and release the item buffer only when
 *        os_queue_init_dynamic allocated it. A queue that owns no buffer keeps its storage and
 *        stays usable, so a statically defined queue needs no init call after this either.
 *        Refuses with OS_ERR_BUSY while tasks are blocked on the queue.
 */
os_err_t os_queue_cleanup(os_queue_t *queue);

#endif /* OS_CONFIG_QUEUE_ENABLE */

/*
 * ***********************************************************************************************************
 * Events             - OS_CONFIG_EVENT_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_EVENT_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Event object: 32 bits several tasks can wait on.
 */
typedef struct
{
    uint32_t  flags;
    os_list_t waiters; /**< Tasks blocked waiting for bits to match. */

} os_event_t;

/******************************************************************************************************/
/**
 * @brief Initialize an event object.
 */
os_err_t os_event_init(os_event_t *event);

/******************************************************************************************************/
/**
 * @brief Set event bits (ISR-safe).
 */
os_err_t os_event_set_bits(os_event_t *event, uint32_t bits);

/******************************************************************************************************/
/**
 * @brief Clear event bits (ISR-safe).
 */
os_err_t os_event_clear_bits(os_event_t *event, uint32_t bits);

/******************************************************************************************************/
/**
 * @brief Wait for event bits, waiting up to timeout_ms until they match. clear_on_exit true
 *        consumes the requested bits atomically with the match (no lost set between the
 *        wait returning and a separate manual clear).
 */
os_err_t os_event_wait_bits(os_event_t *event, uint32_t bits, bool wait_all, bool clear_on_exit, uint32_t *matched_bits, uint32_t timeout_ms);

#endif /* OS_CONFIG_EVENT_ENABLE */

/*
 * ***********************************************************************************************************
 * Software timer     - OS_CONFIG_TIMER_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TIMER_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Timer operating mode.
 */
typedef enum
{
    
    OS_TIMER_MODE_ONE_SHOT = 0, /**< Fires once, then stops.             */
    OS_TIMER_MODE_PERIODIC = 1, /**< Reloads and fires every period.     */
    OS_TIMER_MODE_SUBMIT   = 2, /**< Defers a callback to the timer task */

} os_timer_mode_t;

/******************************************************************************************************/
/**
 * @brief Timer callback signature.
 *
 * Both arguments come from os_timer_start or os_timer_submit, so a call can say WHICH object
 * it concerns and WHAT happened without the kernel copying a payload.
 */
typedef void (*os_timer_callback_t)(void *context, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Software timer object.
 */
typedef struct
{
    /** Points at this object, so the kernel can tell a real timer from a lump of memory: the
     *  link state lives inside the object, and following a garbage node would write to an
     *  address nobody chose. Anything else is refused with OS_ERR_INVALID_ARG. A
     *  self-pointer rather than a constant catches a COPIED timer too. See os_timer.c. */
    void                *self;
    uint32_t            period_ticks;
    uint32_t            remaining_ticks;
    os_timer_mode_t     mode;
    bool                active;  /**< Counting down right now.                       */
    bool                paused;  /**< Halted by os_timer_pause, remaining_ticks kept. */
    bool                queued;  /**< Expiry noted by the tick, waiting its turn to run. */
    os_timer_callback_t callback;
    void                *context;
    uint32_t            value;
    os_list_node_t      ready_node;    /**< Links into the FIFO of expiries awaiting delivery.   */
    os_list_node_t      running_node;  /**< Links into the list of timers the tick counts down. */

} os_timer_t;

/******************************************************************************************************/
/**
 * @brief A pool of deferred calls - see OS_TIMER_DEFINE_SUBMIT and os_timer_submit.
 */
typedef struct os_timer_pool_s os_timer_pool_t;

/******************************************************************************************************/
/**
 * @brief One slot in a pool: a timer, plus the pool to hand it back to. The back-pointer lives
 *        here rather than in os_timer_t so ordinary timers do not pay for it.
 */
typedef struct
{
    os_timer_t      timer;
    os_timer_pool_t *pool;

} os_timer_entry_t;

/******************************************************************************************************/
/**
 * @brief A pool of deferred calls: the storage os_timer_submit hands out, one slot per call.
 *
 * Declared by OS_TIMER_DEFINE_SUBMIT and owned by the caller, which is what keeps OS_ERR_FULL
 * local to one pool. Everything here is settled at compile time except free_list and ready, which
 * the kernel fills in the first time the pool is used - so a pool needs no init call and the
 * kernel keeps no list of pools.
 */
struct os_timer_pool_s
{
    void                *self;       /**< Points at this pool; the same validity check timers use.   */
    os_timer_entry_t    *entries;    /**< The slots, from OS_TIMER_DEFINE_SUBMIT.                    */
    uint32_t            count;       /**< How many, so at most this many calls may be in flight.     */
    uint32_t            delay_ticks; /**< From OS_TIMER_DEFINE_SUBMIT; 0 means deliver immediately.  */
    os_timer_callback_t callback;    /**< What every submission to this pool runs.                   */
    os_list_t           free_list;   /**< Slots nobody is using; they link through timer.ready_node. */
    bool                ready;       /**< Set on first use, when the slots are threaded onto free_list. */
};

/*
 * A timer's life cycle, and what each call does to the countdown:
 *
 *   OS_TIMER_DEFINE_PERIODIC / OS_TIMER_DEFINE_ONESHOT   period and callback, at compile time
 *   os_timer_start     run - from the full period, or from where a pause left off
 *   os_timer_restart   run from the full period, whatever the timer was doing
 *                      both carry the context and value the callback will receive
 *   os_timer_pause     halt, keeping the time that was left
 *   os_timer_stop      cancel, discarding both the remaining time and any owed callback
 *   os_timer_period_set    retune the period; the countdown under way is left alone
 *   os_timer_callback_set  point it at a different callback
 *   os_timer_value_set     change the number the callback receives, without restarting
 *
 * There is no init and no delete: nothing is reserved, so os_timer_start cannot fail for want
 * of a resource however many timers are running, and a stopped timer keeps its configuration
 * so it can simply be started again.
 *
 * ALL of them are ISR-safe - each is a short critical section over a list and none blocks.
 * Two caveats. With OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY set, the interrupt must sit at or below
 * that priority, the rule every ISR-safe call here follows. And the CALLBACK is not ISR
 * context: it runs on the kernel timer task, so it may block, take a mutex or wait on a queue.
 */

/** A timer is set up entirely at COMPILE time - no init call, and the macro's NAME is the mode,
 *  so there is none to pass and none to get wrong:
 *
 *    OS_TIMER_DEFINE_PERIODIC(blinker, 500U, on_blink);
 *    OS_TIMER_DEFINE_ONESHOT(timeout,  250U, on_timeout);
 *
 *    os_timer_start(&blinker, &led2, 3U);
 *
 *  What the timer IS - how often it fires and what it calls - is settled here; what a RUN is
 *  about - the context and value the callback receives - is given to os_timer_start. One place
 *  each, so no setting can contradict another.
 *
 *  Parameters are prefixed (timer_period_ms, not period_ms) so a caller's own variable names
 *  cannot be substituted into the field designators below - the trap OS_QUEUE_DEFINE_BUFFER
 *  documents. */

/** Reloads and fires every period_ms until stopped. */
#define OS_TIMER_DEFINE_PERIODIC(timer_name, timer_period_ms, timer_callback)             \
    OS_STATIC_ASSERT(((timer_period_ms) != 0U) && ((timer_period_ms) != OS_WAIT_FOREVER),   \
                   "OS_TIMER_DEFINE_PERIODIC: the period is in milliseconds and cannot "  \
                   "be 0 or OS_WAIT_FOREVER");                                            \
    static os_timer_t timer_name = {                                                      \
        .self         = &timer_name,                                                      \
        .period_ticks = OS_TICKS_FROM_MS(timer_period_ms),                                \
        .mode         = OS_TIMER_MODE_PERIODIC,                                           \
        .callback     = (timer_callback)                                                  \
    }

/** Fires once, period_ms after it is started, then stops. */
#define OS_TIMER_DEFINE_ONESHOT(timer_name, timer_period_ms, timer_callback)              \
    OS_STATIC_ASSERT(((timer_period_ms) != 0U) && ((timer_period_ms) != OS_WAIT_FOREVER),   \
                   "OS_TIMER_DEFINE_ONESHOT: the period is in milliseconds and cannot "   \
                   "be 0 or OS_WAIT_FOREVER");                                            \
    static os_timer_t timer_name = {                                                      \
        .self         = &timer_name,                                                      \
        .period_ticks = OS_TICKS_FROM_MS(timer_period_ms),                                \
        .mode         = OS_TIMER_MODE_ONE_SHOT,                                           \
        .callback     = (timer_callback)                                                  \
    }

/** A pool of deferred calls, for the case os_timer_start deliberately does NOT serve.
 *
 *    OS_TIMER_DEFINE_SUBMIT(uart_defer, 8U, 0U, on_uart_event);
 *                                       |    |
 *                                       |    delay before each call (0 = as soon as possible)
 *                                       how many may be in flight at once
 *
 *    os_timer_submit(&uart_defer, &dev, code1);   // in the ISR
 *    os_timer_submit(&uart_defer, &dev, code2);   // again, before the first has run
 *    -> the callback runs TWICE, with code1 then code2
 *
 *  os_timer_start would have run it ONCE carrying only code2: starting a pending timer
 *  reschedules it, which is what a debounce wants and what deferring an interrupt must not do.
 *  A submission never coalesces - each call takes its own slot.
 *
 *  The slots are yours, so OS_ERR_FULL means "your eight are in flight" and there is no
 *  kernel-wide number to tune. The delay belongs to the pool, converted to ticks here, so
 *  os_timer_submit does no arithmetic at all; work needing a different delay is another pool.
 *
 *  pool_depth slots cost pool_depth * sizeof(os_timer_entry_t), threaded on first use.
 */
#define OS_TIMER_DEFINE_SUBMIT(pool_name, pool_depth, pool_delay_ms, pool_callback)       \
    OS_STATIC_ASSERT((pool_depth) > 0U,                                                     \
                   "OS_TIMER_DEFINE_SUBMIT: the depth is how many calls may be in "       \
                   "flight at once and cannot be 0");                                     \
    OS_STATIC_ASSERT((pool_delay_ms) != OS_WAIT_FOREVER,                                    \
                   "OS_TIMER_DEFINE_SUBMIT: the delay is in milliseconds; use 0 for "     \
                   "as soon as possible");                                                \
    static os_timer_entry_t pool_name##_entries[(pool_depth)];                            \
    static os_timer_pool_t pool_name = {                                                  \
        .self        = &pool_name,                                                        \
        .entries     = pool_name##_entries,                                               \
        .count       = (pool_depth),                                                      \
        .delay_ticks = OS_TICKS_FROM_MS(pool_delay_ms),                                   \
        .callback    = (pool_callback)                                                    \
    }

/******************************************************************************************************/
/**
 * @brief Start a timer, or resume one os_timer_pause halted - a paused timer continues with the
 *        time it had left, anything else starts a full period. context and value are what the
 *        callback receives on every expiry of this run.
 */
os_err_t os_timer_start(os_timer_t *timer, void *context, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Restart a software timer from a full period, whether it was running, paused or stopped.
 *        The call to reach for when an event should push the deadline back. context and value as
 *        for os_timer_start.
 */
os_err_t os_timer_restart(os_timer_t *timer, void *context, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Halt a running timer, keeping the time it had left for os_timer_start to resume from.
 *        An expiry already noted but not yet delivered still runs.
 */
os_err_t os_timer_pause(os_timer_t *timer);

/******************************************************************************************************/
/**
 * @brief Stop a software timer, discarding the remaining time and any owed callback.
 */
os_err_t os_timer_stop(os_timer_t *timer);

/******************************************************************************************************/
/**
 * @brief Change a timer's period, in milliseconds. A countdown already under way keeps the time
 *        it had; follow with os_timer_restart to apply the new period from now.
 */
os_err_t os_timer_period_set(os_timer_t *timer, uint32_t period_ms);

/******************************************************************************************************/
/**
 * @brief Change what a timer calls. Takes effect from the next expiry; a delivery already under
 *        way still runs what it copied out. The context stays as os_timer_start left it.
 */
os_err_t os_timer_callback_set(os_timer_t *timer, os_timer_callback_t callback);

/******************************************************************************************************/
/**
 * @brief Change the value a timer's callback receives. Takes effect from the next expiry.
 */
os_err_t os_timer_value_set(os_timer_t *timer, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Run the pool's callback later, once per call - the deferred form that never coalesces.
 *        Each submission takes its own slot and produces its own delivery, FIFO. ISR-safe, and
 *        nothing is copied, so whatever context points at must outlive the call.
 *
 *        A submission has NO HANDLE, so once made it cannot be changed: not cancelled, not
 *        retuned, and not given a different context or value. There is nothing to pass to
 *        os_timer_stop or os_timer_value_set, and the pool entry carrying it is the kernel's
 *        until it runs. If the call may need to change its mind, give the callback something to
 *        re-read; if it may need cancelling, it wants a named timer and os_timer_start instead.
 */
os_err_t os_timer_submit(os_timer_pool_t *pool, void *context, uint32_t value);

#endif /* OS_CONFIG_TIMER_ENABLE */

/*
 * ***********************************************************************************************************
 * Task notifications - OS_CONFIG_NOTIFY_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_NOTIFY_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Deliver a value to a task's notification mailbox (overwrite: last write wins), waking
 *        it if it is currently blocked in os_notify_wait; ISR-safe. NULL means the calling task,
 *        which an ISR does not have and is refused for.
 */
os_err_t os_notify_give(os_task_t *task, uint32_t value);

/******************************************************************************************************/
/**
 * @brief Wait for this task's notification mailbox, up to timeout_ms. Task-only. value_out may
 *        be NULL to take the wake-up and discard the value; it is consumed either way.
 */
os_err_t os_notify_wait(uint32_t timeout_ms, uint32_t *value_out);

#endif /* OS_CONFIG_NOTIFY_ENABLE */

/*
 * ***********************************************************************************************************
 * Kernel heap        - OS_CONFIG_ALLOC_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_ALLOC_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Allocate memory from the kernel heap (8-byte aligned; NULL when exhausted).
 */
void* os_mem_alloc(size_t size);

/******************************************************************************************************/
/**
 * @brief Return memory obtained from os_mem_alloc to the kernel heap (NULL is ignored).
 */
void os_mem_free(void *memory);

/******************************************************************************************************/
/**
 * @brief Get the number of bytes currently free in the kernel heap.
 */
size_t os_mem_free_get(void);

/******************************************************************************************************/
/**
 * @brief Get the smallest amount of free heap ever observed (worst case since boot).
 */
size_t os_mem_watermark_get(void);

#endif /* OS_CONFIG_ALLOC_ENABLE */

/*
 * ***********************************************************************************************************
 * Atomics            - OS_CONFIG_ATOMIC_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Atomic word: the type of every variable the os_atomic_* operations act on.
 *
 * Declare one as os_atomic_t, never as a plain int32_t, and touch it only through os_atomic_*.
 * An ordinary read or write of the same word is not ordered against these calls, which is the
 * usual way a counter that "uses atomics" still loses updates.
 */
typedef int32_t os_atomic_t;

/** Initializer for an os_atomic_t: static os_atomic_t counter = OS_ATOMIC_INIT(0); */
#define OS_ATOMIC_INIT(value)  ((os_atomic_t)(value))

/******************************************************************************************************/
/*
 * Atomic operations on an os_atomic_t (see the type above).
 *
 * Every read-modify-write returns the value the word held BEFORE the operation, not after it, so
 * os_atomic_inc returning 4 means the counter now reads 5.
 *
 * How the update is made indivisible is the port's business and varies with the core. Where the
 * instruction set can do it - an exclusive load/store pair - these are lock-free and never mask
 * interrupts. Where it cannot, the port briefly excludes interrupts (and, on a multi-core build,
 * the other cores) instead, which makes an atomic operation cost interrupt latency on those cores
 * and is worth knowing before putting one in a hot path. See the README "Atomics" section for
 * which cores fall on which side.
 *
 * All of them are safe from tasks and from ISRs.
 */

/******************************************************************************************************/
/**
 * @brief Read the current value.
 */
int32_t os_atomic_get(const os_atomic_t *target);

/******************************************************************************************************/
/**
 * @brief Store a value, returning the previous one.
 */
int32_t os_atomic_set(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Store 0, returning the previous value.
 */
int32_t os_atomic_clear(os_atomic_t *target);

/******************************************************************************************************/
/**
 * @brief Add, returning the previous value.
 */
int32_t os_atomic_add(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Subtract, returning the previous value.
 */
int32_t os_atomic_sub(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Add 1, returning the previous value.
 */
int32_t os_atomic_inc(os_atomic_t *target);

/******************************************************************************************************/
/**
 * @brief Subtract 1, returning the previous value.
 */
int32_t os_atomic_dec(os_atomic_t *target);

/******************************************************************************************************/
/**
 * @brief Bitwise OR, returning the previous value.
 */
int32_t os_atomic_or(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Bitwise AND, returning the previous value.
 */
int32_t os_atomic_and(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Bitwise XOR, returning the previous value.
 */
int32_t os_atomic_xor(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Bitwise NAND (~(old & value)), returning the previous value.
 */
int32_t os_atomic_nand(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Compare-and-swap: store desired only if the word still holds expected.
 */
bool os_atomic_cas(os_atomic_t *target, int32_t expected, int32_t desired);

/******************************************************************************************************/
/**
 * @brief Test one bit.
 */
bool os_atomic_test_bit(const os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Set one bit, returning its previous state.
 */
bool os_atomic_test_and_set_bit(os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Clear one bit, returning its previous state.
 */
bool os_atomic_test_and_clear_bit(os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Set one bit.
 */
void os_atomic_set_bit(os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Clear one bit.
 */
void os_atomic_clear_bit(os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Set one bit to the given state.
 */
void os_atomic_set_bit_to(os_atomic_t *target, uint32_t bit, bool value);

#endif /* OS_CONFIG_ATOMIC_ENABLE */

/*
 * ***********************************************************************************************************
 * Stack watermark    - OS_CONFIG_STACK_WATERMARK_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_STACK_WATERMARK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the minimum stack headroom a task has ever had, in bytes (NULL means current task).
 */
os_err_t os_task_stack_watermark_get(const os_task_t *task, size_t *min_free_bytes);
#endif /* OS_CONFIG_STACK_WATERMARK_ENABLE */

/*
 * ***********************************************************************************************************
 * Stack overflow     - OS_CONFIG_STACK_CHECK_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_STACK_CHECK_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Reported when a task is found to have overrun its stack, at the moment it is switched
 *        out. REQUIRED when OS_CONFIG_STACK_CHECK_ENABLE is 1: the kernel ships no default, so a
 *        missing one is a link error. The core parks immediately afterwards either way, which
 *        makes this the only chance to record which task it was.
 *
 *        Runs inside PendSV with the kernel's interrupts masked, so it must NOT call any kernel
 *        API. Write the name to a UART, latch it somewhere the debugger can find, and return.
 *
 * @param[in] task_name  Name of the offending task, as given to OS_TASK_DEFINE.
 */
void os_stack_overflow_cb(const char *task_name);
#endif /* OS_CONFIG_STACK_CHECK_ENABLE */

/*
 * ***********************************************************************************************************
 * CPU usage          - OS_CONFIG_CPU_USAGE_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CPU_USAGE_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Get the CPU usage in percent (0..100) since the previous call; one-tick resolution,
 *        so sample at a period well above the tick period (e.g. once per second).
 */
uint32_t os_cpu_usage_get(void);
#endif /* OS_CONFIG_CPU_USAGE_ENABLE */

/*
 * ***********************************************************************************************************
 * Assertions         - OS_CONFIG_ASSERT_ENABLE
 * ***********************************************************************************************************
*/

/** OS_ASSERT(expr) checks a condition that must hold if the program is correct, and halts at the
 *  point of failure when it does not.
 *
 *  Assertions only ADD checks: the kernel still returns the same status codes either way, so a
 *  build with assertions off behaves exactly as one with them on, minus the halt. Use them for
 *  programming errors (a bad handle, blocking from an ISR), never for conditions that can
 *  legitimately happen at runtime.
 *
 *  The expression is not evaluated at all when assertions are compiled out, so it must be free
 *  of side effects. */
#if (OS_CONFIG_ASSERT_ENABLE == 1U)

#define OS_ASSERT(expr)                                                       \
    do {                                                                      \
        if (!(expr))                                                          \
        {                                                                     \
            os_assert_failed(__FILE__, (uint32_t)__LINE__);                   \
        }                                                                     \
    } while (0)

/******************************************************************************************************/
/**
 * @brief Report a failed OS_ASSERT and halt. Calls os_assert_failed_cb, then parks the core
 *        with interrupts masked so a debugger stops at the cause. Never returns.
 */
void os_assert_failed(const char *file, uint32_t line);

/******************************************************************************************************/
/**
 * @brief Application hook for a failed assertion: record or print the location before the
 *        kernel halts. The application must define it (see template/os_cb.c) - the kernel
 *        ships no stub, because a silent one would leave an assertion with nothing to report.
 *        Runs with the failure's own context still intact, so keep it short and do not expect
 *        to return from the assertion.
 */
void os_assert_failed_cb(const char *file, uint32_t line);

#else /* OS_CONFIG_ASSERT_ENABLE == 0U */

#define OS_ASSERT(expr)         ((void)0)

#endif /* OS_CONFIG_ASSERT_ENABLE */

/*
 * ***********************************************************************************************************
 * Logging            - OS_CONFIG_LOG_ENABLE
 * ***********************************************************************************************************
*/

/** Severity values for OS_CONFIG_LOG_LEVEL, outside the guard below because an os_config.h
 *  selects one by name even though it is read before this header: a macro body is only expanded
 *  where it is used, and every comparison against these lives below. They are compared
 *  numerically in #if, so the increasing order is part of the contract, not just a convention. */
#define OS_LOG_LEVEL_NONE       0U
#define OS_LOG_LEVEL_ERROR      1U
#define OS_LOG_LEVEL_WARN       2U
#define OS_LOG_LEVEL_INFO       3U
#define OS_LOG_LEVEL_DEBUG      4U

/** Buffered log calls (see OS_CONFIG_LOG_ENABLE / OS_CONFIG_LOG_LEVEL). Each
 *  formats like printf, returns immediately, and is safe from tasks and ISRs.
 *  Calls above the configured level expand to nothing, arguments included, so
 *  a disabled OS_LOG_DEBUG costs neither code nor the cost of its arguments. */
#if (OS_CONFIG_LOG_ENABLE == 1U)

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_ERROR)
#define OS_LOG_ERROR(...)       os_log_write(OS_LOG_LEVEL_ERROR, __VA_ARGS__)
#else
#define OS_LOG_ERROR(...)       ((void)0)
#endif

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_WARN)
#define OS_LOG_WARN(...)        os_log_write(OS_LOG_LEVEL_WARN, __VA_ARGS__)
#else
#define OS_LOG_WARN(...)        ((void)0)
#endif

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_INFO)
#define OS_LOG_INFO(...)        os_log_write(OS_LOG_LEVEL_INFO, __VA_ARGS__)
#else
#define OS_LOG_INFO(...)        ((void)0)
#endif

#if (OS_CONFIG_LOG_LEVEL >= OS_LOG_LEVEL_DEBUG)
#define OS_LOG_DEBUG(...)       os_log_write(OS_LOG_LEVEL_DEBUG, __VA_ARGS__)
#else
#define OS_LOG_DEBUG(...)       ((void)0)
#endif

/******************************************************************************************************/
/**
 * @brief Format a log line and queue it for transmission. Prefer the OS_LOG_ERROR/WARN/INFO/
 *        DEBUG macros, which also drop the call entirely below OS_CONFIG_LOG_LEVEL.
 *
 * Safe from tasks and ISRs, and never blocks: the line is formatted, copied into the ring
 * buffer, and the caller returns. A line that does not fit is dropped whole and counted
 * (os_log_dropped_get), never written in part.
 */
void os_log_write(uint32_t level, const char *fmt, ...);

/******************************************************************************************************/
/**
 * @brief Number of log lines dropped so far because the buffer was full. Also reported into
 *        the log itself once space frees up, so this is only needed for programmatic checks.
 */
uint32_t os_log_dropped_get(void);

/******************************************************************************************************/
/**
 * @brief Application hook that transmits finished log bytes; called from the kernel log task,
 *        never from an ISR or a critical section, so it may block or start a DMA transfer.
 *        REQUIRED when OS_CONFIG_LOG_ENABLE is 1: the kernel ships no default, so a log with
 *        nowhere to go is a link error rather than silence.
 *
 * @param[in] data    Bytes to transmit; valid only for the duration of the call.
 * @param[in] length  Number of bytes.
 */
void os_log_output_cb(const uint8_t *data, size_t length);

#else /* OS_CONFIG_LOG_ENABLE == 0U */

#define OS_LOG_ERROR(...)       ((void)0)
#define OS_LOG_WARN(...)        ((void)0)
#define OS_LOG_INFO(...)        ((void)0)
#define OS_LOG_DEBUG(...)       ((void)0)

#endif /* OS_CONFIG_LOG_ENABLE */

/*
 * ***********************************************************************************************************
 * Self-test          - OS_CONFIG_TEST_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TEST_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Kernel self-test suite entry point (see OS_CONFIG_TEST_* in os_config.h). os_init()
 *        creates and starts a task that calls this automatically, so link the ahura_kernel/test
 *        library (CMake target "os_test") to supply it (see README "Self-test suite"). The
 *        kernel ships no stub, which is what lets a plain static-library link pull the suite in
 *        and turns "forgot to link it" into a link error. Not a "_cb" hook, same reasoning as
 *        os_main().
 */
void os_test(void);
#endif /* OS_CONFIG_TEST_ENABLE */

/*
 * ***********************************************************************************************************
 * TrustZone          - OS_CONFIG_TRUSTZONE
 * ***********************************************************************************************************
 *
 * Also declared by the arch port (os_arch_port_common.h), which calls them from
 * the context-switch path; repeated here because they are application-provided.
*/

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
/******************************************************************************************************/
/**
 * @brief TrustZone callback: bank the secure-side context of the task being switched out
 *        (task_id 0 = idle task, no secure context). You define it; the kernel ships no default,
 *        so leaving it out is a link error.
 */
void os_arch_tz_context_save_cb(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief TrustZone callback: restore the secure-side context of the task being switched in.
 *        You define it; the kernel ships no default.
 */
void os_arch_tz_context_restore_cb(uint32_t task_id);
#endif /* OS_CONFIG_TRUSTZONE_NON_SECURE */

/*
 * ***********************************************************************************************************
 * Multi-core         - OS_CONFIG_CORE_COUNT > 1
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)

/******************************************************************************************************/
/**
 * @brief Enter the scheduler on a secondary core. Call after os_start() is running on core 0,
 *        from the secondary core, once the SoC layer has booted it with a vector table routing
 *        PendSV to the kernel handler. Does not return.
 */
void os_core_start(void);

/******************************************************************************************************/
/**
 * @brief Change which cores a task may run on (bitmask, OS_TASK_CORE_ANY = any core).
 */
os_err_t os_task_core_affinity_set(os_task_t *task, uint32_t core_affinity);

/* The two SoC callbacks below are also declared by the arch port
 * (os_arch_port_common.h), which calls them; repeated here because they are
 * application-provided. */

/******************************************************************************************************/
/**
 * @brief Multi-core SoC callback: return the index of the calling core (0-based).
 *        REQUIRED when OS_CONFIG_CORE_COUNT is above 1; the kernel ships no default.
 */
uint32_t os_arch_core_id_get_cb(void);

/******************************************************************************************************/
/**
 * @brief Multi-core SoC callback: interrupt another core so it re-evaluates scheduling.
 *        REQUIRED when OS_CONFIG_CORE_COUNT is above 1; the kernel ships no default.
 */
void os_arch_core_ipi_request_cb(uint32_t core_id);

/******************************************************************************************************/
/**
 * @brief Multi-core SoC callback: boot a secondary core so it reaches os_core_start().
 *        Called by os_start(), once per core above 0. REQUIRED when OS_CONFIG_CORE_COUNT is
 *        above 1; the kernel ships no default.
 */
void os_arch_core_launch_cb(uint32_t core_id);

/******************************************************************************************************/
/**
 * @brief Multi-core SoC callback: print whatever the SoC package knows about its own bring-up.
 *        Called only when something has already gone wrong, so it may take its time.
 *
 * Optional - the kernel ships a weak empty default, so a package that has nothing to add costs
 * nothing. It exists because the kernel cannot diagnose a chip: whether a core was released, which
 * inter-core interrupt was armed, what a fault handler caught are all facts only the package
 * holds, and the failures they cause look identical from the kernel's side.
 *
 * Called from task context with the kernel running, which is deliberate: the natural moment for a
 * package to report is start-up, and on a USB-console board nothing printed then is ever seen -
 * the host has not opened the port yet. This runs late enough to be read.
 */
void os_arch_soc_diagnose_cb(void);

#endif /* OS_CONFIG_CORE_COUNT > 1U */

/******************************************************************************************************/
/**
 * @brief SoC callback: wait for work on an idle core. Optional - the kernel's weak default is a
 *        plain WFI, which is right on most parts.
 *
 * It exists because "how a core waits, and what wakes it" is a fact about the silicon rather than
 * the architecture, and on some parts WFI is the wrong instrument. On the RP2 family WFI gates the
 * core's clock, which stops that core's own SysTick with it: an idle secondary core then has no
 * timer of its own left and depends entirely on an inter-core interrupt arriving. Miss one and the
 * core sleeps for good.
 *
 * WFE is the answer there, because the event register LATCHES: an SEV that arrives before the WFE
 * makes it return immediately, so the wake cannot be lost in the window between deciding to sleep
 * and sleeping. A package overriding this should pair it with an SEV wherever it signals another
 * core. FreeRTOS's own RP2040 SMP port makes the same choice for the same reason.
 *
 * May return spuriously; the idle loop simply calls it again.
 */
void os_arch_soc_idle_cb(void);

/*
 * ***********************************************************************************************************
 * Tickless idle      - OS_CONFIG_TICKLESS_ENABLE
 * ***********************************************************************************************************
 *
 * Three kernel-provided control functions and two application-provided hooks,
 * all behind the one guard. Calling any of them with tickless idle disabled is
 * a compile error naming the function, not a call that silently does nothing.
*/

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Execute one tickless-idle pass: suppress ticking for the next known-idle duration,
 *        sleep, and announce the real elapsed time on wake.
 */
void os_tickless_idle_process(void);

/******************************************************************************************************/
/**
 * @brief Ticks the kernel would plan to suppress right now, bounded by the earliest kernel
 *        time source (timer expiry, finite-delay sleeper).
 */
uint32_t os_tickless_expected_idle_ticks_get(void);

/******************************************************************************************************/
/**
 * @brief Maximum ticks the active arch port can suppress in one tickless window, given the
 *        platform clock and OS_CONFIG_TICK_HZ (not a fixed constant). Returns 0 when the active
 *        port does not yet suppress ticking for real (see README "Tickless idle").
 */
uint32_t os_tickless_max_suppressed_ticks_get(void);

/******************************************************************************************************/
/**
 * @brief Pre-sleep callback invoked before entering low-power mode. The application must define
 *        it; the kernel provides no default.
 */
void os_tickless_pre_sleep_cb(void);

/******************************************************************************************************/
/**
 * @brief Post-sleep callback invoked after leaving low-power mode, before the kernel accounts for
 *        the sleep. The application must define it; the kernel provides no default.
 */
void os_tickless_post_sleep_cb(void);

#endif /* OS_CONFIG_TICKLESS_ENABLE */

#ifdef __cplusplus
}

#endif

#endif /* AHURA_H */
