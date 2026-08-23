/**
 * @file os_arch_port_common.h
 * @brief Architecture port interface shared by every RISC-V core folder.
 *
 * The RV32 counterpart of arch/arm/common/os_arch_port_common.h. Everything the kernel core needs
 * from a CPU is declared here; nothing above this file knows the instruction set, and nothing in
 * this file knows the chip.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_ARCH_PORT_COMMON_H
#define OS_ARCH_PORT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Kernel-owned encodings for two os_config.h options. They live in the PORT header on both
 * architectures, which means every port owes them - and a port that forgets is not caught by the
 * completeness check above, because that only tests whether os_config.h defines the OPTION.
 *
 * Leaving them out is silently wrong rather than loudly wrong, which is why they are worth this
 * comment. An undefined identifier evaluates to 0 inside #if, so with these missing a config
 * saying OS_CONFIG_TRUSTZONE_DISABLED compares EQUAL to OS_CONFIG_TRUSTZONE_SECURE - both sides
 * being 0 - and the kernel builds TrustZone paths on a core that has no such thing. The same trap
 * applies to the tick source. */
#define OS_CONFIG_TRUSTZONE_DISABLED    0U
#define OS_CONFIG_TRUSTZONE_NON_SECURE  1U
#define OS_CONFIG_TRUSTZONE_SECURE      2U

#define OS_CONFIG_TICK_SOURCE_SYSTICK   0U
#define OS_CONFIG_TICK_SOURCE_EXTERNAL  1U

/* TrustZone mode.
 *
 * REQUIRED by the ARM port and deliberately NOT required here. Every other option in the list
 * above is mandatory because a missing one would read as 0 in an #if and silently change
 * behaviour; this one cannot, because DISABLED is the only answer a RISC-V core can give. Making
 * an application state it anyway would be asking it to describe a feature its CPU does not have.
 *
 * It keeps its home in os_config.h rather than moving to a SoC package, because the mode is a
 * PRODUCT decision - two boards built on one STM32 may legitimately differ - and doc/soc.md draws
 * the line exactly there: what the core HAS is arch (OS_ARCH_HAS_TRUSTZONE), which state the
 * kernel runs in is os_config.h. A config that still carries it keeps building; one that has
 * dropped it gets DISABLED, and either way the #error below rejects an actual request. */
#ifndef OS_CONFIG_TRUSTZONE
#define OS_CONFIG_TRUSTZONE            OS_CONFIG_TRUSTZONE_DISABLED
#endif

/* Reject incomplete configurations: a missing option would otherwise read
 * as 0 in #if directives and silently disable or misconfigure features.
 * Start from template/os_config.h, which lists every required option. */
#if !defined(OS_CONFIG_MUTEX_ENABLE) || !defined(OS_CONFIG_SEMAPHORE_ENABLE) ||                       \
    !defined(OS_CONFIG_QUEUE_ENABLE) || !defined(OS_CONFIG_EVENT_ENABLE) ||                           \
    !defined(OS_CONFIG_TIMER_ENABLE) || !defined(OS_CONFIG_ALLOC_ENABLE) ||                           \
    !defined(OS_CONFIG_ATOMIC_ENABLE) || !defined(OS_CONFIG_NOTIFY_ENABLE) ||                         \
    !defined(OS_CONFIG_LOG_ENABLE) || !defined(OS_CONFIG_ASSERT_ENABLE) ||                            \
    !defined(OS_CONFIG_STACK_WATERMARK_ENABLE) || !defined(OS_CONFIG_STACK_CHECK_ENABLE) ||           \
    !defined(OS_CONFIG_CPU_USAGE_ENABLE) || !defined(OS_CONFIG_TEST_ENABLE) ||                        \
    !defined(OS_CONFIG_TICKLESS_ENABLE) ||                                                            \
    !defined(OS_CONFIG_LOG_LEVEL) || !defined(OS_CONFIG_LOG_BUFFER_SIZE) ||                           \
    !defined(OS_CONFIG_LOG_LINE_MAX) || !defined(OS_CONFIG_LOG_TASK_STACK_SIZE) ||                    \
    !defined(OS_CONFIG_LOG_TASK_PRIORITY) || !defined(OS_CONFIG_LOG_CORE_AFFINITY) ||                                                          \
    !defined(OS_CONFIG_TICK_HZ) || !defined(OS_CONFIG_TIME_SLICE_TICKS) ||                            \
    !defined(OS_CONFIG_MAX_USER_TASKS) || !defined(OS_CONFIG_MIN_STACK_SIZE) ||                       \
    !defined(OS_CONFIG_HEAP_SIZE) ||                                                                  \
    !defined(OS_CONFIG_TIMER_PRIORITY) || !defined(OS_CONFIG_TIMER_STACK_SIZE) ||                     \
    !defined(OS_CONFIG_TIMER_CORE_AFFINITY) ||                                                        \
    !defined(OS_CONFIG_MAIN_TASK_STACK_SIZE) || !defined(OS_CONFIG_MAIN_TASK_PRIORITY) ||             \
    !defined(OS_CONFIG_TEST_STACK_SIZE) || !defined(OS_CONFIG_TEST_PRIORITY) ||                       \
    !defined(OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY) ||                  \
    !defined(OS_CONFIG_CORE_COUNT) ||                     \
    !defined(OS_CONFIG_TICKLESS_MIN_IDLE) ||                    \
    !defined(OS_CONFIG_MAX_SUPPRESSED_TICKS)
