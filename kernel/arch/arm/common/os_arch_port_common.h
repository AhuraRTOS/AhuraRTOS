/**
 * @file os_arch_port_common.h
 * @brief Common architecture port interface shared by all ARM Cortex-M variants.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

#ifndef OS_ARCH_PORT_COMMON_H
#define OS_ARCH_PORT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TrustZone mode values for OS_CONFIG_TRUSTZONE: kernel-owned so an
 * application configuration can reference but never change the encoding. */
#define OS_CONFIG_TRUSTZONE_DISABLED    0U
#define OS_CONFIG_TRUSTZONE_NON_SECURE  1U
#define OS_CONFIG_TRUSTZONE_SECURE      2U

/* Tick source values for OS_CONFIG_TICK_SOURCE, kernel-owned on the same terms
 * as the TrustZone modes above. See the "Tick source" block further down. */
#define OS_CONFIG_TICK_SOURCE_SYSTICK   0U
#define OS_CONFIG_TICK_SOURCE_EXTERNAL  1U

/*
 * The application provides the kernel configuration: copy
 * kernel/template/os_config.h into the project as os_config.h
 * and make its directory visible to the kernel build (OS_CONFIG_DIR in
 * CMake, see the README "Configuration" section).
 */
#if defined(__has_include)
#if !__has_include("os_config.h")
#error "No os_config.h found: copy kernel/template/os_config.h into your project as os_config.h and put its directory on the kernel include path (OS_CONFIG_DIR)."
#endif
#endif

#include "os_config.h"

/* Reject incomplete configurations: a missing option would otherwise read
 * as 0 in #if directives and silently disable or misconfigure features.
 * Start from template/os_config.h, which lists every required option. */
#if !defined(OS_CONFIG_MUTEX_ENABLE) || !defined(OS_CONFIG_SEMAPHORE_ENABLE) ||                        \
    !defined(OS_CONFIG_QUEUE_ENABLE) || !defined(OS_CONFIG_EVENT_ENABLE) ||                            \
    !defined(OS_CONFIG_TIMER_ENABLE) || !defined(OS_CONFIG_WORK_ENABLE) ||                             \
    !defined(OS_CONFIG_ALLOC_ENABLE) ||                                                                \
    !defined(OS_CONFIG_STACK_WATERMARK_ENABLE) ||                                                      \
    !defined(OS_CONFIG_STACK_CHECK_ENABLE) ||                                                          \
    !defined(OS_CONFIG_CPU_USAGE_ENABLE) ||                                                            \
    !defined(OS_CONFIG_ASSERT_ENABLE) || !defined(OS_CONFIG_LOG_ENABLE) ||                             \
    !defined(OS_CONFIG_ATOMIC_ENABLE) || !defined(OS_CONFIG_NOTIFY_ENABLE) ||                          \
    !defined(OS_CONFIG_LOG_LEVEL) || !defined(OS_CONFIG_LOG_BUFFER_SIZE) ||                            \
    !defined(OS_CONFIG_LOG_LINE_MAX) || !defined(OS_CONFIG_LOG_TASK_STACK_SIZE) ||                     \
    !defined(OS_CONFIG_LOG_TASK_PRIORITY) ||                                                           \
    !defined(OS_CONFIG_TEST_ENABLE) ||                                                                 \
    !defined(OS_CONFIG_TICK_HZ) || !defined(OS_CONFIG_TIME_SLICE_TICKS) ||                             \
    !defined(OS_CONFIG_HEAP_SIZE) ||                                                                   \
    !defined(OS_CONFIG_MAX_USER_TASKS) ||                                                              \
    !defined(OS_CONFIG_MAX_TIMERS) || !defined(OS_CONFIG_MAX_WORKS) ||                                 \
    !defined(OS_CONFIG_WORK_PAYLOAD_SIZE) ||                                                           \
    !defined(OS_CONFIG_MIN_STACK_SIZE) || !defined(OS_CONFIG_WORK_STACK_SIZE) ||                           !defined(OS_CONFIG_TIMER_PRIORITY) || !defined(OS_CONFIG_WORK_PRIORITY) ||                         \
    !defined(OS_CONFIG_TIMER_STACK_SIZE) || !defined(OS_CONFIG_WORK_CORE_AFFINITY) ||                  \
    !defined(OS_CONFIG_TIMER_CORE_AFFINITY) ||                                                         \
    !defined(OS_CONFIG_MAIN_TASK_STACK_SIZE) || !defined(OS_CONFIG_MAIN_TASK_PRIORITY) ||               \
    !defined(OS_CONFIG_TEST_STACK_SIZE) || !defined(OS_CONFIG_TEST_PRIORITY) ||                        \
    !defined(OS_CONFIG_TRUSTZONE) || !defined(OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY) ||             \
    !defined(OS_CONFIG_CORE_COUNT) || !defined(OS_CONFIG_SPINLOCK_SOC_BACKEND) ||            \
    !defined(OS_CONFIG_TICKLESS_ENABLE) ||                                                             \
    !defined(OS_CONFIG_TICKLESS_MIN_IDLE) || !defined(OS_CONFIG_LPTIM_CLOCK_HZ) ||                     \
    !defined(OS_CONFIG_MAX_SUPPRESSED_TICKS)
#error "os_config.h is incomplete: it must define every option listed in kernel/template/os_config.h."
#endif

#if (OS_CONFIG_CORE_COUNT < 1U)
#error "OS_CONFIG_CORE_COUNT must be at least 1."
#endif

#if (OS_CONFIG_CORE_COUNT > 31U)
#error "OS_CONFIG_CORE_COUNT must be at most 31 (core affinity masks are 32 bits wide)."
#endif

/*
 * ***********************************************************************************************************
 * OPTIONAL configuration (the only options os_config.h may leave out)
 * ***********************************************************************************************************
 *
 * Everything checked above is mandatory, because a missing sizing or feature
 * switch would read as 0 in an #if and silently disable or misconfigure
 * something. The three below are different in kind: each is a NAME or a
 * yes/no diagnostic that the kernel can default correctly on its own, and a
 * missing one is caught by #ifndef rather than misread as 0. They are
 * documented in template/os_config.h, so a config copied from the template
 * carries them, but an older config that predates them still builds and
 * behaves exactly as it did before.
*/

