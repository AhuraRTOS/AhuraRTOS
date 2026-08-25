/**
 * @file os_config.h
 * @brief Template for the application's os_config.h - every kernel option at its default value.
 *
 * NOT included by the kernel: copy it into the application source tree as os_config.h, adjust the
 * values, and make its directory visible to BOTH the application and the kernel library build
 * (OS_CONFIG_DIR). The full reference is doc/integration.md "Configuration".
 *
 * This file is the single source of configuration, so do not also define OS_CONFIG_ macros from
 * the build system, and do not remove options: an incomplete configuration is rejected at compile
 * time rather than silently misconfiguring the kernel. Two options are OPTIONAL and say so where
 * they appear; everything else is mandatory.
 *
 * Every option carries a short note above it, ending with "Values:" - what may legally go there.
 *
 *   PART 1  CORE      always compiled in; no switch removes any of it. Set these first.
 *   PART 2  FEATURES  one section per feature: its _ENABLE switch and the sizing that switch
 *                     controls. 1 = compiled in, 0 = compiled out, API and RAM with it.
 *   PART 3  PLATFORM  properties of the target. Facts about the silicon - the vector name, the
 *                     spinlock backend - belong to the SoC package instead, not here.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_CONFIG_H
#define OS_CONFIG_H

/*
 * ***********************************************************************************************************
 * PART 1 - CORE
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * Tick rate
 * ***********************************************************************************************************
 *
 * The CPU clock is NOT configured here: the kernel reads the live CMSIS SystemCoreClock, which the
 * device's own startup sets and SystemCoreClockUpdate() refreshes. See doc/porting.md.
*/

/* Tick rate: every delay, timeout, timer and round-robin slice is counted in
 * these ticks, so this one number is the resolution of all of them.
 * Values: ticks per second; 1000 = 1 ms. */
#define OS_CONFIG_TICK_HZ                   1000U

/* Who owns the tick timer. SYSTICK: the port programs it from the live CPU
 * clock, with nothing to route. EXTERNAL: the application starts its own timer
 * in os_arch_tick_init_cb() and that ISR calls os_tick_handler() - for parts
 * whose SysTick stops in low power, or where something else already owns it.
 * OPTIONAL: leaving it out selects SYSTICK.
 * Values: OS_CONFIG_TICK_SOURCE_SYSTICK, OS_CONFIG_TICK_SOURCE_EXTERNAL. */
#define OS_CONFIG_TICK_SOURCE               OS_CONFIG_TICK_SOURCE_SYSTICK

/* Round-robin slice: how long a task may hold the CPU before an equal-priority
 * peer takes over. A higher-priority task preempts immediately regardless.
 * Values: ticks; 1 = rotate every tick, 0 = no rotation at all. */
#define OS_CONFIG_TIME_SLICE_TICKS          1U

/*
 * ***********************************************************************************************************
 * Task table and stacks
 * ***********************************************************************************************************
*/

/* How many tasks the APPLICATION may have. The kernel's service tasks get
 * their slots on top of this, so enabling the timer or the log costs none of
 * it. This is exactly what os_task_create() accepts before OS_ERR_FULL.
 * Values: 1..n; the self-test suite needs 8. */
#define OS_CONFIG_MAX_USER_TASKS            8U

/* Keep the name OS_TASK_DEFINE gave each task, for os_task_name_get(), a
 * debugger, the stack-overflow callback and the deadlock report. At 0 the
 * pointer leaves every TCB and the strings leave flash, and every name reads
 * back NULL - which is documented rather than an error.
 * Values: 1 = keep names, 0 = drop them. */
#define OS_CONFIG_TASK_NAME_ENABLE          1U

/* Floor for every task stack, and the idle task's own size. Must leave room
 * for one hardware exception frame (104 bytes with FPU lazy stacking) plus one
 * software context frame (100 bytes with FPU) above the task's own usage.
 * Values: bytes; at least 128, and a multiple of 8. */
#define OS_CONFIG_MIN_STACK_SIZE            256U

/* On every switch away from a task, check that its stack pointer is still
 * inside its own stack and that the guard word at the bottom is intact; on a
 * hit, call os_stack_overflow_cb() and park the core. ARMv8-M mainline traps
 * this in hardware anyway; on M0/M0+/M23/M3/M4/M7 it is the only detection.
 * Values: 1 = check, 0 = skip. */
#define OS_CONFIG_STACK_CHECK_ENABLE        1U