#error "os_config.h is incomplete: it must define every option listed in kernel/template/os_config.h."
#endif

/*
 * ***********************************************************************************************************
 * OPTIONAL configuration
 * ***********************************************************************************************************
 *
 * Same terms as the ARM port: everything checked above is mandatory because a missing switch would
 * read as 0 in an #if. The names below are defaulted with #ifndef instead, so a missing one is
 * caught rather than misread.
*/

/*
 * The symbol the port gives the machine software interrupt handler - the ONE vector the kernel must
 * own on RISC-V, and the structural counterpart of PendSV on ARM.
 *
 * It is NOT called PendSV, and the difference is not cosmetic. PendSV is an ARM Cortex-M exception
 * with no RISC-V equivalent. What RISC-V has is the machine software interrupt (mip.MSIP, trap
 * cause 3), which serves the same purpose: a low-priority, self-triggered trap that runs after the
 * interrupt that requested it has finished.
 *
 * The default is the Pico SDK's name, because crt0_riscv.S declares that vector weak precisely so
 * an RTOS can replace it at link time. Override it for a startup file that names entry 3 something
 * else.
 */
#ifndef OS_CONFIG_ARCH_SWI_HANDLER
#define OS_CONFIG_ARCH_SWI_HANDLER     isr_riscv_machine_soft_irq
#endif

/* Boot-time check that mtvec really routes the software interrupt to the kernel's handler. Same
 * rationale as the ARM port's PendSV check: if another definition won at link time the build still
 * succeeds, and the symptom is a board that reaches os_start() and stops dead. */
#ifndef OS_CONFIG_ARCH_VECTOR_CHECK
#define OS_CONFIG_ARCH_VECTOR_CHECK    1U
#endif

/*
 * Which section the context-switch handler is assembled into.
 *
 * A vectored mtvec table holds a JAL per cause, and JAL reaches +/-1MB. So the handler has to live
 * within that of the table - which is a fact about the linker script, not about the instruction
 * set, and therefore something a SoC package states rather than something the port can assume.
 *
 * It matters on any part whose vector table is in RAM while code runs from flash: on the RP2350 the
 * two are 256MB apart, and the build fails at link time with "relocation truncated to fit" rather
 * than misbehaving, which is the right way round. Putting the handler in RAM is also what an RTOS
 * wants regardless - it is the hottest path in the kernel, and it stops paying XIP latency on every
 * switch.
 *
 * Default is .text: correct wherever the table sits in flash beside the code.
 */
#ifndef OS_CONFIG_ARCH_SWI_SECTION
#define OS_CONFIG_ARCH_SWI_SECTION     ".text"
#endif

/* Where the periodic tick comes from. Unlike Cortex-M's SysTick there is no architectural timer
 * every RISC-V chip must have at a fixed address: mtime/mtimecmp are defined by the privileged
 * spec but their location is platform-defined. The SoC package therefore owns the timer, which is
 * why the RISC-V port defaults to the external source rather than programming one itself. */