/*
 * The symbol the port gives the PendSV exception handler - the ONE vector the
 * kernel must own, because a context switch has to be the exception entry
 * point itself (it manipulates the frame the hardware pushed and returns
 * through EXC_RETURN, neither of which survives an ordinary C call).
 *
 * PendSV_Handler is the CMSIS-Pack convention, which is what essentially every
 * vendor startup file uses - ST, Nordic, NXP, TI, Silicon Labs, Renesas,
 * Microchip, Infineon - so the default needs no configuration on any of them.
 * Override it when the vector table uses a different name (a hand-written
 * startup file, a non-CMSIS RTOS environment, a bootloader's own table).
 *
 * The kernel claims nothing else. SysTick is routed by the application
 * (see "Tick source" below) and SVC is left entirely alone - see the
 * context-switch comment in the port .c files for why the first task no
 * longer boots through it.
 */
#ifndef OS_CONFIG_ARCH_PENDSV_HANDLER
#define OS_CONFIG_ARCH_PENDSV_HANDLER  PendSV_Handler
#endif

/*
 * Boot-time check that the live vector table really routes PendSV to the
 * kernel (1 = on, the default; 0 = skip).
 *
 * Worth leaving on. The failure it catches is otherwise silent and very
 * expensive to debug: if some other definition of the handler name wins at
 * link time, or the table was relocated and re-populated without the kernel
 * entry, os_start() simply never switches and the board hangs with no clue
 * why. This turns that into an immediate park in os_arch_config_fault_trap(),
 * where the debugger lands on the cause.
 *
 * Set it to 0 only for a boot flow whose vector table genuinely cannot be read
 * at os_init() time - e.g. a table that is patched in later, or one behind a
 * memory window VTOR does not describe.
 */
#ifndef OS_CONFIG_ARCH_VECTOR_CHECK
#define OS_CONFIG_ARCH_VECTOR_CHECK    1U
#endif

/*
 * Where the kernel tick comes from.
 *
 *   OS_CONFIG_TICK_SOURCE_SYSTICK   The port programs SysTick from
 *                                   SystemCoreClock and OS_CONFIG_TICK_HZ. The
 *                                   default, and the right answer whenever
 *                                   SysTick is free and keeps running in
 *                                   whatever sleep modes the product uses.
 *
 *   OS_CONFIG_TICK_SOURCE_EXTERNAL  The application owns the tick hardware:
 *                                   the port programs nothing, calls
 *                                   os_arch_tick_init_cb() so the application
 *                                   can start its own timer, and expects
 *                                   os_tick_handler() to be called from that
 *                                   timer's ISR at OS_CONFIG_TICK_HZ.
 *
 * EXTERNAL exists because SysTick is not universally usable. It does not run
 * in the low-power modes several families actually ship with (Nordic nRF5x
 * drives time from the RTC peripheral for exactly this reason), some SoCs do
 * not implement it at all, and on others it is already spoken for by a vendor
 * HAL or a bootloader. None of that is something the kernel can detect, so it
 * is a configuration choice rather than a guess.
 */
#ifndef OS_CONFIG_TICK_SOURCE
#define OS_CONFIG_TICK_SOURCE          OS_CONFIG_TICK_SOURCE_SYSTICK
#endif

#if (OS_CONFIG_TICK_SOURCE != OS_CONFIG_TICK_SOURCE_SYSTICK) &&                                        \
    (OS_CONFIG_TICK_SOURCE != OS_CONFIG_TICK_SOURCE_EXTERNAL)
#error "OS_CONFIG_TICK_SOURCE must be OS_CONFIG_TICK_SOURCE_SYSTICK or OS_CONFIG_TICK_SOURCE_EXTERNAL."
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* CMSIS-style register qualifiers; defined here so the kernel does not depend
 * on a CMSIS core header (identical to the CMSIS definitions when both are
 * seen). Use __IO instead of a bare volatile for memory-mapped registers and
 * shared kernel state. */
#ifndef __IO
#define __IO volatile             /*!< read/write */
#endif
#ifndef __I
#define __I  volatile const       /*!< read only  */
#endif
#ifndef __O
#define __O  volatile             /*!< write only */
#endif

/* Weak-linkage marker for user-overridable defaults (the _cb callbacks and
 * optional linker symbols). Same ordering rule as OS_STACK_ALIGNED in ahura.h:
 * armclang also defines __clang__, and clang also defines __GNUC__, so the
 * most specific test comes first or the later branches never match. */
#ifndef OS_WEAK
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)
#define OS_WEAK __attribute__((weak))   /* Arm Compiler 6 (armclang) */
#elif defined(__clang__)
#define OS_WEAK __attribute__((weak))   /* LLVM clang                */
#elif defined(__GNUC__)
#define OS_WEAK __attribute__((weak))   /* GNU GCC                   */
#else
#define OS_WEAK
#endif
#endif

/*
 * Inline even at -O0, where a plain "static inline" is not inlined at all - the compiler emits it
 * as an ordinary local function and calls it.
 *
 * Used only on the atomics below, where the body is smaller than the call sequence reaching it, so
 * forcing it is a win on both size and speed at every optimization level rather than a trade. It
 * also keeps a debug build's atomic path the same shape as a release build's, which matters because
 * the self-test suite's benchmark table is normally read from exactly such a build during bring-up.
 *
 * Same compiler ordering rule as OS_WEAK above: armclang also defines __clang__, and clang also
 * defines __GNUC__, so the most specific test has to come first.
 */
#ifndef OS_ALWAYS_INLINE
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)
#define OS_ALWAYS_INLINE __attribute__((always_inline)) static inline
#elif defined(__clang__)
#define OS_ALWAYS_INLINE __attribute__((always_inline)) static inline
#elif defined(__GNUC__)
#define OS_ALWAYS_INLINE __attribute__((always_inline)) static inline
#else
#define OS_ALWAYS_INLINE static inline
#endif
#endif

