/**
 * @file os_config.h
 * @brief Template for the application's os_config.h - every kernel option at its default value.
 *
 * NOT included by the kernel: copy it into the application source tree as os_config.h, adjust the
 * values, and make its directory visible to BOTH the application and the kernel library build
 * (OS_CONFIG_DIR, see the README "Configuration" section).
 *
 * This file is the single source of configuration, so do not also define OS_CONFIG_ macros from
 * the build system, and do not remove options: an incomplete configuration is rejected at compile
 * time rather than silently misconfiguring the kernel.
 *
 *   PART 1  CORE - always compiled in. Set these first.
 *   PART 2  OPTIONAL FEATURES - one section per feature, each holding its _ENABLE switch with the
 *           sizing that switch controls. Same order as PART 2 of ahura.h.
 *   PART 3  PLATFORM - target properties (TrustZone, core count, tickless). The target's own
 *           facts - PendSV vector name, spinlock backend - belong to the SoC package instead.
 *
 * Two options are marked OPTIONAL where they appear - OS_CONFIG_TICK_SOURCE and
 * OS_CONFIG_ARCH_VECTOR_CHECK. Each is a name or a yes/no
 * diagnostic the kernel can default correctly on its own, so a config predating them still builds
 * and behaves identically. Everything else is mandatory: a missing sizing or feature switch would
 * read as 0 in an #if and silently disable or misconfigure something, which is why the kernel
 * rejects it outright instead.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_CONFIG_H
#define OS_CONFIG_H

/*
 * ***********************************************************************************************************
 * PART 1 - CORE (always compiled in; no switch removes any of this)
 * ***********************************************************************************************************
 *
 * Tasks, the tick, delays, critical sections and the intrusive list have no
 * switch at all - the scheduler itself runs on them - so their API is always
 * present and the values below always apply.
*/

/*
 * ***********************************************************************************************************
 * Tick rate
 * ***********************************************************************************************************
*/

#define OS_CONFIG_TICK_HZ                   1000U

/**
 * Where that tick comes from. The two values are kernel-owned
 * (os_arch_port_common.h); this only selects between them.
 *
 *   OS_CONFIG_TICK_SOURCE_SYSTICK   The port programs SysTick itself, from the
 *                                   live CPU clock and OS_CONFIG_TICK_HZ above.
 *                                   Nothing to write, nothing to route.
 *
 *   OS_CONFIG_TICK_SOURCE_EXTERNAL  The application owns the tick hardware. The
 *                                   port programs nothing and instead calls
 *                                   os_arch_tick_init_cb() (os_cb.c) once at
 *                                   the end of os_init(); that callback starts
 *                                   whatever timer the board uses, and its ISR
 *                                   must call os_tick_handler() OS_CONFIG_TICK_HZ
 *                                   times per second.
 *
 * Reach for EXTERNAL when SysTick is unusable or already taken:
 *
 *   - It stops in the low-power modes several families ship with. Nordic nRF5x
 *     drives time from the RTC peripheral for exactly this reason, and a
 *     SysTick-based kernel there simply loses time whenever the CPU sleeps.
 *   - A vendor HAL or bootloader already owns it and cannot give it up.
 *   - The device does not implement it.
 *
 * Note for tickless idle: the ARMv8-M port suppresses ticks by reprogramming
 * SysTick, which it obviously cannot do for a timer it does not own. With
 * EXTERNAL it therefore reports no suppression capability and idle degrades to
 * a plain WFI - correct, just not power-optimal.
 *
 * This option is OPTIONAL: leaving it out selects SYSTICK.
 */
#define OS_CONFIG_TICK_SOURCE               OS_CONFIG_TICK_SOURCE_SYSTICK

/* Round-robin time slice, in ticks: how long a task may hold the CPU before an
 * equal-priority peer takes over. Tasks of DIFFERENT priorities are unaffected
 * - a higher-priority task always preempts immediately, whatever is set here.
 *
 *   1  Rotate on every tick. The classic default, and what the kernel did
 *      before this option existed.
 *   N  Rotate every N ticks. Each equal-priority task gets a longer, less
 *      interrupted run, and the kernel spends proportionally fewer context
 *      switches on rotation alone - the tick that would only have rotated now
 *      costs a bitmap check instead of a full PendSV round trip.
 *   0  No rotation at all: a task runs until it blocks, yields or is preempted
 *      by a higher priority. Lowest overhead, but two equal-priority
 *      compute-bound tasks will starve each other.
 *
 * A task that blocks or yields gives up the rest of its slice, and a freshly
 * dispatched task always starts a whole one. */