#ifndef OS_CONFIG_TICK_SOURCE
#define OS_CONFIG_TICK_SOURCE          OS_CONFIG_TICK_SOURCE_EXTERNAL
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Architecture facts
 * ***********************************************************************************************************
*/

/* No Security Extension equivalent. RISC-V privilege modes are not TrustZone: there is no banked
 * stack pointer pair to switch per task and no secure-context API to call, so the kernel's
 * TrustZone paths compile out entirely. */
#define OS_ARCH_HAS_TRUSTZONE             0

#if (OS_CONFIG_TRUSTZONE != OS_CONFIG_TRUSTZONE_DISABLED)
#error "OS_CONFIG_TRUSTZONE must be OS_CONFIG_TRUSTZONE_DISABLED on RISC-V: the Security Extension is an Arm feature and has no RISC-V counterpart. See OS_ARCH_HAS_TRUSTZONE."
#endif

/* The A extension. Every Hazard3 build the Pico SDK produces includes it (-march=rv32imac...), so
 * lr.w/sc.w are available and the kernel's atomics are lock-free. A build for an rv32i core
 * without A would set this to 0 and fall back to the mask-based implementations. */
#if defined(__riscv_atomic) && (__riscv_atomic == 1)
#define OS_ARCH_HAS_EXCLUSIVES            1
#else
#define OS_ARCH_HAS_EXCLUSIVES            0
#endif

/* No BASEPRI equivalent in the base ISA. mstatus.MIE is all-or-nothing, the direct counterpart of
 * PRIMASK, so a kernel critical section masks every interrupt.
 *
 * Hazard3 does have a preemption-priority mechanism (meicontext/meinext, the Xh3irq extension), but
 * it governs the EXTERNAL interrupt dispatcher rather than a global threshold register, so it is
 * not a drop-in for BASEPRI and is deliberately not used here. OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY
 * is therefore required to be 0 - see the check below. */
#define OS_ARCH_HAS_BASEPRI               0

#if (OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY != 0U)
#error "OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY must be 0 on RISC-V: mstatus.MIE masks all interrupts or none, so there is no priority threshold to raise. See OS_ARCH_HAS_BASEPRI above."
#endif

/* Stack alignment. The RISC-V calling convention (ilp32) requires the stack pointer to be 16-byte
 * aligned at every procedure call boundary - stricter than ARM's 8, and it applies to the frame the
 * port lays down for a new task as well. */
#define OS_ARCH_STACK_ALIGNMENT_BYTES     16U

/* mstatus.MIE, bit 3: the global machine-interrupt enable. */
#define OS_ARCH_MSTATUS_MIE_MSK           (1UL << 3)

/* mie.MSIE / mie.MTIE / mie.MEIE - the per-source enables for the software, timer and external
 * machine interrupts, at their architectural bit positions (mip/mie share the layout). */
#define OS_ARCH_MIE_MSIE_MSK              (1UL << 3)
#define OS_ARCH_MIE_MTIE_MSK              (1UL << 7)
#define OS_ARCH_MIE_MEIE_MSK              (1UL << 11)

/* Trap causes, which double as the vector table index in mtvec vectored mode (offset = cause * 4). */
#define OS_ARCH_TRAP_CAUSE_SWI            3U
#define OS_ARCH_TRAP_CAUSE_TIMER          7U
#define OS_ARCH_TRAP_CAUSE_EXTERNAL       11U

#define OS_ARCH_STRINGIFY_(text)          #text
#define OS_ARCH_STRINGIFY(text)           OS_ARCH_STRINGIFY_(text)

/* Register qualifiers. These carry CMSIS names because the kernel core uses them for shared state
 * (see core/os_internal.h) and the ARM port introduced them - so they are part of the port contract
 * rather than an ARM detail, and every port owes them. Defined here rather than included from
 * anywhere: RISC-V has no CMSIS, and the kernel depends on no SDK. */
#ifndef __IO
#define __IO volatile             /*!< read/write */
#endif
#ifndef __I
#define __I  volatile const       /*!< read only  */
#endif
#ifndef __O
#define __O  volatile             /*!< write only */
#endif