/*
 * Architecture capabilities derived from the compiler target: TrustZone (the
 * ARMv8-M Security Extension) and exclusive load/store (LDREX/STREX, absent
 * on ARMv6-M).
 */
#if defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
#define OS_ARCH_HAS_TRUSTZONE             1
#else
#define OS_ARCH_HAS_TRUSTZONE             0
#endif

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || (OS_ARCH_HAS_TRUSTZONE == 1)
#define OS_ARCH_HAS_EXCLUSIVES            1
#else
#define OS_ARCH_HAS_EXCLUSIVES            0
#endif

/* BASEPRI exists on ARMv7-M / ARMv7E-M / ARMv8-M mainline / ARMv8.1-M only;
 * ARMv6-M and ARMv8-M baseline (Cortex-M0/M0+/M23) have PRIMASK alone. */
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
#define OS_ARCH_HAS_BASEPRI               1
#else
#define OS_ARCH_HAS_BASEPRI               0
#endif

/*
 * Kernel interrupt-mask backend selected by OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY:
 *   0        PRIMASK: critical sections mask every interrupt (all cores).
 *   nonzero  BASEPRI: critical sections mask only interrupts whose NVIC
 *            priority byte is numerically >= the value; interrupts above it
 *            (numerically lower) keep zero kernel latency but MUST NOT call
 *            any kernel API. Requires BASEPRI (see OS_ARCH_HAS_BASEPRI).
 */
#if (OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY != 0U) && (OS_ARCH_HAS_BASEPRI == 0)
#error "OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY requires BASEPRI (ARMv7-M/ARMv7E-M/ARMv8-M mainline); set it to 0 on Cortex-M0/M0+/M23."
#endif

#if (OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY > 255U)
#error "OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY is an 8-bit NVIC priority byte (0..255, pre-shifted into the implemented bits)."
#endif

/* Validate the configured TrustZone mode against the target early, with
 * readable errors instead of a mis-built kernel. __ARM_FEATURE_CMSE is 3 when
 * compiling secure code (-mcmse) and 1 for plain v8-M builds. */
#if (OS_CONFIG_TRUSTZONE != OS_CONFIG_TRUSTZONE_DISABLED) && (OS_ARCH_HAS_TRUSTZONE == 0)
#error "OS_CONFIG_TRUSTZONE requires an ARMv8-M core (Cortex-M23/M33/M35P/M52/M55/M85)."
#endif

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_SECURE) && (!defined(__ARM_FEATURE_CMSE) || (__ARM_FEATURE_CMSE < 3))
#error "OS_CONFIG_TRUSTZONE_SECURE: compile the kernel as secure code (-mcmse)."
#endif

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE) && defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE >= 3)
#error "OS_CONFIG_TRUSTZONE_NON_SECURE: do not compile the kernel with -mcmse."
#endif

/* Stringify, used to paste OS_CONFIG_ARCH_PENDSV_HANDLER into the inline
 * assembly that defines the handler. Two levels: the inner one would stringify
 * the macro's NAME instead of its expansion. */
#define OS_ARCH_STRINGIFY_(text)          #text
#define OS_ARCH_STRINGIFY(text)           OS_ARCH_STRINGIFY_(text)

#define OS_ARCH_REG_ICSR                  (*(__IO uint32_t *)0xE000ED04UL)
#define OS_ARCH_ICSR_PENDSVSET_MSK        (1UL << 28)

/* Vector Table Offset Register. Writable on ARMv7-M/ARMv8-M and on most ARMv6-M
 * implementations; where it is not implemented it reads as zero, which is also
 * the fixed table address, so reading it locates the table on every core. */
#define OS_ARCH_REG_VTOR                  (*(__I uint32_t *)0xE000ED08UL)

/* Exception numbers, which - unlike the handler NAMES - are architectural. */
#define OS_ARCH_VECTOR_PENDSV             14U

/*
 * SysTick. Defined here rather than per port because the block is identical on
 * ARMv6-M, ARMv7-M and ARMv8-M, and all three ports need it: as the tick source
 * and, where DWT is unavailable, as the cycle counter behind
 * os_arch_cycle_systick_get() (os_arch_cycle_systick.c).
 */
#define OS_ARCH_REG_SYST_CSR              (*(__IO uint32_t *)0xE000E010UL)
#define OS_ARCH_REG_SYST_RVR              (*(__IO uint32_t *)0xE000E014UL)
#define OS_ARCH_REG_SYST_CVR              (*(__IO uint32_t *)0xE000E018UL)

#define OS_ARCH_SYST_CSR_ENABLE_MSK       (1UL << 0)
#define OS_ARCH_SYST_CSR_TICKINT_MSK      (1UL << 1)
#define OS_ARCH_SYST_CSR_CLKSOURCE_MSK    (1UL << 2)
#define OS_ARCH_SYST_CSR_COUNTFLAG_MSK    (1UL << 16)
#define OS_ARCH_SYST_RVR_RELOAD_MSK       0x00FFFFFFUL

/* Byte-addressable priority register banks, indexed rather than read as whole
 * words (os_arch_isr_priority_check reads one exception's priority byte):
 *   NVIC_IPR   one byte per external interrupt, indexed by IRQ number
 *              (IPSR value - OS_ARCH_IPSR_IRQ_BASE).
 *   SHPR       one byte per configurable-priority system handler, indexed by
 *              (IPSR value - OS_ARCH_IPSR_SYSHANDLER_BASE); the bank starts at
 *              SHPR1, whose first byte is exception number 4 (MemManage). */
#define OS_ARCH_REG_NVIC_IPR_BASE         ((__I uint8_t *)0xE000E400UL)
#define OS_ARCH_REG_SHPR_BASE             ((__I uint8_t *)0xE000ED18UL)

/* IPSR exception-number boundaries: 0 = thread mode, 1..15 = system
 * exceptions, 16+ = external interrupts (IRQ n is IPSR 16 + n). */