#define OS_CONFIG_TIME_SLICE_TICKS          1U

/* The CPU clock is NOT configured here. The kernel reads the live CMSIS
 * SystemCoreClock variable, which the device's own SystemInit() sets and
 * SystemCoreClockUpdate() refreshes after every clock-tree change - a
 * build-time constant could not follow a runtime switch. Devices whose startup
 * code does not define that symbol just define it themselves; see the kernel
 * README, "Platform clock". */

/*
 * ***********************************************************************************************************
 * Task table and stacks
 * ***********************************************************************************************************
*/

/* How many tasks the APPLICATION may have. The kernel's own service tasks are
 * NOT counted here: it reserves their slots on top of this number, so enabling
 * the log or the timer service never costs the application a task, and this value
 * is exactly what os_task_create() will accept before returning
 * OS_ERR_FULL.
 *
 * 8 rather than something smaller because the self-test suite's multi-core
 * section runs on it: the test task, the two heartbeat workers it parks, and up
 * to five concurrent stress helpers (see doc/testing.md). A smaller value is
 * fine for an application that never runs the suite, and then the suite simply
 * fails its SMP section with OS_ERR_FULL. */
#define OS_CONFIG_MAX_USER_TASKS            8U

/**
 * Minimum stack size in bytes. Must leave room for one hardware exception
 * frame (104 bytes with FPU lazy stacking) plus one software context frame
 * (100 bytes with FPU) on top of the task's own usage.
 */
#define OS_CONFIG_MIN_STACK_SIZE            256U

/* Stack-overflow detection. On every switch away from a task the kernel checks
 * that its stack pointer is still inside its own stack and that the guard word
 * at the bottom is intact; on a hit it calls os_stack_overflow_cb() and parks
 * the core, exactly as a failed OS_ASSERT does. Costs a compare and a load per
 * context switch.
 *
 * Worth leaving on. A blown stack otherwise corrupts whatever happens to sit
 * below it and the symptom appears somewhere unrelated, often much later.
 * ARMv8-M mainline traps this in hardware (per-task PSPLIM) whatever this is
 * set to; every other core - M0, M0+, M23, M3, M4, M7 - has no such register
 * and this is the only detection available. */
#define OS_CONFIG_STACK_CHECK_ENABLE        1U

/*
 * ***********************************************************************************************************
 * Default application task
 * ***********************************************************************************************************
*/

/* os_init() unconditionally creates and starts a default application task running os_main(),
 * which the application must define in its own os_main.c (copied from kernel/template/os_main.c) -
 * the kernel ships no stub, so a missing one is a link error. See the README "Default
 * application task" section. Not created when OS_CONFIG_TEST_ENABLE (PART 2) is 1: the
 * self-test task runs alone instead, and no os_main.c is needed at all.
 *
 * os_init() discards the creation status for this task (void, matching the timer
 * system-init calls) - an out-of-range priority (must be
 * OS_TASK_PRIO_1_LOWEST..OS_TASK_PRIO_30_HIGHEST) or a too-small stack (must be at least
 * OS_CONFIG_MIN_STACK_SIZE above) fails SILENTLY: the firmware still builds, boots and
 * schedules, but os_main() simply never runs. */
#define OS_CONFIG_MAIN_TASK_STACK_SIZE      1024U
#define OS_CONFIG_MAIN_TASK_PRIORITY        OS_TASK_PRIO_1

/*
 * ***********************************************************************************************************
 * Kernel interrupt mask (zero-latency interrupts, BASEPRI)
 * ***********************************************************************************************************
*/