/*
 * ***********************************************************************************************************
 * Default application task
 * ***********************************************************************************************************
 *
 * os_init() always creates a task running os_main(), which the application defines in its own
 * os_main.c - the kernel ships no stub, so a missing one is a link error. Not created when
 * OS_CONFIG_TEST_ENABLE is 1. See doc/api.md "Default application task".
 *
 * Both values below fail SILENTLY if wrong: os_init() discards the creation status, so the board
 * builds, boots and schedules, and os_main() simply never runs.
*/

/* Values: bytes; at least OS_CONFIG_MIN_STACK_SIZE. */
#define OS_CONFIG_MAIN_TASK_STACK_SIZE      1024U

/* Values: OS_TASK_PRIO_1 .. OS_TASK_PRIO_30. */
#define OS_CONFIG_MAIN_TASK_PRIORITY        OS_TASK_PRIO_1

/*
 * ***********************************************************************************************************
 * Kernel interrupt mask
 * ***********************************************************************************************************
*/

/* Highest interrupt priority the kernel masks inside a critical section.
 *
 * 0 masks everything with PRIMASK. Nonzero switches to BASEPRI, so interrupts
 * MORE urgent than this keep running even inside the kernel - and must never
 * call a kernel API. It is the raw NVIC priority byte, not a logical level:
 * shift the logical priority left by (8 - implemented priority bits), e.g.
 * logical 5 on a 4-bit part is (5 << 4) = 0x50. Verified at boot.
 * See doc/design.md "The three barriers".
 * Values: 0, or a shifted NVIC priority byte. */
#define OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY  0U

/*
 * ***********************************************************************************************************
 * PART 2 - OPTIONAL FEATURES
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * Mutex
 * ***********************************************************************************************************
*/

/* Mutexes (os_mutex_*), always with single-level priority inheritance: a
 * lower-priority owner is boosted to the highest waiter's priority until it
 * unlocks. Chained inheritance across several mutexes is not implemented.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_MUTEX_ENABLE              1U

/*
 * ***********************************************************************************************************
 * Semaphore, queue, message buffer, event
 * ***********************************************************************************************************
*/

/* Counting semaphores (os_semaphore_*): a token count tasks take from and give
 * back to - for signalling, and for capping concurrent access.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_SEMAPHORE_ENABLE          1U

/* Queues (os_queue_*): N items of ONE fixed size, copied between tasks or in
 * from an ISR. For messages whose length varies, see MSG below.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_QUEUE_ENABLE              1U

/* Message buffers (os_msg_*): whole messages of VARYING length out of one byte
 * budget, each costing its own length plus a small header. Reach for it when
 * the length is data - a protocol frame, a console line. Independent of the
 * queue switch; neither is built on the other, and it costs no RAM of its own.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_MSG_ENABLE                1U

/* Event groups (os_event_*): 32 bits several tasks can wait on, all of them or
 * any of them - for waiting on a CONDITION rather than on data arriving.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_EVENT_ENABLE              1U

/*
 * ***********************************************************************************************************
 * Software timers
 * ***********************************************************************************************************
 *
 * Software timers and deferred calls (os_timer_*). Callbacks run on the kernel timer task
 * tsk_timer, which takes a kernel-reserved slot - not one of OS_CONFIG_MAX_USER_TASKS.
*/

/* Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_TIMER_ENABLE              1U

/* tsk_timer's stack: it must hold your timer callbacks.
 * Values: bytes; at least OS_CONFIG_MIN_STACK_SIZE. */
#define OS_CONFIG_TIMER_STACK_SIZE          512U

/* Priority of tsk_timer. OS_TASK_PRIO_MAX puts an expiry above every user
 * task; lower it into the user range when real work must outrank a callback.
 * Values: OS_TASK_PRIO_1 .. OS_TASK_PRIO_MAX. */
#define OS_CONFIG_TIMER_PRIORITY            OS_TASK_PRIO_MAX

/* Which core runs tsk_timer. Ignored on single-core builds; core 0 by default
 * because that is where the time base lives.
 * Values: OS_TASK_CORE(n), or OS_TASK_CORE_ANY to follow the work. */
#define OS_CONFIG_TIMER_CORE_AFFINITY       OS_TASK_CORE(0)

/*
 * ***********************************************************************************************************
 * Task notifications
 * ***********************************************************************************************************
*/

/* Direct-to-task notifications (os_notify_*): a one-word mailbox built into
 * every task's own control block, so one task or an ISR can signal a specific
 * task without a separate semaphore or queue object.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_NOTIFY_ENABLE             1U

/*
 * ***********************************************************************************************************
 * Kernel heap
 * ***********************************************************************************************************
 *
 * os_mem_alloc / os_mem_free: a first-fit allocator with coalescing over one static array. Also
 * what os_queue_init_dynamic() allocates from.
*/

/* Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_ALLOC_ENABLE              1U

/* Values: bytes of static heap. */
#define OS_CONFIG_HEAP_SIZE                 4096U