#define OS_ARCH_IPSR_THREAD_MODE          0U
#define OS_ARCH_IPSR_SYSHANDLER_BASE      4U   /* MemManage: first byte of the SHPR bank */
#define OS_ARCH_IPSR_IRQ_BASE             16U

#define OS_ARCH_STACK_ALIGNMENT_BYTES     4U
#define OS_ARCH_DSB()                     __asm volatile("dsb 0xF" ::: "memory")
#define OS_ARCH_ISB()                     __asm volatile("isb 0xF" ::: "memory")

/*
 * The System Control Space is banked per core, so this pends PendSV on the
 * calling core - which is correct now that every scheduling core runs its
 * own PendSV. When a task can only run on another core, the scheduler
 * routes the request through the SoC IPI callback instead (os_task.c,
 * os_task_preempt_request).
 */
#define OS_ARCH_CONTEXT_SWITCH_REQUEST()  do { OS_ARCH_REG_ICSR = OS_ARCH_ICSR_PENDSVSET_MSK; OS_ARCH_DSB(); OS_ARCH_ISB(); } while (0)

/*
 * CPS writes to PRIMASK are self-synchronizing on ARMv6-M/v7-M/v8-M: masking
 * is guaranteed to take effect before the next instruction executes (ARM
 * AN321 sec 4.2; only a CONTROL register change needs an ISB). The "memory"
 * clobber is a compiler barrier only (stops instruction reordering across
 * the mask change) and costs nothing at runtime, unlike DSB/ISB, which this
 * pair used to pay on every os_critical_enter/exit, every PendSV, and three
 * times per tick for no architectural reason. Kept out of the BASEPRI mask
 * path (os_arch_kernel_mask_save/restore, os_arch_isr_priority_check),
 * which retains its own DSB/ISB - raising/lowering execution priority via
 * BASEPRI has its own barrier requirements and (on Cortex-M7) an errata
 * history that favors the conservative sequence.
 */
#define OS_ARCH_IRQ_DISABLE()             do { __asm volatile("cpsid i" ::: "memory"); } while (0)
#define OS_ARCH_IRQ_ENABLE()              do { __asm volatile("cpsie i" ::: "memory"); } while (0)
#define OS_ARCH_IDLE()                    do { __asm volatile("wfi"); } while (0)
#define OS_ARCH_SLEEP(ticks)              do { os_arch_sleep_prepare((ticks)); OS_ARCH_DSB(); __asm volatile("wfi"); OS_ARCH_ISB(); } while (0)

/*
 * ***********************************************************************************************************
 * Inline helpers
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Read the PRIMASK register (1 when interrupts are masked).
 */
static inline uint32_t os_arch_primask_get(void)
{
    uint32_t primask;

    __asm volatile("mrs %0, primask" : "=r"(primask));

    return primask;
}

/******************************************************************************************************/
/**
 * @brief Return true when executing in interrupt (handler) context.
 */
static inline bool os_arch_in_isr(void)
{
    uint32_t ipsr;

    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));

    return (ipsr != 0U);
}

/*
 * ***********************************************************************************************************
 * Kernel interrupt mask
 * ***********************************************************************************************************
 *
 * Every kernel critical section and ISR-safe walk masks interrupts through
 * this pair. With OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY == 0 it is the
 * classic PRIMASK mask (everything). With a nonzero value it raises BASEPRI
 * instead, so interrupts of numerically lower (more urgent) priority stay
 * enabled with zero kernel-induced latency - in exchange they must never
 * call a kernel API (os_arch_isr_priority_check traps violations).
*/

/******************************************************************************************************/
/**
 * @brief Raise the kernel interrupt mask; returns the previous mask state for restore.
 */
static inline uint32_t os_arch_kernel_mask_save(void)
{
#if (OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY != 0U)
    uint32_t previous;

    __asm volatile("mrs %0, basepri" : "=r"(previous));

    /* basepri_max only ever tightens the mask: a nested save can never
     * accidentally lower a threshold already raised by an outer level. */
    __asm volatile("msr basepri_max, %0" :: "r"((uint32_t)OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY) : "memory");
    OS_ARCH_DSB();
    OS_ARCH_ISB();

    return previous;
#else
    uint32_t previous = os_arch_primask_get();

    OS_ARCH_IRQ_DISABLE();

    return previous;
#endif
}

/******************************************************************************************************/
/**
 * @brief Restore the kernel interrupt mask to a state returned by os_arch_kernel_mask_save.
 */
static inline void os_arch_kernel_mask_restore(uint32_t saved_state)
{
#if (OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY != 0U)
    __asm volatile("msr basepri, %0" :: "r"(saved_state) : "memory");
    OS_ARCH_ISB();
#else
    if (saved_state == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }
#endif
}

/******************************************************************************************************/
/**
 * @brief Return nonzero while the kernel interrupt mask is raised (diagnostics/self-test).
 */
static inline uint32_t os_arch_kernel_mask_active(void)
{
#if (OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY != 0U)
    uint32_t basepri;

    __asm volatile("mrs %0, basepri" : "=r"(basepri));

    return basepri;
#else
    return os_arch_primask_get();
#endif
}

/******************************************************************************************************/
/**
 * @brief Trap for unrecoverable configuration faults detected at runtime; parks the core
 *        with all interrupts masked so a debugger lands right at the cause.
 */
static inline void os_arch_config_fault_trap(void)
{
    OS_ARCH_IRQ_DISABLE();

    while (1)
    {
    }
}

/******************************************************************************************************/
/**
 * @brief Verify that the live vector table routes PendSV to the kernel's handler, and park in
 *        os_arch_config_fault_trap() if it does not. Called from os_arch_init() on every core.
 *
 * PendSV is the only vector the kernel must own, and owning it is not something the linker can
 * confirm: if another definition of the configured handler name wins (a vendor IDE's generated
 * interrupt file is the usual one), or a bootloader relocated the table and repopulated it from
 * its own image, the build still succeeds and the symptom is a board that reaches os_start() and
 * then simply stops - no fault, no output, nothing to attach a debugger to. Comparing the table
 * against the address the port actually assembled turns that into a halt at the cause, at boot.
 *
 * Compiles to nothing when OS_CONFIG_ARCH_VECTOR_CHECK is 0.
 *
 * @param[in] pendsv_handler  Address the port's PendSV handler assembled to.
 * @return None.
 */