/* Weak linkage, the other qualifier the kernel core expects from a port. It is what lets a SoC
 * package replace a default with a strong definition whatever order the linker sees them in - see
 * the one-definition rule in doc/soc.md. */
#ifndef OS_WEAK
#if defined(__GNUC__) || defined(__clang__)
#define OS_WEAK __attribute__((weak))
#else
#define OS_WEAK
#endif
#endif

/* The kernel spinlock. Declared here with the other architecture facts rather than beside its
 * accessors, because the SoC callbacks that may implement it are named before those. */
typedef struct
{
    volatile uint32_t locked;

} os_arch_spinlock_t;

#define OS_ARCH_SPINLOCK_INIT  { 0U }


/*
 * ***********************************************************************************************************
 * CSR access
 * ***********************************************************************************************************
 *
 * Written out here rather than taken from the Pico SDK's hardware/riscv.h, for the same reason the
 * ARM port defines __IO itself instead of including a CMSIS core header: the kernel names no vendor
 * and depends on no SDK. These are plain Zicsr instructions, which every RISC-V build has.
*/

#define OS_ARCH_CSR_READ(csr)                                                                        \
    ({ uint32_t os_arch_csr_value_;                                                                  \
       __asm volatile ("csrr %0, " OS_ARCH_STRINGIFY(csr) : "=r"(os_arch_csr_value_) :: "memory");   \
       os_arch_csr_value_; })

#define OS_ARCH_CSR_WRITE(csr, value)                                                                \
    do { __asm volatile ("csrw " OS_ARCH_STRINGIFY(csr) ", %0" :: "r"(value) : "memory"); } while (0)

#define OS_ARCH_CSR_SET(csr, mask)                                                                   \
    do { __asm volatile ("csrs " OS_ARCH_STRINGIFY(csr) ", %0" :: "r"(mask) : "memory"); } while (0)

#define OS_ARCH_CSR_CLEAR(csr, mask)                                                                 \
    do { __asm volatile ("csrc " OS_ARCH_STRINGIFY(csr) ", %0" :: "r"(mask) : "memory"); } while (0)

/* Read-and-set / read-and-clear as one instruction. The immediate forms take a 5-bit mask, which is
 * all mstatus.MIE needs, and they are what makes the kernel's mask save atomic - on ARM that same
 * step is a read followed by a separate cpsid. */
#define OS_ARCH_CSR_READ_SET_IMM(csr, imm)                                                           \
    ({ uint32_t os_arch_csr_value_;                                                                  \
       __asm volatile ("csrrsi %0, " OS_ARCH_STRINGIFY(csr) ", " OS_ARCH_STRINGIFY(imm)              \
                       : "=r"(os_arch_csr_value_) :: "memory");                                      \
       os_arch_csr_value_; })

#define OS_ARCH_CSR_READ_CLEAR_IMM(csr, imm)                                                         \
    ({ uint32_t os_arch_csr_value_;                                                                  \
       __asm volatile ("csrrci %0, " OS_ARCH_STRINGIFY(csr) ", " OS_ARCH_STRINGIFY(imm)              \
                       : "=r"(os_arch_csr_value_) :: "memory");                                      \
       os_arch_csr_value_; })

/*
 * ***********************************************************************************************************
 * Barriers, interrupt control and idle
 * ***********************************************************************************************************
*/

/* RISC-V has no DSB/ISB pair. "fence" orders memory accesses; "fence.i" additionally synchronises
 * the instruction stream, which is what an ISB is for. Both are mapped so shared kernel code reads
 * the same on either architecture. */
#define OS_ARCH_DSB()                     __asm volatile("fence" ::: "memory")
#define OS_ARCH_ISB()                     __asm volatile("fence.i" ::: "memory")

#define OS_ARCH_IRQ_DISABLE()             OS_ARCH_CSR_CLEAR(mstatus, OS_ARCH_MSTATUS_MIE_MSK)
#define OS_ARCH_IRQ_ENABLE()              OS_ARCH_CSR_SET(mstatus, OS_ARCH_MSTATUS_MIE_MSK)

#define OS_ARCH_IDLE()                    __asm volatile("wfi")
#define OS_ARCH_SLEEP(ticks)                                                                         \
    do { os_arch_sleep_prepare((ticks)); OS_ARCH_DSB(); __asm volatile("wfi"); OS_ARCH_ISB(); } while (0)