/**
 * A priority NUMBER, not a bitmask: the raw 8-bit byte the NVIC stores, i.e. the logical
 * priority already shifted into the device's implemented priority bits.
 *
 *     value = logical_priority << (8 - __NVIC_PRIO_BITS)
 *
 * Example, a device with 4 implemented bits: logical 5 -> (5 << 4) = 0x50.
 *
 *   0        Critical sections mask ALL interrupts (PRIMASK). Every ISR may call the ISR-safe
 *            kernel APIs. The only choice on cores without BASEPRI (Cortex-M0/M0+/M23).
 *   nonzero  Critical sections raise BASEPRI to this value. ISRs more urgent than it
 *            (numerically lower) are never masked - zero kernel-induced latency - but MUST NOT
 *            call any kernel API. ISRs at this value or higher keep full API access.
 *
 * So with 0x50: logical 0..4 are zero-latency/no-kernel-API, logical 5..15 may use the kernel.
 * Verified at boot (parks in os_arch_config_fault_trap on violation): the value must fit the
 * implemented priority bits exactly, and NVIC grouping must give every implemented bit to
 * preemption, with no subpriority bits.
 */

#define OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY  0U

/*
 * ***********************************************************************************************************
 * PART 2 - OPTIONAL FEATURES (1 = compiled in, 0 = compiled out)
 * ***********************************************************************************************************
 *
 * Disabling a feature removes its code, its API, and - for timer/log -
 * its kernel service task and stack. Each section below carries its own
 * sizing, which is dead weight to reason about once the switch is 0.
*/

/*
 * ***********************************************************************************************************
 * Mutex
 * ***********************************************************************************************************
*/

/* Mutexes always do single-level priority inheritance: os_mutex_lock
 * boosts a lower-priority owner to the blocking waiter's (effective) priority for as long as it
 * holds the mutex, restoring it on os_mutex_unlock (accounting for other mutexes the task still
 * holds). Transitive/chained inheritance across multiple mutexes is NOT implemented - see
 * README "Timeout semantics". */
#define OS_CONFIG_MUTEX_ENABLE              1U

/*
 * ***********************************************************************************************************
 * Semaphore, queue, event
 * ***********************************************************************************************************
*/

#define OS_CONFIG_SEMAPHORE_ENABLE          1U
#define OS_CONFIG_QUEUE_ENABLE              1U
#define OS_CONFIG_EVENT_ENABLE              1U

/*
 * ***********************************************************************************************************
 * Software timers
 * ***********************************************************************************************************
*/

/* Timer callbacks run on the kernel timer task (tsk_timer), which occupies one
 * task-table slot the kernel reserves for itself - not one of
 * OS_CONFIG_MAX_USER_TASKS - and runs the callbacks on the stack sized here. */
#define OS_CONFIG_TIMER_ENABLE              1U
#define OS_CONFIG_TIMER_STACK_SIZE          512U

/* Priority of the timer service task, and so of every timer callback.
 *
 * OS_TASK_PRIO_MAX (31) by default, above every user task: an expiry then reaches its callback as
 * soon as the tick notes it, which is what makes a timer's timing depend on the timer rather than
 * on what else is runnable. Lower it when callbacks matter less than some user task - they run at
 * this priority for their whole duration, so a slow callback at 31 delays everything.
 *
 * Any level from OS_TASK_PRIO_1_LOWEST to OS_TASK_PRIO_MAX is allowed, including levels user
 * tasks also use; the timer task is a kernel service task either way, so os_task_pause and
 * os_task_delete keep refusing it whatever this is set to. */
#define OS_CONFIG_TIMER_PRIORITY            OS_TASK_PRIO_MAX

/* Which cores the timer task (and so the timer callbacks) may run on:
 * a core-affinity bitmask; OS_TASK_CORE_ANY (0) = any core.
 *
 * Default OS_TASK_CORE(0) because core 0 owns the time base and the priority
 * relationship between timer callbacks and the application's core-0 tasks
 * then means what it says. Only meaningful when OS_CONFIG_CORE_COUNT
 * (PART 3) > 1; OS_TASK_CORE(0) is accepted - and ignored - on a single-core
 * build, so one value serves both. */
#define OS_CONFIG_TIMER_CORE_AFFINITY       OS_TASK_CORE(0)

/*
 * ***********************************************************************************************************
 * Task notifications
 * ***********************************************************************************************************
*/