static inline void os_arch_vector_check(void (*pendsv_handler)(void))
{
#if (OS_CONFIG_ARCH_VECTOR_CHECK != 0U)
    const uint32_t *vector_table = (const uint32_t *)(uintptr_t)OS_ARCH_REG_VTOR;
    uint32_t        installed;
    uint32_t        expected;

    /* Bit 0 of a vector entry is the Thumb bit and bit 0 of a function pointer
     * is the same flag, so both sides normally carry it - masking it off makes
     * the comparison independent of how either was produced. */
    installed = vector_table[OS_ARCH_VECTOR_PENDSV] & ~(uint32_t)1U;
    expected  = (uint32_t)(uintptr_t)pendsv_handler & ~(uint32_t)1U;

    if (installed != expected)
    {
        os_arch_config_fault_trap();
    }
#else
    (void)pendsv_handler;
#endif
}

/******************************************************************************************************/
/**
 * @brief In BASEPRI mode, trap a kernel API call from an interrupt the kernel mask cannot
 *        reach (NVIC priority numerically below OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY):
 *        such a call could corrupt kernel state, so it parks in os_arch_config_fault_trap.
 *        Compiles to nothing in PRIMASK mode, where every interrupt is maskable.
 */
static inline void os_arch_isr_priority_check(void)
{
#if (OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY != 0U)
    uint32_t ipsr;
    uint32_t priority;

    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));

    if (ipsr == OS_ARCH_IPSR_THREAD_MODE)
    {
        return; /* task context */
    }

    if (ipsr >= OS_ARCH_IPSR_IRQ_BASE)
    {
        /* External interrupt: priority byte in NVIC_IPR. */
        priority = (uint32_t)OS_ARCH_REG_NVIC_IPR_BASE[ipsr - OS_ARCH_IPSR_IRQ_BASE];
    }
    else if (ipsr >= OS_ARCH_IPSR_SYSHANDLER_BASE)
    {
        /* Configurable-priority system handler (MemManage, BusFault,
         * UsageFault, SVCall, SecureFault, DebugMonitor): priority byte in
         * SHPR1-SHPR3. These reset to 0 = above any threshold, so an
         * application-enabled fault handler calling kernel APIs is caught
         * here. SVCall (11) gets no exemption: the kernel no longer uses SVC
         * for anything, so an SVC handler is application code like any other
         * and is held to the same rule. PendSV (14) and SysTick (15) sit at
         * the lowest priority and pass the comparison anyway. */
        priority = (uint32_t)OS_ARCH_REG_SHPR_BASE[ipsr - OS_ARCH_IPSR_SYSHANDLER_BASE];
    }
    else
    {
        /* NMI and HardFault execute above every configurable priority:
         * no mask backend can defer them, so a kernel API call from them
         * is never safe - trap unconditionally. */
        os_arch_config_fault_trap();
        return;
    }

    /* Raw-byte comparison is exact because os_arch_init rejects thresholds
     * with unimplemented bits and priority groupings with subpriority bits
     * (both would make this differ from the hardware's masking decision). */
    if (priority < (uint32_t)OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY)
    {
        os_arch_config_fault_trap();
    }
#endif
}

/******************************************************************************************************/
/**
 * @brief Index of the highest set bit in a non-zero bitmap (the scheduler's ready-priority pick).
 *        One CLZ instruction on ARMv7-M and up; ARMv6-M has no CLZ, so GCC emits its small
 *        library routine there - still cheaper than scanning the task table.
 */
static inline uint32_t os_arch_highest_bit_get(uint32_t bitmap)
{
    return 31U - (uint32_t)__builtin_clz(bitmap);
}

/******************************************************************************************************/
/**
 * @brief Index of the lowest set bit in a non-zero bitmap (picks the IPI target from an
 *        affinity mask).
 */
static inline uint32_t os_arch_lowest_bit_get(uint32_t bitmap)
{
    return (uint32_t)__builtin_ctz(bitmap);
}

/*
 * ***********************************************************************************************************
 * CPU clock
 * ***********************************************************************************************************
*/

/* The CPU clock in Hz, as maintained by the device's own startup code. On CMSIS platforms
 * SystemInit() sets it and SystemCoreClockUpdate() refreshes it after every clock-tree change,
 * so reading the live variable is always correct - including on a board that boots on an
 * internal oscillator and only later switches to a PLL.
 *
 * The kernel deliberately does NOT mirror this into a build-time constant: a constant cannot
 * follow a runtime clock switch, and a stale one would silently mis-program the SysTick reload
 * and every busy-wait delay.
 *
 * Devices whose startup code does not provide the CMSIS symbol simply define it themselves,
 * which is all the kernel needs:
 *
 *     uint32_t SystemCoreClock = 120000000U;   // and update it if the clock tree changes
 */
extern uint32_t SystemCoreClock;

/******************************************************************************************************/
/**
 * @brief Current CPU clock in Hz. Lives in the arch layer because SystemCoreClock is an
 *        ARM/CMSIS convention, not a portable one.
 *
 * @return uint32_t  CPU clock frequency in Hz.
 */
static inline uint32_t os_arch_clock_hz_get(void)
{
    return SystemCoreClock;
}

/*
 * ***********************************************************************************************************
 * Public function prototypes
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize architecture-specific low-level resources.
 */
void os_arch_init(void);

/******************************************************************************************************/
/**
 * @brief Start the first task context. Does not return.
 */
void os_arch_start_first_task(void);

/******************************************************************************************************/
/**
 * @brief Start the kernel tick at OS_CONFIG_TICK_HZ.
 *
 * With OS_CONFIG_TICK_SOURCE_SYSTICK this programs SysTick from the live CPU clock. With
 * OS_CONFIG_TICK_SOURCE_EXTERNAL it touches no hardware and calls os_arch_tick_init_cb()
 * instead, leaving the tick entirely to the application.
 */