/*
 * Request a context switch on THIS core.
 *
 * On ARM this is one store to ICSR, because PendSV is architectural. RISC-V has the trap (mip.MSIP,
 * cause 3) but not a standard way to raise it: mip.MSIP is read-only to software, and what actually
 * sets it is platform hardware - SIO_RISCV_SOFTIRQ on the RP2350, a CLINT MSIP word elsewhere.
 *
 * By the kernel's own arch/soc rule - "if two chips with the same core would answer differently, it
 * is soc" - that store belongs to the SoC package, so the port asks for it through a callback
 * rather than inventing an address. os_arch_core_ipi_request_cb is the same operation aimed at
 * another core, and on a chip where one register serves both, a package implements the two in one
 * line each.
 */
#define OS_ARCH_CONTEXT_SWITCH_REQUEST()  os_arch_swi_request_cb()

/*
 * ***********************************************************************************************************
 * Inline helpers
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Index of the highest set bit in a non-zero bitmap (the scheduler's ready-priority pick).
 *        One clz instruction where Zbb is present, which every Pico SDK RISC-V build has; GCC falls
 *        back to a small library routine otherwise.
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

/******************************************************************************************************/
/**
 * @brief Raise the kernel interrupt mask; returns the previous mask state for restore.
 *
 * Returns 0 when interrupts were enabled on entry and nonzero when they were already masked - the
 * same convention the ARM port uses for PRIMASK, so the shared kernel code that saves and restores
 * these values needs no per-architecture knowledge.
 */
static inline uint32_t os_arch_kernel_mask_save(void)
{
    /* One instruction reads mstatus and clears MIE, so there is no window between sampling the
     * previous state and masking - the two-step the ARM PRIMASK path has to live with. */
    uint32_t previous = OS_ARCH_CSR_READ_CLEAR_IMM(mstatus, 8);

    return ((previous & OS_ARCH_MSTATUS_MIE_MSK) != 0UL) ? 0U : 1U;
}

/******************************************************************************************************/
/**
 * @brief Restore the kernel interrupt mask to a state returned by os_arch_kernel_mask_save.
 */