/* Task notifications: a single overwrite uint32_t "mailbox" built into every task's own
 * control block (os_notify_give / os_notify_wait) - lets one task or an ISR signal
 * a specific task directly without allocating a separate semaphore/queue object. */
#define OS_CONFIG_NOTIFY_ENABLE             1U

/*
 * ***********************************************************************************************************
 * Kernel heap
 * ***********************************************************************************************************
*/

/* Kernel heap (os_mem_alloc/os_mem_free): first-fit allocator with coalescing over a
 * static heap of OS_CONFIG_HEAP_SIZE bytes. Also what os_queue_init_dynamic allocates from. */
#define OS_CONFIG_ALLOC_ENABLE              1U
#define OS_CONFIG_HEAP_SIZE                 4096U

/*
 * ***********************************************************************************************************
 * Atomics
 * ***********************************************************************************************************
*/

/* Atomic operations on a single word (os_atomic_*): indivisible add/sub/bitwise/compare-and-swap
 * updates that no task, ISR or core can observe half-finished. Lock-free where the ISA has
 * LDREX/STREX, otherwise implemented by briefly excluding interrupts. Costs no RAM and no kernel
 * task; unused operations are dropped by the linker, so disabling this removes API surface rather
 * than footprint. */
#define OS_CONFIG_ATOMIC_ENABLE             1U

/*
 * ***********************************************************************************************************
 * Diagnostics
 * ***********************************************************************************************************
*/

/* Fill task stacks with a pattern at creation and provide
 * os_task_stack_watermark_get() to measure worst-case stack usage. */
#define OS_CONFIG_STACK_WATERMARK_ENABLE    1U

/* Sample CPU load from the tick interrupt (idle vs non-idle) and provide
 * os_cpu_usage_get(): percentage of ticks that interrupted a non-idle task
 * since the previous call. Costs two counter updates per tick. */
#define OS_CONFIG_CPU_USAGE_ENABLE          1U

/*
 * ***********************************************************************************************************
 * Assertions
 * ***********************************************************************************************************
*/

/* OS_ASSERT(): catch static programming errors (a NULL object handle, a blocking call made
 * from an ISR, an unbalanced critical section) at the point they happen instead of only
 * through a status code the caller may ignore. Runtime outcomes that have a documented
 * status - NOT_OWNER, BUSY, FULL, EMPTY, TIMEOUT - are never asserted on: callers are
 * entitled to attempt the operation and handle the result.
 *
 * A failure calls os_assert_failed_cb(), which the application must define, then parks the
 * core with interrupts masked so a debugger lands on the cause. Assertions only ADD checks:
 * every API still returns the same status either way, so turning this off leaves behavior
 * unchanged.
 *
 * This switch also carries MUTEX DEADLOCK DETECTION, which has no switch of its own because an
 * assertion is its only way to report. When a task is about to block FOREVER on a locked mutex,
 * the kernel follows the wait chain - who owns it, what is that owner itself blocked on - and
 * asserts if it arrives back at the caller, which is a deadlock. Priority inheritance cannot
 * prevent that (it fixes when a waiting task runs, not the ORDER two tasks took two locks in), so
 * catching it as it forms is the difference between a debugger stopping on the guilty call and a
 * board that silently stops with several tasks blocked forever.
 *
 * A lock with a timeout is never reported and never appears inside a reported cycle: it will give
 * up, so it is not deadlocked and it breaks any cycle it is in. Only a cycle in which EVERY task
 * waits forever is real, and only that is asserted on.
 *
 * Since an assertion carries just a file and a line, the mutexes and task names involved are left
 * in os_task_deadlock_report for the debugger to read after the halt. Costs that record, one
 * pointer per task, and a short walk on contended unbounded locks; at 0, none of it exists. */
#define OS_CONFIG_ASSERT_ENABLE             1U

/*
 * ***********************************************************************************************************
 * Buffered logging
 * ***********************************************************************************************************
*/