void os_arch_tick_init(void);

/******************************************************************************************************/
/**
 * @brief Build the initial stack frame for a newly created task.
 */
uint32_t* os_arch_task_stack_initialize(uint8_t *stack_base, size_t stack_bytes, void (*entry)(void *context), void *context);

/******************************************************************************************************/
/**
 * @brief Read the free-running core cycle counter (DWT, or SysTick-derived when absent).
 */
uint32_t os_arch_cycle_count_get(void);

/******************************************************************************************************/
/**
 * @brief Return elapsed ticks while in low-power mode.
 */
uint32_t os_arch_elapsed_ticks_get(void);

/*
 * ***********************************************************************************************************
 * Atomics
 * ***********************************************************************************************************
 *
 * The complete set the portable os_atomic_* API rests on, rather than one primitive it composes
 * from, because how a word updates indivisibly is a property of the core. Two backends, chosen by
 * OS_ARCH_ATOMIC_LOCK_FREE below:
 *
 *   LDREX/STREX        One retry loop per operation. Lock-free: nothing is masked, so an ISR - or
 *                      another core - landing in the middle costs a second pass rather than costing
 *                      anyone correctness.
 *   Critical section   For cores that cannot express those loops. Costs the length of the update in
 *                      interrupt latency, and needs no retry, since nothing can interfere.
 *
 * Every read-modify-write below returns the value the word held BEFORE the operation, takes a
 * pointer to a naturally aligned 32-bit word, and is safe from tasks and from ISRs.
 *
 * ALL OF THEM ARE INLINE, and deliberately so. Each is called from exactly one place - the matching
 * one-line wrapper in os_atomic.c, which validates its argument and forwards - so leaving them in
 * the port translation unit meant every atomic operation paid a real call and stack frame to reach
 * five instructions. os_atomic.c already refuses to layer its own operations on each other for that
 * reason (increment calls the port's add directly, not os_atomic_add); defining these here finishes
 * the job by removing the last cross-unit call. It costs nothing in code size, because a body this
 * small is smaller than the call sequence that used to reach it.
*/

/* Kernel services these fall back on where there are no usable exclusives. Declared here because
 * the port translation unit does not include ahura.h - it needs these two and nothing else from
 * the core. */
void os_critical_enter(void);
void os_critical_exit(void);

/*
 * Whether the LDREX/STREX retry loops below can be written for this core.
 *
 * NOT the same question as OS_ARCH_HAS_EXCLUSIVES. ARMv8-M baseline (Cortex-M23) has the exclusive
 * instructions, but runs the Thumb-1 subset, where the three-operand data-processing forms these
 * loops use ("add %1, %0, %4") have no encoding. So baseline takes the critical-section backend
 * despite having the hardware for the other one - the same conclusion the ports reached before
 * these moved into this header, and the same one their context-switch code reaches when it shares
 * os_arch_port_v6m.c. Rewriting the loops in the flag-setting Thumb-1 forms would lift that, at the
 * cost of constraining register allocation on every core.
 */
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) ||           \
    defined(__ARM_ARCH_8_1M_MAIN__)
#define OS_ARCH_ATOMIC_LOCK_FREE          1
#else
#define OS_ARCH_ATOMIC_LOCK_FREE          0
#endif

/******************************************************************************************************/
/**
 * @brief Read a word indivisibly.
 *
 * The one operation that is identical on every ARM core and needs no backend split: a single
 * naturally aligned 32-bit load is already indivisible everywhere this port runs, so the whole
 * operation is one LDR. What the volatile access adds over reading the variable directly is that
 * the compiler may not reuse a value it cached before some other code path changed the word.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_load(const __IO int32_t *target)
{
    return *target;
}

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

#if (OS_ARCH_ATOMIC_LOCK_FREE == 1)

/*
 * Lock-free backend. Each operation is a single inline-assembly block: take a reservation on the
 * word, compute from what it held, store only if the reservation survived, and go round again if it
 * did not.
 *
 * Written out per operation rather than funnelled through a shared helper or a CAS loop. Speed: the
 * whole sequence is five instructions, and CAS would re-load and compare what the reservation
 * already tells us. Safety: one asm block per loop is the same five instructions at -O0 as at -O2,
 * with no compiler spill between the LDREX and the STREX - ARM allows only one reservation per core
 * and leaves it implementation-defined whether other accesses clear it.
 *
 * "1:" and "1b" are local numeric labels, so each block stays correct even if the compiler emits it
 * more than once - which, now that these are inline, it routinely does.
 */

/******************************************************************************************************/
/**
 * @brief Store a word indivisibly, returning what it held before.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_exchange(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%2]      \n"
        "    strex   %1, %3, [%2]  \n"
        "    cmp     %1, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Add, returning the value held before the operation.
 *
 * ADD and SUB wrap rather than overflow: the assembler works on raw 32-bit registers, so the
 * signed-overflow undefined behaviour that the equivalent C expression would carry never arises.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_add(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    add     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Subtract, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_sub(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    sub     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Bitwise OR, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_or(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    orr     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Bitwise AND, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_and(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    and     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Bitwise XOR, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_xor(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    eor     %1, %0, %4    \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Bitwise NAND, returning the value held before the operation.
 *
 * The only operation needing two instructions inside the window: ARM has no single NAND, so the
 * AND result is inverted in place before the store.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_nand(__IO int32_t *target, int32_t value)
{
    int32_t  current;
    int32_t  updated;
    uint32_t store_failed;

    __asm volatile(
        "1:  ldrex   %0, [%3]      \n"
        "    and     %1, %0, %4    \n"
        "    mvn     %1, %1        \n"
        "    strex   %2, %1, [%3]  \n"
        "    cmp     %2, #0        \n"
        "    bne     1b            \n"
        : "=&r"(current), "=&r"(updated), "=&r"(store_failed)
        : "r"(target), "r"(value)
        : "cc", "memory");

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic compare-and-swap: if *target still holds expected, store desired and report true.
 *
 * The only operation here that does not retry internally, which is what makes it the building
 * block for lock-free algorithms the kernel knows nothing about - and so the only one that needs
 * CLREX: on a mismatch the reservation is dropped rather than left set on a word this call is
 * walking away from, since a stale one can make an unrelated later STREX succeed when it should
 * not. store_failed is seeded with 1 so the mismatch path falls out reporting failure without the
 * STREX having run.
 *
 * A false return means only that the swap did not happen - either the value had changed, or the
 * reservation was lost to an interrupt or another core. That spurious case is part of the
 * contract, which is why callers needing certainty re-read the word instead of reading false as
 * "someone else won", and callers that only care about the final state loop.
 */