static inline void os_arch_kernel_mask_restore(uint32_t saved_state)
{
    if (saved_state == 0U)
    {
        OS_ARCH_IRQ_ENABLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Return nonzero while the kernel interrupt mask is raised (diagnostics/self-test).
 */
static inline uint32_t os_arch_kernel_mask_active(void)
{
    return ((OS_ARCH_CSR_READ(mstatus) & OS_ARCH_MSTATUS_MIE_MSK) != 0UL) ? 0U : 1U;
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

/*
 * ***********************************************************************************************************
 * SoC-owned callbacks
 * ***********************************************************************************************************
 *
 * The kernel calls these and defines none of them: a SoC package supplies the group, or the
 * application copies template/soc_cb.c. See doc/soc.md for the one-definition rule that makes the
 * split safe.
*/

/******************************************************************************************************/
/**
 * @brief Raise this core's machine software interrupt - the context-switch request.
 *
 * The one callback the RISC-V port needs that the ARM port does not, because ICSR has no RISC-V
 * equivalent. See OS_ARCH_CONTEXT_SWITCH_REQUEST above for why this is soc-owned.
 */
void os_arch_swi_request_cb(void);

/******************************************************************************************************/
/**
 * @brief Acknowledge this core's machine software interrupt. Called first thing in the kernel's
 *        handler, because mip.MSIP is cleared at the platform register rather than by entering the
 *        trap - leave it set and the trap re-enters forever.
 */
void os_arch_swi_clear_cb(void);

/******************************************************************************************************/
/**
 * @brief Per-core SoC start-up, called from os_init() before the tick is programmed.
 */
void os_arch_soc_init_cb(void);

/******************************************************************************************************/
/**
 * @brief Start the application-owned tick timer under OS_CONFIG_TICK_SOURCE_EXTERNAL.
 */
void os_arch_tick_init_cb(void);

#if (OS_CONFIG_CORE_COUNT > 1U)
/******************************************************************************************************/
/**
 * @brief This core's index.
 */
uint32_t os_arch_core_id_get_cb(void);

/******************************************************************************************************/
/**
 * @brief Interrupt another core so it re-evaluates scheduling.
 */
void os_arch_core_ipi_request_cb(uint32_t core_id);

/******************************************************************************************************/
/**
 * @brief Release a secondary core into the kernel.
 */
void os_arch_core_launch_cb(uint32_t core_id);

/******************************************************************************************************/
/**
 * @brief Take / release the SoC's own cross-core lock, under OS_CONFIG_SPINLOCK_SOC_BACKEND.
 */
void os_arch_spinlock_acquire_cb(os_arch_spinlock_t *lock);
void os_arch_spinlock_release_cb(os_arch_spinlock_t *lock);

/******************************************************************************************************/
/**
 * @brief Top of the trap stack for the given core.
 */
uint32_t os_arch_handler_stack_top_cb(uint32_t core_id);

/******************************************************************************************************/
/**
 * @brief Limit of the trap stack for the given core.
 */
uint32_t os_arch_handler_stack_limit_cb(uint32_t core_id);
#endif

/*
 * ***********************************************************************************************************
 * Trap context
 * ***********************************************************************************************************
 *
 * "Am I in an interrupt?" is one CSR read on Cortex-M (IPSR) and genuinely has no architectural
 * answer on RISC-V. The Pico SDK says so in as many words: "there is no way to get the current
 * exception on RISC-V (as there is no such thing -- the hardware does not model the exception
 * lifecycle like on Arm)".
 *
 * The answer therefore comes from two places, OR'd together, and between them they cover every
 * context that can reach a kernel API:
 *
 *   1. The core's interrupt controller, where it has one. On Hazard3 meicontext.NOIRQ is clear
 *      exactly while an external IRQ is being dispatched, which is self-maintaining, correct
 *      through arbitrary nesting, and costs four instructions. THIS is the one that matters,
 *      because every application ISR and the kernel tick arrive as external IRQs.
 *
 *   2. os_arch_isr_nesting, for a trap that is NOT an external IRQ and still wants kernel APIs.
 *      Nothing in the kernel needs it today - the context-switch handler deliberately does not
 *      touch it, since it calls no API that asks and it is the hottest path in the port - but a
 *      tick driven straight off mip.MTIP rather than through the interrupt controller would, and
 *      os_arch_isr_enter/exit() are how such a handler declares itself.
 *
 * Getting this wrong is not loud: os_task_current_get() would hand an ISR a task pointer, and a
 * blocking call from interrupt context would be accepted instead of rejected. Hence two mechanisms
 * rather than a single one that is nearly always right.
*/

/* Set while a non-external trap that wants kernel APIs is running; see os_arch_isr_enter(). */
extern volatile uint32_t os_arch_isr_nesting[OS_CONFIG_CORE_COUNT];

#ifndef OS_ARCH_HAS_XH3IRQ
#define OS_ARCH_HAS_XH3IRQ    0
#endif

#if (OS_ARCH_HAS_XH3IRQ == 1)

/* meicontext, Hazard3's interrupt-context CSR. Addressed by number rather than by name because the
 * assembler only knows the standard CSRs - a vendor extension is not in its table - and named here
 * rather than pulled from the SDK's rvcsr.h for the reason given in os_arch_port.h. */
#define OS_ARCH_CSR_MEICONTEXT            0xbe5
#define OS_ARCH_MEICONTEXT_NOIRQ_MSK      (1UL << 15)

/******************************************************************************************************/
/**
 * @brief True while the core's interrupt controller is dispatching an external IRQ.
 *
 * NOIRQ reads 1 when no IRQ is active, so the sense is inverted here. The bit is maintained by the
 * hardware and by the dispatcher's meinext updates, which is what makes this correct under
 * preemption without the port counting anything.
 */
static inline bool os_arch_ext_irq_active(void)
{
    uint32_t meicontext;

    __asm volatile("csrr %0, " OS_ARCH_STRINGIFY(OS_ARCH_CSR_MEICONTEXT) : "=r"(meicontext));

    return ((meicontext & OS_ARCH_MEICONTEXT_NOIRQ_MSK) == 0UL);
}

#else

/******************************************************************************************************/
/**
 * @brief No interrupt controller the port knows how to ask; os_arch_isr_nesting carries the answer
 *        alone, and every trap that uses kernel APIs must bracket itself.
 */
static inline bool os_arch_ext_irq_active(void)
{
    return false;
}

#endif

/******************************************************************************************************/
/**
 * @brief Declare that a non-external trap handler is running, so os_arch_in_isr() reports it.
 *
 * Only needed by a handler the core's interrupt controller does not account for - a tick taken
 * straight off mip.MTIP is the realistic case. An external IRQ needs neither call.
 */
static inline void os_arch_isr_enter(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    os_arch_isr_nesting[os_arch_core_id_get_cb()]++;
#else
    os_arch_isr_nesting[0]++;
#endif
}

/******************************************************************************************************/
/**
 * @brief Close the bracket opened by os_arch_isr_enter().
 */
static inline void os_arch_isr_exit(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    os_arch_isr_nesting[os_arch_core_id_get_cb()]--;
#else
    os_arch_isr_nesting[0]--;
#endif
}

/*
 * ***********************************************************************************************************
 * CPU clock
 * ***********************************************************************************************************
 *
 * The kernel needs the CPU frequency to size the tick and the microsecond busy-waits. On CMSIS
 * parts that symbol is SystemCoreClock and the vendor's startup code maintains it; RISC-V has no
 * such convention at all, so the value comes through the SoC package like every other silicon fact.
*/

/******************************************************************************************************/
/**
 * @brief The CPU clock in Hz.
 */
uint32_t os_arch_clock_hz_get(void);

/*
 * ***********************************************************************************************************
 * Public function prototypes
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Per-core architecture start-up: install mtvec if the port owns it, verify the software
 *        interrupt vector, and enable the interrupt sources the kernel needs.
 */
void os_arch_init(void);

/******************************************************************************************************/
/**
 * @brief Switch to the first task and never return.
 */
void os_arch_start_first_task(void);

/******************************************************************************************************/
/**
 * @brief Program the periodic tick. Defers to os_arch_tick_init_cb() under the external tick
 *        source, which is the RISC-V default - see OS_CONFIG_TICK_SOURCE above.
 */
void os_arch_tick_init(void);

/******************************************************************************************************/
/**
 * @brief Free-running cycle counter, for the CPU-load sampler and the benchmark table.
 *        Reads the mcycle CSR, which the privileged spec makes architectural.
 */
uint32_t os_arch_cycle_count_get(void);

/******************************************************************************************************/
/**
 * @brief Whole ticks elapsed since the last tick interrupt (tickless accounting).
 */
uint32_t os_arch_elapsed_ticks_get(void);

/******************************************************************************************************/
/**
 * @brief Lay a task's initial trap frame and return its stack pointer.
 *
 * The frame layout is entirely the port's business - the core only ever holds the pointer this
 * returns. It differs from the ARM one in kind, not just in size: RISC-V stacks NOTHING
 * automatically on a trap, so every register the task expects to see has to be written here.
 */
uint32_t* os_arch_task_stack_initialize(uint8_t *stack_base, size_t stack_bytes,
                                        void (*entry)(void *context), void *context);

/******************************************************************************************************/
/**
 * @brief Where a task lands if its entry function returns. Never reached by a well-formed task.
 */
void os_arch_task_exit_trap(void);

/******************************************************************************************************/
/**
 * @brief True when the caller is in trap context.
 */
bool os_arch_in_isr(void);

/******************************************************************************************************/
/**
 * @brief Assert that the calling interrupt is allowed to use kernel APIs.
 *
 * A no-op on RISC-V. On ARM this catches an ISR above OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY calling
 * into the kernel; here every kernel critical section masks all interrupts (OS_ARCH_HAS_BASEPRI is
 * 0), so there is no such class of interrupt to catch.
 */
void os_arch_isr_priority_check(void);

/******************************************************************************************************/
/**
 * @brief Read a word indivisibly.
 *
 * Inline here rather than written out in os_arch_atomic.c, for the same reason as on ARM: a
 * naturally aligned 32-bit load is already indivisible on RV32, so the whole operation is one lw and
 * calling across to the port would cost several times what it does. The volatile access is what
 * stops the compiler reusing a value it cached before another path changed the word.
 */
static inline int32_t os_arch_atomic_load(const volatile int32_t *target)
{
    return *target;
}

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

/*
 * The kernel's atomic set. Lock-free wherever OS_ARCH_HAS_EXCLUSIVES is 1, which on RV32 means the
 * A extension; see os_arch_atomic.c for how much of this is a single instruction here.
 *
 * Each returns the value held BEFORE the operation.
 */
int32_t os_arch_atomic_exchange(volatile int32_t *target, int32_t value);
int32_t os_arch_atomic_add(volatile int32_t *target, int32_t value);
int32_t os_arch_atomic_sub(volatile int32_t *target, int32_t value);
int32_t os_arch_atomic_or(volatile int32_t *target, int32_t value);
int32_t os_arch_atomic_and(volatile int32_t *target, int32_t value);
int32_t os_arch_atomic_xor(volatile int32_t *target, int32_t value);
int32_t os_arch_atomic_nand(volatile int32_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Compare-and-swap, used by the kernel's atomics.
 */
bool os_arch_atomic_cas(volatile int32_t *target, int32_t expected, int32_t desired);

#endif /* OS_CONFIG_ATOMIC_ENABLE */

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)
/******************************************************************************************************/
/**
 * @brief Stretch the tick across a known-idle window.
 */
void os_arch_sleep_prepare(uint32_t planned_ticks);

/******************************************************************************************************/
/**
 * @brief Close out a tickless window and re-arm the periodic tick.
 */
void os_arch_sleep_finish(void);

/******************************************************************************************************/
/**
 * @brief Largest number of ticks the timer can suppress in one window.
 */
uint32_t os_arch_max_suppressed_ticks_get(void);
#endif

/*
 * ***********************************************************************************************************
 * Spinlock
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief This core's index. Constant 0 on a single-core build, so the callback is never reached and
 *        a package that does not implement it is still a valid single-core package.
 */
static inline uint32_t os_arch_core_id_get(void)
{
#if (OS_CONFIG_CORE_COUNT > 1U)
    return os_arch_core_id_get_cb();
#else
    return 0U;
#endif
}

/******************************************************************************************************/
/**
 * @brief Take a kernel spinlock. Spins until it is held; callers mask interrupts around it.
 */
static inline void os_arch_spinlock_acquire(os_arch_spinlock_t *lock)
{
#if (OS_CONFIG_CORE_COUNT == 1U)
    (void)lock;
#elif (OS_CONFIG_SPINLOCK_SOC_BACKEND == 1U)
    os_arch_spinlock_acquire_cb(lock);
#elif (OS_ARCH_HAS_EXCLUSIVES == 1)
    uint32_t current;
    uint32_t failed;

    do
    {
        /* Reserve, wait for the lock to read free, then try to claim it. sc.w reports failure if
         * anything touched the word since the lr.w, which is what makes the claim exclusive. */
        do
        {
            __asm volatile("lr.w %0, (%1)" : "=r"(current) : "r"(&lock->locked) : "memory");
        } while (current != 0U);

        __asm volatile("sc.w %0, %1, (%2)"
                       : "=&r"(failed) : "r"(1U), "r"(&lock->locked) : "memory");
    } while (failed != 0U);

    /* Nothing inside the critical section may be hoisted above the claim. */
    __asm volatile("fence r, rw" ::: "memory");
#else
#error "Multi-core build without exclusives and without a SoC spinlock backend: a lock that excludes nothing. Set OS_CONFIG_SPINLOCK_SOC_BACKEND, or build single-core."
#endif
}

/******************************************************************************************************/
/**
 * @brief Release a kernel spinlock.
 */
static inline void os_arch_spinlock_release(os_arch_spinlock_t *lock)
{
#if (OS_CONFIG_CORE_COUNT == 1U)
    (void)lock;
#elif (OS_CONFIG_SPINLOCK_SOC_BACKEND == 1U)
    os_arch_spinlock_release_cb(lock);
#else
    /* Everything inside the critical section must be visible before the lock reads free. */
    __asm volatile("fence rw, w" ::: "memory");
    lock->locked = 0U;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* OS_ARCH_PORT_COMMON_H */