/*
 * ***********************************************************************************************************
 * Atomics
 * ***********************************************************************************************************
*/

/* Atomic single-word operations (os_atomic_*): add, bitwise and
 * compare-and-swap updates no task, ISR or core can observe half-finished.
 * Lock-free where the ISA has LDREX/STREX, otherwise by briefly masking
 * interrupts. Costs no RAM and no kernel task.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_ATOMIC_ENABLE             1U

/*
 * ***********************************************************************************************************
 * Diagnostics
 * ***********************************************************************************************************
*/

/* Fill task stacks with a pattern at creation so os_task_stack_watermark_get()
 * can report worst-case usage. A measurement you poll, not a detector - that
 * is OS_CONFIG_STACK_CHECK_ENABLE above.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_STACK_WATERMARK_ENABLE    1U

/* Sample CPU load from the tick interrupt (idle versus not) for
 * os_cpu_usage_get(), which reports the percentage since the previous call.
 * Costs two counter updates per tick.
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_CPU_USAGE_ENABLE          1U

/*
 * ***********************************************************************************************************
 * Assertions
 * ***********************************************************************************************************
*/

/* OS_ASSERT(): catch programming errors - a NULL handle, a blocking call from
 * an ISR, an unbalanced critical section - at the point they happen. A failure
 * calls os_assert_failed_cb(), which the application must define, then parks
 * the core. Documented outcomes (BUSY, FULL, EMPTY, TIMEOUT, NOT_OWNER) are
 * never asserted on, and no API returns a different status either way.
 *
 * This switch also carries MUTEX DEADLOCK DETECTION, which has no switch of
 * its own because an assertion is its only way to report: a task about to
 * block FOREVER on a locked mutex has its wait chain walked, and a cycle back
 * to itself is asserted on. A lock with a timeout is never reported - it will
 * give up, so it cannot deadlock. The mutexes and task names involved are left
 * in os_task_deadlock_report for the debugger. See doc/api.md "Debugging".
 * Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_ASSERT_ENABLE             1U

/*
 * ***********************************************************************************************************
 * Buffered logging
 * ***********************************************************************************************************
 *
 * OS_LOG_ERROR / WARN / INFO / DEBUG: printf-style calls format into a ring buffer and return
 * immediately, and the kernel task tsk_log hands finished bytes to os_log_output_cb() for the
 * application to transmit. Safe from tasks and ISRs, and it never blocks the caller.
 *
 * Two costs: tsk_log takes a kernel-reserved task slot, and formatting pulls newlib's vsnprintf
 * into the link (~1-3 KB) unless the application already uses printf. See doc/api.md "Debugging".
*/

/* Values: 1 = on, 0 = compiled out. */
#define OS_CONFIG_LOG_ENABLE                1U

/* Calls above this level compile to nothing at the call site, arguments
 * included.
 * Values: OS_LOG_LEVEL_NONE, _ERROR, _WARN, _INFO, _DEBUG. */
#define OS_CONFIG_LOG_LEVEL                 OS_LOG_LEVEL_INFO

/* The ring buffer. Size it for the burst you want to survive: a task logging
 * faster than the transport drains loses the excess, counted then reported.
 * Values: bytes. */
#define OS_CONFIG_LOG_BUFFER_SIZE           1024U

/* Longest single formatted line. Also the scratch buffer os_log_write() puts
 * on the CALLER's stack, so every task that logs needs this much headroom.
 * Longer lines are truncated, never overflowed.
 * Values: bytes. */
#define OS_CONFIG_LOG_LINE_MAX              128U

/* tsk_log's stack: it must hold os_log_output_cb().
 * Values: bytes; at least OS_CONFIG_MIN_STACK_SIZE. */
#define OS_CONFIG_LOG_TASK_STACK_SIZE       512U

/* Keep it low - logging must never preempt real work.
 * Values: OS_TASK_PRIO_1 .. OS_TASK_PRIO_30. */
#define OS_CONFIG_LOG_TASK_PRIORITY         OS_TASK_PRIO_1

/* Which core drains the log. Ignored on single-core builds; a fixed core keeps
 * the drain's priority relationship to that core's tasks meaningful, which the
 * self-test's overrun checks depend on.
 * Values: OS_TASK_CORE(n), or OS_TASK_CORE_ANY to follow the work. */
#define OS_CONFIG_LOG_CORE_AFFINITY         OS_TASK_CORE(0)