/* Buffered debug logging (OS_LOG_ERROR/WARN/INFO/DEBUG): printf-style calls format into a ring
 * buffer and return immediately, and a low-priority kernel task (tsk_log) hands finished bytes
 * to os_log_output_cb() for the application to transmit. Safe from tasks and ISRs, and it never
 * blocks the caller. Two costs to budget for: tsk_log occupies a kernel-reserved task-table slot (not one of OS_CONFIG_MAX_USER_TASKS),
 * and formatting uses libc vsnprintf, which pulls newlib's formatter into the link (~1-3 KB) if
 * the application does not already use printf. As usual, %f additionally needs the linker flag
 * -u _printf_float. See the README "Debugging" section. */
#define OS_CONFIG_LOG_ENABLE                1U

/**
 * Log sizing (only meaningful when OS_CONFIG_LOG_ENABLE above is 1).
 *
 * LEVEL        Calls above this compile to nothing at the call site, arguments included:
 *              OS_LOG_LEVEL_NONE / _ERROR / _WARN / _INFO / _DEBUG.
 * BUFFER_SIZE  Ring buffer in bytes. Sized for the burst you want to survive: a task logging
 *              faster than the UART drains simply loses the excess (counted, then reported).
 * LINE_MAX     Longest single formatted line. Also the scratch buffer os_log_write() puts on
 *              the CALLER's stack, so every task that logs needs this much headroom. Longer
 *              lines are truncated, never overflowed.
 * TASK_*       tsk_log, which drains the ring and calls os_log_output_cb(). Its stack must hold
 *              that callback. Keep the priority low: logging must never preempt real work.
 *              CORE_AFFINITY places the drain task on multi-core builds. Default
 *              OS_TASK_CORE(0): a fixed core keeps the drain's priority relationship to that
 *              core's tasks meaningful - which the self-test's overrun checks depend on (they
 *              flood from core 0 and count on tsk_log being starved there). OS_TASK_CORE_ANY
 *              lets it follow the work instead, but the suite then fails its drop checks on
 *              multi-core. OS_TASK_CORE(0) is accepted - and ignored - on a single-core build.
 */
#define OS_CONFIG_LOG_LEVEL                 OS_LOG_LEVEL_INFO
#define OS_CONFIG_LOG_BUFFER_SIZE           1024U
#define OS_CONFIG_LOG_LINE_MAX              128U
#define OS_CONFIG_LOG_TASK_STACK_SIZE       512U
#define OS_CONFIG_LOG_TASK_PRIORITY         OS_TASK_PRIO_1
#define OS_CONFIG_LOG_CORE_AFFINITY         OS_TASK_CORE(0)

/*
 * ***********************************************************************************************************
 * Self-test suite
 * ***********************************************************************************************************
*/

/* os_init() creates and starts a self-test task running os_test(), which comes from the
 * ahura_kernel/test library (the "os_test" CMake target) - link it and the suite runs, forget
 * it and the link fails. Off by default: opt in per project. When 1, the default application
 * task (OS_CONFIG_MAIN_TASK_* in PART 1) is NOT created and the suite runs alone; see the
 * README "Self-test suite" section.
 *
 * The suite itself needs a generous stack - it exercises every kernel feature, including
 * nested helper tasks. Same silent-failure caveat as OS_CONFIG_MAIN_TASK_*. */
#define OS_CONFIG_TEST_ENABLE               0U
#define OS_CONFIG_TEST_STACK_SIZE           2048U
#define OS_CONFIG_TEST_PRIORITY             OS_TASK_PRIO_2

/*
 * ***********************************************************************************************************
 * PART 3 - PLATFORM (target properties, not application design choices)
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * Exception vector the kernel owns
 *
 * The NAME of that vector is not here: it is a fact about the target's
 * startup code, so the SoC package sets it (OS_CONFIG_ARCH_PENDSV_HANDLER,
 * see doc/soc.md). It defaults to the CMSIS-Pack name PendSV_Handler, which
 * is what essentially every vendor startup file uses. Defining it here as
 * well would override the SoC package silently, because this file is read
 * after the build system's -D.
 * ***********************************************************************************************************
*/