OS_ALWAYS_INLINE bool os_arch_atomic_cas(__IO int32_t *target, int32_t expected, int32_t desired)
{
    int32_t  current;
    uint32_t store_failed;

    __asm volatile(
        "    mov     %1, #1        \n"
        "    ldrex   %0, [%2]      \n"
        "    cmp     %0, %3        \n"
        "    bne     1f            \n"
        "    strex   %1, %4, [%2]  \n"
        "    b       2f            \n"
        "1:  clrex                 \n"
        "2:                        \n"
        : "=&r"(current), "=&r"(store_failed)
        : "r"(target), "r"(expected), "r"(desired)
        : "cc", "memory");

    return (store_failed == 0U);
}

#else /* OS_ARCH_ATOMIC_LOCK_FREE == 0 */

/*
 * Critical-section backend. Interference cannot be DETECTED here, so it has to be PREVENTED: each
 * operation runs inside os_critical_enter/exit, which costs the length of the update in interrupt
 * latency and, on multi-core, can wait on unrelated kernel work holding the same lock. Reusing the
 * kernel's critical section rather than a second private lock is deliberate - two locks over the
 * same data is how lock-ordering bugs start. No retry loop is needed, since nothing can interfere
 * while the section is held.
 *
 * ADD and SUB compute in the unsigned domain and convert back: signed overflow is undefined
 * behaviour, and unsigned wrapping reproduces the same two's-complement pattern anyway.
 */

/******************************************************************************************************/
/**
 * @brief Store a word indivisibly, returning what it held before.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_exchange(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = value;

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Add, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_add(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = (int32_t)((uint32_t)current + (uint32_t)value);

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Subtract, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_sub(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = (int32_t)((uint32_t)current - (uint32_t)value);

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Bitwise OR, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_or(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = current | value;

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Bitwise AND, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_and(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = current & value;

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Bitwise XOR, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_xor(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = current ^ value;

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Bitwise NAND, returning the value held before the operation.
 */
OS_ALWAYS_INLINE int32_t os_arch_atomic_nand(__IO int32_t *target, int32_t value)
{
    int32_t current;

    os_critical_enter();

    current = *target;
    *target = ~(current & value);

    os_critical_exit();

    return current;
}

/******************************************************************************************************/
/**
 * @brief Atomic compare-and-swap: if *target still holds expected, store desired and report true.
 *
 * Never fails spuriously on this backend - nothing could have interfered - but callers still loop,
 * because the portable contract is written to the weaker guarantee the exclusives backend gives.
 */
OS_ALWAYS_INLINE bool os_arch_atomic_cas(__IO int32_t *target, int32_t expected, int32_t desired)
{
    bool swapped = false;

    os_critical_enter();

    if (*target == expected)
    {
        *target = desired;
        swapped = true;
    }

    os_critical_exit();

    return swapped;
}

#endif /* OS_ARCH_ATOMIC_LOCK_FREE */

#endif /* OS_CONFIG_ATOMIC_ENABLE */

/******************************************************************************************************/
/**
 * @brief Record low-power entry context for elapsed tick accounting.
 */
void os_arch_sleep_prepare(uint32_t planned_ticks);

/******************************************************************************************************/
/**
 * @brief Close the tickless window opened by os_arch_sleep_prepare, releasing any interrupt mask
 *        it took. Idempotent, and safe when the port never armed a window.
 *
 * Split out of os_arch_elapsed_ticks_get so the kernel can finish accounting for the sleep BEFORE
 * interrupts come back: between measuring the sleep and calling os_tick_announce, os_tick_count is
 * still missing the whole sleep duration, and any tick or timer processing that ran in that gap
 * would decide against a clock several hundred ticks behind reality.
 */
void os_arch_sleep_finish(void);

/******************************************************************************************************/
/**
 * @brief Maximum ticks this port can suppress in a single tickless window, given the current
 *        tick rate and CPU clock (register-width limited - e.g. SysTick's 24-bit reload). 0 if
 *        this port does not yet suppress ticking for real (falls back to a plain WFI - see the
 *        kernel README "Tickless idle" for which ports currently do) or the clock/tick source
 *        isn't ready. Portable callers (os_tick.c, tests) use this instead of assuming any fixed
 *        tick count, since it varies with both the platform clock and OS_CONFIG_TICK_HZ.
 */
uint32_t os_arch_max_suppressed_ticks_get(void);

/*
 * ***********************************************************************************************************
 * CONFIGURABLE - External tick source (OS_CONFIG_TICK_SOURCE_EXTERNAL only)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TICK_SOURCE == OS_CONFIG_TICK_SOURCE_EXTERNAL)
/******************************************************************************************************/
/**
 * @brief Application callback: start the timer that will drive the kernel tick, and arrange for its
 *        ISR to call os_tick_handler() OS_CONFIG_TICK_HZ times per second.
 *
 * Called once from os_tick_init(), at the end of os_init(), so the clock tree is already configured.
 *
 * REQUIRED in this mode: the kernel ships no default, so a missing implementation is a link error
 * rather than a kernel whose clock never advances - which would look like every delay, timeout and
 * timer hanging forever, with nothing pointing at the tick as the cause.
 *
 * Give the tick interrupt the LOWEST priority the device offers, matching what the port does for
 * SysTick. Anything else lets a tick preempt application interrupts. The handler must also be
 * reachable by the kernel's interrupt mask: with a nonzero OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY,
 * a tick ISR above the threshold is trapped by os_arch_isr_priority_check the moment it calls in.
 */