/*
 * ***********************************************************************************************************
 * Self-test suite
 * ***********************************************************************************************************
 *
 * At 1, os_init() runs os_test() instead of creating the default application task, and the
 * AhuraRTOS/test library must be linked - nothing in the kernel defines os_test(), so forgetting
 * it is a link error rather than a build that silently tests nothing. Off by default: opt in per
 * project. See doc/testing.md "Self-test suite".
*/

/* Values: 1 = run the suite, 0 = run os_main(). */
#define OS_CONFIG_TEST_ENABLE               0U

/* The suite exercises every kernel feature, including nested helper tasks.
 * Same silent-failure caveat as OS_CONFIG_MAIN_TASK_* above.
 * Values: bytes; at least OS_CONFIG_MIN_STACK_SIZE. */
#define OS_CONFIG_TEST_STACK_SIZE           2048U

/* Values: OS_TASK_PRIO_1 .. OS_TASK_PRIO_30. */
#define OS_CONFIG_TEST_PRIORITY             OS_TASK_PRIO_2

/*
 * ***********************************************************************************************************
 * PART 3 - PLATFORM
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * Exception vector the kernel owns
 * ***********************************************************************************************************
 *
 * The NAME of that vector is not here - it is a fact about the target's startup code, so the SoC
 * package sets it (OS_CONFIG_ARCH_PENDSV_HANDLER, see doc/soc.md). It defaults to the CMSIS-Pack
 * name PendSV_Handler. Defining it here as well would override the package silently, because this
 * file is read after the build system's -D.
*/

/* Check at boot that the live vector table really routes that vector to the
 * kernel. Without it the failure is silent and expensive: the build succeeds
 * and the board simply stops at os_start(), with no fault and no output. Set
 * to 0 only for a boot flow whose vector table genuinely cannot be read when
 * os_init() runs. OPTIONAL: leaving it out enables the check.
 * Values: 1 = check, 0 = skip. */
#define OS_CONFIG_ARCH_VECTOR_CHECK         1U

/*
 * ***********************************************************************************************************
 * TrustZone security state (ARMv8-M cores only)
 * ***********************************************************************************************************
*/

/* Which ARMv8-M security state the kernel runs in; the value macros are
 * kernel-owned. DISABLED on any core without the Security Extension, and on
 * v8-M devices with TrustZone off. NON_SECURE runs the kernel and all tasks
 * non-secure beside separate secure firmware, calling
 * os_arch_tz_context_save_cb() / _restore_cb() around every switch so the
 * application can bank a per-task secure context. SECURE runs everything
 * secure-side; compile with -mcmse. See doc/porting.md "TrustZone".
 * Values: OS_CONFIG_TRUSTZONE_DISABLED, _NON_SECURE, _SECURE. */
#define OS_CONFIG_TRUSTZONE                 OS_CONFIG_TRUSTZONE_DISABLED

/*
 * ***********************************************************************************************************
 * Multi-core (experimental scaffold)
 * ***********************************************************************************************************
*/

/* Cores that schedule tasks. Each runs its own idle task and pulls from the
 * shared ready lists honoring core_affinity; core 0 owns the time base, and
 * secondary cores enter through os_core_start(). The SoC layer supplies the
 * core id, the IPI and - without LDREX/STREX - the spinlock callbacks.
 *
 * Two preconditions the kernel cannot verify before this goes above 1. The
 * interconnect must implement a GLOBAL exclusive monitor for the spinlock
 * address, or the SoC package must route the lock to a hardware semaphore. And
 * every shared kernel static must sit in non-cacheable or coherent memory:
 * Cortex-M has no coherency between cores, and DSB is not cache maintenance.
 * See doc/porting.md "Multi-core".
 * Values: 1..31. */
#define OS_CONFIG_CORE_COUNT                1U

/*
 * ***********************************************************************************************************
 * Tickless idle (experimental scaffold, not functional yet)
 * ***********************************************************************************************************
 *
 * Instead of waking the CPU OS_CONFIG_TICK_HZ times a second with nothing to do, the idle path
 * reprograms the tick timer for the whole planned sleep. Not wired in yet: at 1 today, only the
 * hooks the ports report through exist. See doc/porting.md "Tickless idle".
*/

/* Values: 1 = allow suppression, 0 = plain WFI. */
#define OS_CONFIG_TICKLESS_ENABLE           0U

/* Shortest planned idle worth suppressing for; below it the wake-up costs more
 * than the sleep saves.
 * Values: ticks. */
#define OS_CONFIG_TICKLESS_MIN_IDLE         2U

/* Ceiling on a single suppressed window. The real limit is usually the tick
 * timer's register width - SysTick is 24 bits, which the default reflects.
 * Values: ticks. */
#define OS_CONFIG_MAX_SUPPRESSED_TICKS      0x00FFFFFFUL

#endif /* OS_CONFIG_H */