/**
 * Check at boot that the live vector table really routes PendSV to the kernel
 * (1 = on, 0 = skip).
 *
 * Worth leaving on. Without it, the failure is silent and expensive: if some
 * other definition of the handler name wins at link time, or a bootloader
 * relocated the vector table and repopulated it from its own image, the build
 * still succeeds and the board simply stops at os_start() - no fault, no
 * output, nothing to attach to. The check turns that into an immediate park in
 * os_arch_config_fault_trap(), where the debugger lands on the cause. It costs
 * one load and one compare, once.
 *
 * Set it to 0 only for a boot flow whose vector table genuinely cannot be read
 * when os_init() runs - one patched in later, or behind a memory window VTOR
 * does not describe.
 *
 * This option is OPTIONAL: leaving it out enables the check.
 */
#define OS_CONFIG_ARCH_VECTOR_CHECK         1U

/*
 * ***********************************************************************************************************
 * TrustZone security state (ARMv8-M cores only)
 * ***********************************************************************************************************
*/

/**
 * Selects which ARMv8-M security state the kernel runs in; the three value
 * macros are kernel-owned (os_arch_port_common.h). On cores without the
 * Security Extension (M0/M0+/M3/M4/M7, or v8-M devices with TrustZone
 * disabled) keep OS_CONFIG_TRUSTZONE_DISABLED.
 *
 *   OS_CONFIG_TRUSTZONE_DISABLED    The kernel ignores TrustZone. Use when the
 *                                   Security Extension is absent or disabled.
 *   OS_CONFIG_TRUSTZONE_NON_SECURE  The kernel and all tasks run non-secure
 *                                   alongside separate secure firmware. Task
 *                                   frames use the non-secure EXC_RETURN and
 *                                   the port calls os_arch_tz_context_save_cb()
 *                                   / os_arch_tz_context_restore_cb() around
 *                                   every context switch so the application's
 *                                   secure-side glue can bank per-task secure
 *                                   contexts (secure stack / PSP_S).
 *   OS_CONFIG_TRUSTZONE_SECURE      The kernel and all tasks run entirely in
 *                                   the secure state; compile with -mcmse.
 */

#define OS_CONFIG_TRUSTZONE                 OS_CONFIG_TRUSTZONE_DISABLED

/*
 * ***********************************************************************************************************
 * Multi-core (experimental scaffold)
 * ***********************************************************************************************************
*/

/**
 * Number of cores that schedule tasks (max 31). Each runs its own PendSV and idle task and pulls
 * from the shared ready lists honoring core_affinity; core 0 owns the time base, and secondary
 * cores enter through os_core_start(). The SoC layer must supply os_arch_core_id_get_cb(), the IPI
 * callback, and - on cores without LDREX/STREX - the spinlock callbacks. A SoC
 * package under kernel/soc/ supplies all three; see doc/soc.md.
 *
 * Two preconditions the kernel cannot verify, both SoC properties, before setting this above 1:
 *
 *   1. Global exclusive monitor. The inter-core spinlock uses LDREX/STREX, which only excludes
 *      another core when the interconnect implements a GLOBAL monitor for that address AND the
 *      address is Shareable-mapped. Without both, two cores can succeed at once and the lock
 *      silently stops excluding anything. The SoC package's
 *      OS_CONFIG_SPINLOCK_SOC_BACKEND routes it through a hardware semaphore instead.
 *   2. Cache coherency. Cortex-M has none between cores, and DSB is not cache maintenance. Every
 *      shared kernel object (ready/delay lists, the registries, os_task_current[], the spinlock
 *      word) must sit in a non-cacheable or coherent region - typically the whole kernel's shared
 *      statics placed in one non-cacheable MPU region by the linker script. No portable fallback.
 *
 * Service tasks are placed with OS_CONFIG_TIMER_CORE_AFFINITY / OS_CONFIG_LOG_CORE_AFFINITY.
 */

#define OS_CONFIG_CORE_COUNT                1U

/*
 * ***********************************************************************************************************
 * Tickless idle (experimental scaffold, not functional yet)
 * ***********************************************************************************************************
*/

#define OS_CONFIG_TICKLESS_ENABLE           0U
#define OS_CONFIG_TICKLESS_MIN_IDLE         2U
#define OS_CONFIG_MAX_SUPPRESSED_TICKS      0x00FFFFFFUL

#endif /* OS_CONFIG_H */