void os_arch_tick_init_cb(void);
#endif /* OS_CONFIG_TICK_SOURCE_EXTERNAL */

/*
 * ***********************************************************************************************************
 * CONFIGURABLE - Multi-core primitives (OS_CONFIG_CORE_COUNT)
 * ***********************************************************************************************************
 *
 * Cortex-M has no architectural core-id register and no architectural IPI, so
 * on multi-core builds the SoC layer supplies both through _cb callbacks
 * (e.g. RP2040: SIO CPUID and the inter-core FIFO/doorbell). The spinlock is
 * implemented with LDREX/STREX where the ISA provides them; ARMv6-M SoCs must
 * route it to their hardware spinlocks instead.
 *
 * The type and the two inline entry points stay unguarded so os_critical.c can
 * call them unconditionally - they compile to nothing at OS_CONFIG_CORE_COUNT 1.
*/

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief SoC callback: return the index of the calling core (0-based). No default is provided:
 *        an id fixed at 0 would make every core believe it is core 0.
 */
uint32_t os_arch_core_id_get_cb(void);

/******************************************************************************************************/
/**
 * @brief SoC callback: interrupt another core so it re-evaluates scheduling. No default is
 *        provided: a do-nothing IPI leaves every cross-core wakeup waiting for the next tick.
 */
void os_arch_core_ipi_request_cb(uint32_t core_id);
#endif /* OS_CONFIG_CORE_COUNT > 1U */

typedef struct
{
    __IO uint32_t locked;

} os_arch_spinlock_t;

#define OS_ARCH_SPINLOCK_INIT  { 0U }

/******************************************************************************************************/
/**
 * @brief Index of the calling core; always 0 on single-core builds.
 */
static inline uint32_t os_arch_core_id_get(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    return os_arch_core_id_get_cb();
#else
    return 0U;
#endif
}

/*
 * The SoC-callback spinlock backend is used when the core lacks LDREX/STREX
 * (mandatory - ARMv6-M) or when OS_CONFIG_SPINLOCK_SOC_BACKEND opts
 * out of the built-in LDREX/STREX backend: a core WITH exclusives can still
 * need this when its interconnect implements no GLOBAL exclusive monitor, or
 * the spinlock's memory is not Shareable-mapped - STREX then only excludes
 * within one core and the built-in backend would silently stop locking
 * across cores. See the OS_CONFIG_CORE_COUNT precondition notes in
 * template/os_config.h.
 */
#define OS_ARCH_SPINLOCK_USE_CB  ((OS_ARCH_HAS_EXCLUSIVES == 0) || (OS_CONFIG_SPINLOCK_SOC_BACKEND != 0))

#if (OS_CONFIG_CORE_COUNT > 1U) && (OS_ARCH_SPINLOCK_USE_CB)
/******************************************************************************************************/
/**
 * @brief SoC callbacks backing the kernel spinlock: mandatory on cores without LDREX/STREX
 *        (ARMv6-M multi-core SoCs, e.g. hardware SIO spinlocks on the RP2040), optional
 *        elsewhere via OS_CONFIG_SPINLOCK_SOC_BACKEND. No default is provided
 *        on purpose: a missing implementation must fail at link time rather than silently
 *        not lock.
 */
void os_arch_spinlock_acquire_cb(os_arch_spinlock_t *lock);
void os_arch_spinlock_release_cb(os_arch_spinlock_t *lock);
#endif

/******************************************************************************************************/
/**
 * @brief Acquire an inter-core spinlock (busy-waits; call with interrupts disabled).
 *        Compiles to nothing on single-core builds.
 */
static inline void os_arch_spinlock_acquire(os_arch_spinlock_t *lock)
{
#if (OS_CONFIG_CORE_COUNT == 1U)
    (void)lock;
#elif (OS_ARCH_SPINLOCK_USE_CB)
    os_arch_spinlock_acquire_cb(lock);
#else
    uint32_t fail;

    do
    {
        uint32_t current;

        do
        {
            __asm volatile("ldrex %0, [%1]" : "=r"(current) : "r"(&lock->locked) : "memory");
        } while (current != 0U);

        __asm volatile("strex %0, %1, [%2]" : "=&r"(fail) : "r"(1U), "r"(&lock->locked) : "memory");
    } while (fail != 0U);

    OS_ARCH_DSB();
#endif
}

/******************************************************************************************************/
/**
 * @brief Release an inter-core spinlock. Compiles to nothing on single-core builds.
 */
static inline void os_arch_spinlock_release(os_arch_spinlock_t *lock)
{
#if (OS_CONFIG_CORE_COUNT == 1U)
    (void)lock;
#elif (OS_ARCH_SPINLOCK_USE_CB)
    os_arch_spinlock_release_cb(lock);
#else
    OS_ARCH_DSB();
    lock->locked = 0U;
    OS_ARCH_DSB();
#endif
}

/*
 * ***********************************************************************************************************
 * CONFIGURABLE - TrustZone callbacks (OS_CONFIG_TRUSTZONE_NON_SECURE only)
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
/******************************************************************************************************/
/**
 * @brief Application callback: bank the secure-side context of the task being switched out.
 *        Called from the context-switch handler; task_id 0 means the idle task (no secure
 *        context). REQUIRED in this mode: the kernel ships no default, so a missing one is a
 *        link error rather than a task switched without its secure state.
 */
void os_arch_tz_context_save_cb(uint32_t task_id);

/******************************************************************************************************/
/**
 * @brief Application callback: restore the secure-side context of the task being switched in.
 *        REQUIRED in this mode, like its save counterpart above.
 */
void os_arch_tz_context_restore_cb(uint32_t task_id);
#endif /* OS_CONFIG_TRUSTZONE_NON_SECURE */

#ifdef __cplusplus
}
#endif

#endif /* OS_ARCH_PORT_COMMON_H */
