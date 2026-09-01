/**
 * @file os_arch_port_v8m.c
 * @brief Shared port for ARMv8-M mainline (Cortex-M33, M35P) and ARMv8.1-M (M52, M55, M85). A
 *        superset of the v7m port: it adds per-task PSPLIM overflow detection and an MSPLIM guard
 *        for the handler stack. Helium (MVE) needs no extra handling - Q4-Q7 alias s16-s31, which
 *        are already saved, and the hardware lazy-stacks the rest.
 *
 * Textually included by each variant's os_arch_port.c wrapper; ARMv8-M baseline (Cortex-M23) runs
 * the Thumb-1 subset and lives in os_arch_port_v6m.c instead. TrustZone (via OS_CONFIG_TRUSTZONE)
 * may be disabled, secure, or non-secure - in which case the context switch banks per-task secure
 * state through the tz_context callbacks and the initial frames use the non-secure EXC_RETURN.
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

#include "os_arch_port_common.h"

#if !defined(__ARM_ARCH_8M_MAIN__) && !defined(__ARM_ARCH_8_1M_MAIN__)
#error "os_arch_port_v8m.c targets ARMv8-M mainline / ARMv8.1-M cores (check -mcpu / -march)."
#endif

/* Cycle counter used whenever DWT CYCCNT turns out to be unavailable on this
 * device (see os_arch_cycle_count_get). Textual include, like this file itself. */
#include "os_arch_cycle_systick.c"

/* The atomic operation set, shared by all three ports: its backend follows the
 * instruction set (OS_ARCH_ATOMIC_LOCK_FREE), not the v6m/v7m/v8m split. Textual
 * include as well. */
#include "os_arch_atomic.c"

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_ARCH_REG_SHPR3                    (*(__IO uint32_t *)0xE000ED20UL)
#define OS_ARCH_REG_AIRCR                    (*(__IO uint32_t *)0xE000ED0CUL)
#define OS_ARCH_REG_DEMCR                    (*(__IO uint32_t *)0xE000EDFCUL)
#define OS_ARCH_REG_DWT_CTRL                 (*(__IO uint32_t *)0xE0001000UL)
#define OS_ARCH_REG_DWT_CYCCNT               (*(__IO uint32_t *)0xE0001004UL)
#define OS_ARCH_REG_DWT_LAR                  (*(__IO uint32_t *)0xE0001FB0UL)

#define OS_ARCH_DEMCR_TRCENA_MSK             (1UL << 24)
#define OS_ARCH_DWT_CTRL_CYCCNTENA_MSK       (1UL << 0)
#define OS_ARCH_DWT_CTRL_NOCYCCNT_MSK        (1UL << 25)
#define OS_ARCH_DWT_LAR_UNLOCK_KEY           0xC5ACCE55UL

/* SVCall's priority field (SHPR2) is deliberately absent: the kernel does not
 * use SVC, so it has no business changing that exception's priority. */
#define OS_ARCH_SHPR3_PENDSV_PRI_POS         16U
#define OS_ARCH_SHPR3_SYSTICK_PRI_POS        24U

#define OS_ARCH_PRIORITY_LOWEST              255U
#define OS_ARCH_XPSR_THUMB                   (1UL << 24)

/*
 * EXC_RETURN for the initial task frame: return to thread mode, use PSP,
 * basic (non-FPU) stack frame. Stored as part of the software-saved context
 * so each task carries its own frame type across switches. A non-secure
 * TrustZone kernel returns with the S and ES bits clear (0xFFFFFFBC); the
 * secure and TrustZone-less encodings are both 0xFFFFFFFD.
 */
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFBCUL
#else
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFFDUL
#endif

#define OS_ARCH_CONTROL_FPCA_MSK             (1UL << 2)

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/* Tickless idle (SysTick suppression): os_arch_tick_reload_cycles is the normal
 * 1-tick reload, cached once by os_arch_tick_init(); os_arch_planned_idle_ticks
 * is the effective (possibly 24-bit-capped) request, 0 = not currently armed;
 * os_arch_suppressed_reload_cycles is the cycle count actually programmed for
 * (planned - 1) ticks (see os_arch_sleep_prepare); os_arch_sleep_mask_state is
 * the os_arch_kernel_mask_save() token, acquired in os_arch_sleep_prepare and
 * released in os_arch_elapsed_ticks_get. */
static uint32_t os_arch_tick_reload_cycles      = 0U;
static uint32_t os_arch_planned_idle_ticks      = 0U;
static uint32_t os_arch_suppressed_reload_cycles = 0U;

/** Cycles of the tick already running when the window opened; the first boundary falls there. */
static uint32_t os_arch_suppressed_head_cycles = 0U;
static uint32_t os_arch_sleep_mask_state        = 0U;

/* Whether os_arch_sleep_mask_state actually holds a mask os_arch_sleep_finish must release. */
static bool     os_arch_sleep_mask_held         = false;

/* Whether DWT CYCCNT is present and actually counting on this device; decided
 * once in os_arch_init(). False routes os_arch_cycle_count_get() to the
 * SysTick-derived counter instead. */
static bool     os_arch_dwt_available           = false;

/*
 * ***********************************************************************************************************
 * Context switch handler (PendSV does everything)
 * ***********************************************************************************************************
 *
 * Software-saved frame layout on a task stack (low address first):
 *   [ s16-s31 ]  only when the task was using the FPU (EXC_RETURN bit 4 clear)
 *   PSPLIM       per-task stack limit (always present on ARMv8-M mainline)
 *   r4-r11, EXC_RETURN
 *   [ hardware frame: r0-r3, r12, lr, pc, xpsr, (s0-s15, fpscr) ]
 *
 * Storing EXC_RETURN with the context lets each task keep its own frame type,
 * which is mandatory with -mfloat-abi=hard where any task or the startup code
 * may touch the FPU. os_task_stack_select_next() never returns NULL (the idle
 * task always exists), so the restore path needs no fallback.
 *
 * PendSV is the ONLY exception this kernel takes over, and the PSP == 0
 * sentinel is what lets one handler serve both jobs: zero means no task has
 * run yet, so there is no outgoing context to save and the handler simply
 * installs the first task (the "first start" path below). Every later entry
 * finds a real PSP and performs an ordinary switch. See os_arch_port_v7m.c's
 * equivalent block for why SVC is deliberately left to the application - a
 * point that matters most on exactly this core, where secure firmware
 * (TF-M and the like) routinely uses SVC for its own gateway calls.
 *
 * The first-start path also resets MSP to a clean top, abandoning the boot
 * context it was entered with. The vector table only names ONE initial stack
 * pointer - core 0's - so on a multi-core build the top comes from
 * os_arch_handler_stack_top_cb(), which the SoC layer answers per core. A
 * secondary core that reset from the table instead would park its MSP inside
 * core 0's handler stack and both cores would overwrite each other's frames.
*/

__asm(
".syntax unified\n"
".thumb\n"
".text\n"
".align 2\n"

".global " OS_ARCH_STRINGIFY(OS_CONFIG_ARCH_PENDSV_HANDLER) "\n"
".type   " OS_ARCH_STRINGIFY(OS_CONFIG_ARCH_PENDSV_HANDLER) ", %function\n"
".thumb_func\n"
OS_ARCH_STRINGIFY(OS_CONFIG_ARCH_PENDSV_HANDLER) ":\n"
"    mrs     r0, psp\n"
"    cbz     r0, 1f\n"                     /* PSP == 0: no task has run yet, go start the first */
#if defined(__ARM_FP)
"    tst     lr, #0x10\n"
"    it      eq\n"
"    vstmdbeq r0!, {s16-s31}\n"            /* task used the FPU: save callee-saved FP regs */
#endif
"    mrs     r2, psplim\n"                 /* save the per-task stack limit */
"    stmdb   r0!, {r2, r4-r11, lr}\n"
"    bl      os_task_stack_save_current\n" /* r0 = stack pointer of outgoing task */
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
"    bl      os_arch_tz_context_save\n"    /* bank the outgoing task's secure context */
#endif
"    bl      os_task_stack_select_next\n"  /* r0 = stack pointer of incoming task */
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
"    mov     r4, r0\n"                     /* r4 was saved above; free to clobber here */
"    bl      os_arch_tz_context_restore\n" /* load the incoming task's secure context */
"    mov     r0, r4\n"
#endif
"    b       os_arch_context_restore_asm\n"

"1:\n"                                     /* first start: nothing to save */
"    bl      os_task_stack_select_next\n"  /* r0 = first task stack pointer */
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
"    mov     r4, r0\n"                     /* r4 survives the call (callee-saved) */
"    bl      os_arch_tz_context_restore\n" /* load the first task's secure context */
"    mov     r0, r4\n"
#endif
#if (OS_CONFIG_CORE_COUNT > 1U)
"    mov     r5, r0\n"                     /* keep the task stack pointer across the calls below */
"    bl      os_arch_core_id_get_cb\n"     /* r0 = this core's id */
"    bl      os_arch_handler_stack_top_cb\n" /* r0 = this core's handler stack top */
"    msr     msp, r0\n"                    /* abandon the boot context, including the frame this  */
"    mov     r0, r5\n"                     /* exception pushed - the return below unstacks from   */
#else                                      /* PSP instead. Single-core: the vector-table value    */
"    movw    r1, #0xED08\n"                /* is exactly this core's stack, and always was.      */
"    movt    r1, #0xE000\n"
"    ldr     r1, [r1]\n"
"    ldr     r1, [r1]\n"
"    msr     msp, r1\n"
#endif
"    b       os_arch_context_restore_asm\n"

".global os_arch_context_restore_asm\n"
".type   os_arch_context_restore_asm, %function\n"
".thumb_func\n"
"os_arch_context_restore_asm:\n"           /* r0 = stack pointer of task to restore */
"    clrex\n"                              /* drop any LDREX reservation the outgoing task left */
"    ldmia   r0!, {r2, r4-r11, lr}\n"
"    msr     psplim, r2\n"                 /* restore the stack limit before PSP */
#if defined(__ARM_FP)
"    tst     lr, #0x10\n"
"    it      eq\n"
"    vldmiaeq r0!, {s16-s31}\n"
#endif
"    msr     psp, r0\n"
"    dsb\n"
"    isb\n"
"    bx      lr\n"
);

/* Declared through the configured name so the boot-time vector check compares
 * against exactly the symbol the vector table is expected to reference. */
extern void OS_CONFIG_ARCH_PENDSV_HANDLER(void);

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

extern void     os_task_exit(void);
extern uint32_t os_task_current_id_get(void);

/* Bottom of the main (handler) stack, under the two names linker scripts
 * commonly give it (__StackLimit in CMSIS-style scripts, _sstack in several
 * vendor-generated ones). Both are weak references, so either naming works
 * unmodified; when neither symbol exists both resolve to address 0 and the
 * MSPLIM guard is skipped. */
extern uint32_t __StackLimit OS_WEAK;
extern uint32_t _sstack      OS_WEAK;

static bool     os_arch_dwt_enable(void);
static void     os_arch_task_exit_trap(void);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize architecture-specific low-level resources.
 *
 * @return None.
 */
/******************************************************************************************************/
/**
 * @brief Default: the chip cannot be asked. Overridden by a SoC package that knows where its
 *        security-enable bit lives. OS_WEAK, so a package's strong definition wins whatever order
 *        the linker sees them in - the same rule as every other SoC callback (doc/soc.md).
 */
OS_WEAK uint32_t os_arch_soc_trustzone_state_cb(void)
{
    return OS_CONFIG_TRUSTZONE_UNKNOWN;
}

/******************************************************************************************************/
/**
 * @brief Trap if the silicon's actual security state contradicts OS_CONFIG_TRUSTZONE.
 *
 * Compiles to nothing where no package answers, which is every unpackaged part.
 */
OS_INLINE void os_arch_trustzone_state_check(void)
{
    uint32_t actual = os_arch_soc_trustzone_state_cb();

    if ((actual != OS_CONFIG_TRUSTZONE_UNKNOWN) && (actual != (uint32_t)OS_CONFIG_TRUSTZONE))
    {
        /* Stopping here is the whole point. A secure-built kernel on a device whose security
         * extension was never armed does not fault at the mismatch - it faults later, somewhere
         * unrelated, on the first banked operation. */
        os_arch_config_fault_trap();
    }
}

void os_arch_init(void)
{
    uint32_t shpr3 = OS_ARCH_REG_SHPR3;

    /* Before anything else: confirm the vector table really routes PendSV
     * here. Everything below assumes the kernel owns that exception, and a
     * table that does not is a silent hang rather than a fault. */
    os_arch_vector_check(OS_CONFIG_ARCH_PENDSV_HANDLER);

    /* Same spirit as the vector check above: confirm the device agrees with the build.
     * Costs nothing unless a SoC package can actually answer. */
    os_arch_trustzone_state_check();

    /* PSP == 0 is the sentinel the PendSV handler uses to recognize "no task
     * context yet" (see the context-switch block above). Primed here - the
     * very first arch call from os_init(), before os_tick_init() ever starts
     * the tick - rather than only in os_arch_start_first_task:
     * os_kernel_running is set true in os_start() a few instructions before
     * that function re-primes PSP, and interrupts stay enabled the whole time
     * (the kernel never masks them at boot), so a tick landing in that gap
     * would pend a PendSV that reads PSP's architecturally-unpredictable
     * power-on-reset value instead of the sentinel - PSP is not the active
     * stack pointer yet (Thread mode still runs on MSP), so priming it this
     * early has no other effect and closes the window unconditionally. */
    __asm volatile("msr psp, %0" :: "r"(0U));
    OS_ARCH_ISB();

    /* PendSV lowest, which is a correctness requirement rather than a preference: the switch has to
     * wait until every other exception has returned, or it would swap stacks underneath a handler
     * that has not finished, and that handler would resume inside a different task. 0xFF sets every
     * priority bit the device actually implements, whatever their number, so it lands on the lowest
     * level without the port needing to know. SVCall's priority (SHPR2) is left exactly as the
     * application set it: the kernel does not use SVC. */
    shpr3 &= ~(0xFFUL << OS_ARCH_SHPR3_PENDSV_PRI_POS);
    shpr3 |= ((uint32_t)OS_ARCH_PRIORITY_LOWEST << OS_ARCH_SHPR3_PENDSV_PRI_POS);

#if (OS_CONFIG_TICK_SOURCE == OS_CONFIG_TICK_SOURCE_SYSTICK)
    /* The tick goes to the lowest priority too, for latency rather than correctness: it only does
     * scheduling bookkeeping, so it has no business delaying a device interrupt.
     *
     * Only where the kernel OWNS SysTick. With an EXTERNAL tick source the application owns that
     * timer and may be using SysTick for something else entirely (a HAL timebase is the usual one),
     * at a priority it chose. os_arch_tick_init() programs nothing in that mode, and quietly
     * overriding the priority here would contradict it. */
    shpr3 &= ~(0xFFUL << OS_ARCH_SHPR3_SYSTICK_PRI_POS);
    shpr3 |= ((uint32_t)OS_ARCH_PRIORITY_LOWEST << OS_ARCH_SHPR3_SYSTICK_PRI_POS);
#endif

    OS_ARCH_REG_SHPR3 = shpr3;

#if (OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY != 0U)
    /* The raw-byte comparisons in os_arch_isr_priority_check are only exact
     * when (1) the configured threshold lives entirely in this device's
     * implemented priority bits (write-back must return it unchanged; a
     * truncated value would mask at a different level than the check tests)
     * and (2) the priority grouping dedicates every implemented bit to
     * preemption - no subpriority bits (BASEPRI masks by GROUP priority, so
     * subpriority bits would let the byte compare disagree with the
     * hardware's masking decision). Violations park here at boot instead of
     * running with checks that silently differ from the mask. */
    {
        uint32_t readback;
        uint32_t implemented;
        uint32_t prigroup;

        __asm volatile("msr basepri, %0" :: "r"((uint32_t)OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY) : "memory");
        __asm volatile("mrs %0, basepri" : "=r"(readback));
        __asm volatile("msr basepri, %0" :: "r"(0xFFU) : "memory");
        __asm volatile("mrs %0, basepri" : "=r"(implemented));
        __asm volatile("msr basepri, %0" :: "r"(0U) : "memory");

        prigroup = (OS_ARCH_REG_AIRCR >> 8) & 0x7U;

        if ((readback != (uint32_t)OS_CONFIG_MAX_SYSCALL_IRQ_PRIORITY) ||
            ((implemented & ((1UL << (prigroup + 1U)) - 1U)) != 0U))
        {
            os_arch_config_fault_trap();
        }
    }
#endif

    /* Start the cycle counter used for precise busy-wait delays and tickless
     * accounting. The LAR write unlocks DWT on cores implementing the
     * CoreSight software lock; it is ignored elsewhere. */
    OS_ARCH_REG_DEMCR   |= OS_ARCH_DEMCR_TRCENA_MSK;
    OS_ARCH_REG_DWT_LAR  = OS_ARCH_DWT_LAR_UNLOCK_KEY;

    os_arch_dwt_available = os_arch_dwt_enable();

    os_arch_cycle_systick_reset();
    os_arch_planned_idle_ticks = 0U;

    /* Guard the handler stack: an MSP push below the stack bottom raises a
     * UsageFault instead of corrupting whatever sits below the stack.
     * MSPLIM ignores its low 3 bits, so round the limit up. Skipped (MSPLIM
     * keeps its reset value 0 = no checking) when no known symbol exists. */
    /* The limit comes from os_arch_handler_stack_limit_cb, per core, because only the SoC layer
     * knows where each core's handler stack really is. Zero means "not known here", and the guard
     * is then left at its reset value rather than pointed somewhere arbitrary. */
    {
        uint32_t limit = os_arch_handler_stack_limit_cb(os_arch_core_id_get());

        if (limit != 0U)
        {
            /* MSPLIM ignores its low 3 bits, so round up rather than down: a limit rounded the
             * other way sits BELOW the stack it guards and lets the first overflowing push
             * through. */
            uint32_t msp_limit = (limit + 7U) & ~(uint32_t)0x7U;

            __asm volatile("msr msplim, %0" :: "r"(msp_limit));
        }
    }
}

/******************************************************************************************************/
/**
 * @brief Bottom of the calling core's handler (MSP) stack, or 0 when the port cannot know it.
 *
 * WEAK: this default answers from the linker, which can only describe a single stack, so it
 * answers for core 0 and declines for every other core. A multi-core SoC package overrides it -
 * that layer placed the secondary stacks and is the only one that knows where they are.
 *
 * Getting this wrong is worse than declining, which is why the zero case exists. An MSPLIM set
 * below the stack it is meant to guard does not protect it and does not fail either: the overflow
 * runs on, silently, through whatever lives underneath, and only faults once it has passed the
 * wrong limit too - by which time the memory that explains it is gone. That is not hypothetical.
 * On the RP2350 the Pico SDK's __StackLimit is the bottom of the LOWEST stack, which on a
 * dual-core build is core 1's, six kilobytes under core 0's - so core 0's handler overflow used
 * to run straight through core 1's entire stack before tripping anything, and presented as core 1
 * dying at random points for no visible reason.
 *
 * @param[in] core_id  Core asking about its own stack.
 * @return uint32_t  Lowest address the MSP may reach, or 0 for "unknown, leave the guard off".
 */
OS_WEAK uint32_t os_arch_handler_stack_limit_cb(uint32_t core_id)
{
    uint32_t *stack_limit;

#if (OS_CONFIG_CORE_COUNT > 1U)
    if (core_id != 0U)
    {
        /* No stack placed for a core this port does not know about. */
        stack_limit = NULL;
    }
    else
#else
    (void)core_id;
#endif
    {
    /* Two names for the same thing: __StackLimit in CMSIS-style scripts, _sstack in several
     * vendor-generated ones. Both weak, so either naming works unmodified and neither existing
     * resolves to 0. */
    stack_limit = &__StackLimit;

    if (stack_limit == NULL)
    {
        stack_limit = &_sstack;
    }
    }

    return (uint32_t)(uintptr_t)stack_limit;
}

/******************************************************************************************************/
/**
 * @brief Start the first task context. Does not return.
 *
 * @return None.
 */
void os_arch_start_first_task(void)
{
#if defined(__ARM_FP)
    uint32_t control;

    /* Startup/HAL code (hard-float ABI) may have used the FPU: clear FPCA so
     * the bootstrap exception stacks a basic frame and leaves no lazy FP state
     * pointing at the abandoned main stack. */
    __asm volatile("mrs %0, control" : "=r"(control));
    control &= ~OS_ARCH_CONTROL_FPCA_MSK;
    __asm volatile("msr control, %0" :: "r"(control));
    OS_ARCH_ISB();
#endif

    /* PSP == 0 tells the PendSV handler there is no task context to save yet,
     * so it takes its "first start" path and installs the first task. */
    __asm volatile("msr psp, %0" :: "r"(0U));
    OS_ARCH_ISB();

    OS_ARCH_CONTEXT_SWITCH_REQUEST();
    OS_ARCH_IRQ_ENABLE();

    /* Never reached: PendSV is the lowest priority, so it is taken as soon as
     * nothing else is pending, and it returns into the first task rather than
     * back to here. */
    while (1)
    {
        OS_ARCH_IDLE();
    }
}

/******************************************************************************************************/
/**
 * @brief Start the kernel tick. See os_arch_port_common.h.
 *
 * @return None.
 */
void os_arch_tick_init(void)
{
#if (OS_CONFIG_TICK_SOURCE == OS_CONFIG_TICK_SOURCE_EXTERNAL)
    /* The application owns the tick hardware; the port programs nothing.
     * os_arch_tick_reload_cycles stays 0, which is exactly what disables this
     * port's SysTick tick suppression - see os_arch_sleep_prepare. */
    os_arch_tick_init_cb();
#else
    uint32_t clock_hz = os_arch_clock_hz_get();
    uint32_t reload_value;

    /* A zero clock, a zero tick rate, or a reload the timer cannot hold all mean there is
     * nothing sane to program, so the whole body is skipped rather than each bailing out. */
    if ((clock_hz != 0U) && (OS_CONFIG_TICK_HZ != 0U))
    {
    reload_value = (clock_hz / OS_CONFIG_TICK_HZ);

    if ((reload_value != 0U) && (reload_value <= (OS_ARCH_SYST_RVR_RELOAD_MSK + 1UL)))
    {

    /* Cached for tickless idle: os_arch_elapsed_ticks_get() restores exactly
     * this cadence after a suppressed sleep. */
    os_arch_tick_reload_cycles = reload_value;

    OS_ARCH_REG_SYST_CSR = 0U;
    OS_ARCH_REG_SYST_RVR = reload_value - 1UL;
    OS_ARCH_REG_SYST_CVR = 0U;
    OS_ARCH_REG_SYST_CSR = OS_ARCH_SYST_CSR_CLKSOURCE_MSK |
                           OS_ARCH_SYST_CSR_TICKINT_MSK |
                           OS_ARCH_SYST_CSR_ENABLE_MSK;
    }
    }
#endif
}

/******************************************************************************************************/
/**
 * @brief Build the initial task stack frame for a newly created task.
 *
 * @param[in] stack_base   Base address of the caller-provided stack memory.
 * @param[in] stack_bytes  Size of the stack memory in bytes.
 * @param[in] entry        Task entry function.
 * @param[in] context      Task argument passed in R0.
 * @return uint32_t*       Initial process stack pointer for first restore, NULL on bad arguments.
 */
uint32_t* os_arch_task_stack_initialize(uint8_t *stack_base, size_t stack_bytes, void (*entry)(void *context), void *context)
{
    uint32_t *stack_top = NULL;

    if ((stack_base != NULL) && (entry != (void (*)(void *))0) &&
        (stack_bytes >= OS_CONFIG_MIN_STACK_SIZE))
    {
    /* The hardware exception frame must sit on an 8-byte aligned address. */
    stack_top = (uint32_t *)((uintptr_t)(stack_base + stack_bytes) & ~(uintptr_t)0x7U);

    /* Hardware frame restored by exception return. */
    *(--stack_top) = OS_ARCH_XPSR_THUMB;                    /* xPSR */
    *(--stack_top) = (uint32_t)(uintptr_t)entry;            /* PC   */
    *(--stack_top) = (uint32_t)(uintptr_t)os_arch_task_exit_trap; /* LR */
    *(--stack_top) = 0U;                                    /* R12  */
    *(--stack_top) = 0U;                                    /* R3   */
    *(--stack_top) = 0U;                                    /* R2   */
    *(--stack_top) = 0U;                                    /* R1   */
    *(--stack_top) = (uint32_t)(uintptr_t)context;          /* R0   */

    /* Software frame restored by the context-switch code. */
    *(--stack_top) = OS_ARCH_EXC_RETURN_THREAD_PSP;         /* EXC_RETURN */
    *(--stack_top) = 0U;                                    /* R11  */
    *(--stack_top) = 0U;                                    /* R10  */
    *(--stack_top) = 0U;                                    /* R9   */
    *(--stack_top) = 0U;                                    /* R8   */
    *(--stack_top) = 0U;                                    /* R7   */
    *(--stack_top) = 0U;                                    /* R6   */
    *(--stack_top) = 0U;                                    /* R5   */
    *(--stack_top) = 0U;                                    /* R4   */

    /* PSPLIM ignores its low 3 bits, so round the limit up: an overflow then
     * always faults before writing outside the caller's stack memory. */
    *(--stack_top) = ((uint32_t)(uintptr_t)stack_base + 7U) & ~(uint32_t)0x7U; /* PSPLIM */
    }

    return stack_top;
}

/******************************************************************************************************/
/**
 * @brief Read the free-running core cycle counter: DWT CYCCNT where the device implements it,
 *        the SysTick-derived counter otherwise.
 *
 * DWT and CYCCNT within it are both OPTIONAL on ARMv8-M, so assuming CYCCNT exists is not portable
 * across vendors - on a part without it, CYCCNT reads a constant and every busy-wait built on it
 * (os_delay_us, os_delay_ms below one tick) would spin forever. See os_arch_dwt_enable().
 *
 * @return uint32_t  Current cycle count.
 */
uint32_t os_arch_cycle_count_get(void)
{
    return os_arch_dwt_available ? OS_ARCH_REG_DWT_CYCCNT : os_arch_cycle_systick_get();
}

/******************************************************************************************************/
/**
 * @brief Only with DWT. Without it this port falls back to the counter synthesized from SysTick.
 *
 * @return bool  True when DWT CYCCNT is present and running.
 */
bool os_arch_cycle_is_independent(void)
{
    return os_arch_dwt_available;
}

/******************************************************************************************************/
/**
 * @brief Return elapsed ticks while in low-power mode, restoring SysTick's normal cadence.
 *
 * Detects whether the suppressed (planned-1)-tick window fully elapsed (a real SysTick
 * exception is then already pending, and supplies the final +1 through the ordinary
 * os_tick_handler() path once the mask this function releases lets it fire) or whether
 * some other interrupt woke the core early (elapsed cycles reconstructed from CVR).
 *
 * @return uint32_t  Elapsed ticks since os_arch_sleep_prepare(), 0 if it never armed a window.
 */
uint32_t os_arch_elapsed_ticks_get(void)
{
    uint32_t csr;
    uint32_t cvr;
    uint32_t elapsed_cycles;
    uint32_t elapsed_ticks = 0U;   /* no window was armed */

    if (os_arch_planned_idle_ticks != 0U)
    {
    /* Single CSR read: it clears COUNTFLAG as a side effect, so it must be sampled once. */
    csr = OS_ARCH_REG_SYST_CSR;

    if ((csr & OS_ARCH_SYST_CSR_COUNTFLAG_MSK) != 0U)
    {
        /* Full window elapsed: a real SysTick exception is already pending in the
         * NVIC (latched the instant the down-counter hit zero, independent of the
         * interrupt mask still held here) - it supplies the final +1 once the mask
         * below is released, through the unmodified os_tick_handler() ISR. */
        elapsed_ticks = os_arch_planned_idle_ticks - 1U;
    }
    else
    {
        /* Woke early: reconstruct how far CVR counted down from the reload
         * actually programmed for this window. */
        cvr            = OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK;
        elapsed_cycles = (os_arch_suppressed_reload_cycles - 1U) - cvr;

        /* Boundaries, not a plain cycles-to-ticks conversion. The window did not begin on a tick
         * boundary: the first one falls after os_arch_suppressed_head_cycles, and whole periods
         * follow it. Dividing the raw elapsed cycles instead would report a tick before the first
         * boundary was ever reached. */
        if ((elapsed_cycles >= os_arch_suppressed_head_cycles) && (os_arch_tick_reload_cycles != 0U))
        {
            elapsed_ticks = 1U + ((elapsed_cycles - os_arch_suppressed_head_cycles) /
                                  os_arch_tick_reload_cycles);
        }

        if (elapsed_ticks > (os_arch_planned_idle_ticks - 1U))
        {
            elapsed_ticks = os_arch_planned_idle_ticks - 1U;
        }
    }

    /* Restore SysTick to its normal single-tick cadence (identical values
     * os_arch_tick_init programs). Writing CVR clears COUNTFLAG and forces an
     * immediate reload from the now-normal RVR on the next clock; it does not
     * affect an already-latched pending exception in the NVIC, which is exactly
     * the point of the COUNTFLAG branch above. */
    OS_ARCH_REG_SYST_CSR = 0U;
    OS_ARCH_REG_SYST_RVR = os_arch_tick_reload_cycles - 1UL;
    OS_ARCH_REG_SYST_CVR = 0U;
    OS_ARCH_REG_SYST_CSR = OS_ARCH_SYST_CSR_CLKSOURCE_MSK |
                           OS_ARCH_SYST_CSR_TICKINT_MSK |
                           OS_ARCH_SYST_CSR_ENABLE_MSK;

    os_arch_planned_idle_ticks = 0U;
    }

    /* The kernel interrupt mask taken in os_arch_sleep_prepare stays held: os_arch_sleep_finish()
     * releases it once os_tick.c has announced this sleep and restored the application's hardware.
     * Releasing it here instead would expose a window in which os_tick_count is short by the
     * entire sleep duration while the pending SysTick and any other interrupt are free to run. */
    return elapsed_ticks;
}

/******************************************************************************************************/
/**
 * @brief Release the interrupt mask held across the tickless window. See os_arch_port_common.h.
 *
 * @return None.
 */
void os_arch_sleep_finish(void)
{
    /* os_arch_sleep_prepare only masks once it commits to reprogramming SysTick; every early
     * return leaves the mask untaken. Restoring unconditionally would then push a stale saved
     * state onto a core that was never masked, so the flag tracks ownership explicitly. */
    if (os_arch_sleep_mask_held)
    {
        os_arch_sleep_mask_held = false;
        os_arch_kernel_mask_restore(os_arch_sleep_mask_state);
    }
}

/******************************************************************************************************/
/**
 * @brief Ticks that fit in one suppressed window given the register width (24-bit SysTick
 *        reload) and the current tick-to-cycle ratio. Shared by os_arch_sleep_prepare (the cap)
 *        and os_arch_max_suppressed_ticks_get (the public query) so the two can never disagree.
 *
 * @return uint64_t  Maximum ticks per window, 0 if the normal tick was never set up.
 */
static uint64_t os_arch_max_window_ticks_get(void)
{
    uint64_t window = 0U;   /* the normal tick was never set up */

    if (os_arch_tick_reload_cycles != 0U)
    {
        window = (uint64_t)(OS_ARCH_SYST_RVR_RELOAD_MSK + 1UL) /
                 (uint64_t)os_arch_tick_reload_cycles;
    }

    return window;
}


/******************************************************************************************************/
/**
 * @brief Reprogram SysTick to suppress ticking for (planned_ticks - 1) ticks and mask
 *        interrupts until os_arch_elapsed_ticks_get() restores normal cadence.
 *
 * TICKINT stays enabled throughout: if this window fully elapses, the SysTick exception
 * legitimately becomes pending exactly like a normal tick would, and os_arch_elapsed_ticks_get()
 * relies on that pending exception to supply the final tick once it releases the mask below -
 * The established technique for this, rather than a self-contained alternative.
 *
 * @param[in] planned_ticks  Planned idle duration in kernel ticks.
 * @return None.
 */
void os_arch_sleep_prepare(uint32_t planned_ticks)
{
    uint32_t clock_hz;
    uint32_t remaining_cycles;
    uint64_t max_window_ticks;
    uint64_t suppressed_cycles64;

    os_arch_planned_idle_ticks = 0U; /* not armed until proven below */

    /* Below 2 ticks there is nothing meaningful to suppress (planned_ticks - 1
     * would be 0); os_arch_elapsed_ticks_get() then reports 0 and this idle
     * pass behaves like a plain WFI. */
    if (planned_ticks >= 2U)
    {
    clock_hz = os_arch_clock_hz_get();

    /* No usable clock, or the normal tick was never actually set up
     * (os_arch_tick_init bailed at boot): nothing safe to reprogram. */
    if ((clock_hz != 0U) && (os_arch_tick_reload_cycles != 0U))
    {

    /* Cap the TICK COUNT first, then re-derive the cycle budget from the capped
     * count: (planned_ticks - 1) * reload_cycles can vastly exceed uint32_t
     * range for realistic tick counts, so capping after multiplying would
     * overflow. One tick of headroom is left because the remainder below is
     * added on top and can be almost a whole reload by itself. */
    max_window_ticks = os_arch_max_window_ticks_get();

    /* Zero is defensive only: unreachable given the reload range os_arch_tick_init enforces. */
    if (max_window_ticks > 1U)
    {
    if ((uint64_t)(planned_ticks - 1U) > (max_window_ticks - 1U))
    {
        planned_ticks = (uint32_t)max_window_ticks;
    }

    /* Whatever is left of the tick ALREADY RUNNING, kept rather than discarded.
     *
     * The deadline the kernel asked for is planned_ticks tick BOUNDARIES away, and the first of
     * them is this remainder away - not a whole period. Zeroing CVR here, as this once did,
     * silently shortens every window to planned_ticks - 1 periods while os_arch_elapsed_ticks_get
     * still reports planned_ticks, so the clock gains a tick per window. A single sleep looks
     * right; twenty of them are twenty ticks fast. */
    remaining_cycles = OS_ARCH_REG_SYST_CVR & OS_ARCH_SYST_RVR_RELOAD_MSK;

    suppressed_cycles64 = (uint64_t)remaining_cycles +
                          ((uint64_t)(planned_ticks - 1U) * (uint64_t)os_arch_tick_reload_cycles);

    /* Committed to reprogramming SysTick: hold the kernel interrupt mask until
     * os_arch_elapsed_ticks_get() restores normal cadence and releases it, so a
     * real tick can never fire against a half-reprogrammed register set. */
    os_arch_sleep_mask_state          = os_arch_kernel_mask_save();
    os_arch_sleep_mask_held           = true;
    os_arch_planned_idle_ticks        = planned_ticks;
    os_arch_suppressed_reload_cycles  = (uint32_t)suppressed_cycles64;
    os_arch_suppressed_head_cycles    = remaining_cycles;

    OS_ARCH_REG_SYST_CSR = 0U;
    OS_ARCH_REG_SYST_RVR = os_arch_suppressed_reload_cycles - 1UL;
    OS_ARCH_REG_SYST_CVR = 0U;
    OS_ARCH_REG_SYST_CSR = OS_ARCH_SYST_CSR_CLKSOURCE_MSK |
                           OS_ARCH_SYST_CSR_TICKINT_MSK |
                           OS_ARCH_SYST_CSR_ENABLE_MSK;
    }
    }
    }
}

/******************************************************************************************************/
/**
 * @brief Maximum ticks this port can suppress in a single tickless window (see
 *        os_arch_port_common.h for the full contract) - the planned_ticks value at which
 *        os_arch_sleep_prepare's own cap first binds.
 *
 * @return uint32_t  Maximum suppressible ticks, 0 if the normal tick was never set up.
 */
uint32_t os_arch_max_suppressed_ticks_get(void)
{
    /* No suppression without a cycle counter of its own.
     *
     * Where DWT CYCCNT is missing or gated, os_arch_cycle_count_get falls back to a counter
     * SYNTHESIZED FROM SysTick: it accumulates whole periods in the tick interrupt and multiplies
     * them by the reload it reads live. Suppressing the tick changes that reload, so periods
     * counted against the old one get scaled by the new one and the value jumps - and that counter
     * is what os_delay_us and the busy-wait half of os_delay_ms run on.
     *
     * DWT is independent of SysTick, so with it there is nothing to disturb. Without it, this port
     * reports 0 and tickless degrades to a plain WFI, which is correct rather than merely safe:
     * the alternative is a suppressed window that quietly breaks every microsecond delay. */
    uint32_t suppressible = 0U;

    if (os_arch_dwt_available)
    {
        uint64_t max_window_ticks = os_arch_max_window_ticks_get();

        suppressible = (max_window_ticks == 0U) ? 0U : ((uint32_t)max_window_ticks + 1U);
    }

    return suppressible;
}

/*
 * ***********************************************************************************************************
 * TrustZone context-switch glue
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
/******************************************************************************************************/
/**
 * @brief Bank the outgoing task's secure context; called from the PendSV handler while
 *        os_task_current still names that task.
 *
 * @return None.
 */
void os_arch_tz_context_save(void)
{
    os_arch_tz_context_save_cb(os_task_current_id_get());
}

/******************************************************************************************************/
/**
 * @brief Restore the incoming task's secure context; called once the scheduler has selected it.
 *
 * @return None.
 */
void os_arch_tz_context_restore(void)
{
    os_arch_tz_context_restore_cb(os_task_current_id_get());
}
#endif /* OS_CONFIG_TRUSTZONE_NON_SECURE */

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Enable DWT CYCCNT and report whether it is genuinely usable on this device.
 *
 * Three ways it can be absent, all of them normal parts rather than exotic ones, and none of them
 * detectable from the core type alone:
 *
 *   1. DWT_CTRL.NOCYCCNT reads 1        The cycle counter is not implemented. Architecturally
 *                                       optional on every core this port covers.
 *   2. The enable does not stick        DWT is behind the debug power domain on some devices and
 *                                       reads back unchanged until a debugger powers it up.
 *   3. It is enabled but does not run   Same cause, seen later: the register accepts the write
 *                                       yet the counter never advances.
 *
 * All three are checked, in that order, because each is cheaper than the next. The last needs an
 * actual observation, so it spends a bounded handful of loop iterations looking for the counter to
 * move - once, at boot.
 *
 * @return bool  true when CYCCNT is implemented, enabled and counting.
 */
static bool os_arch_dwt_enable(void)
{
    uint32_t control = OS_ARCH_REG_DWT_CTRL;
    uint32_t first_sample;
    uint32_t attempt;

    bool counting = false;

    /* Not implemented, or the enable was refused because the debug power domain is down. */
    if ((control & OS_ARCH_DWT_CTRL_NOCYCCNT_MSK) == 0U)
    {
        OS_ARCH_REG_DWT_CYCCNT = 0U;
        OS_ARCH_REG_DWT_CTRL   = control | OS_ARCH_DWT_CTRL_CYCCNTENA_MSK;

        if ((OS_ARCH_REG_DWT_CTRL & OS_ARCH_DWT_CTRL_CYCCNTENA_MSK) != 0U)
        {
            /* Confirm it counts. The bound is generous next to the handful of cycles a working
             * counter needs to move, and it runs exactly once, so the cost is invisible against
             * boot. `counting` ends the loop through its own condition. */
            first_sample = OS_ARCH_REG_DWT_CYCCNT;

            for (attempt = 0U; (attempt < 64U) && !counting; attempt++)
            {
                counting = (OS_ARCH_REG_DWT_CYCCNT != first_sample);
            }
        }
    }

    return counting;
}

/******************************************************************************************************/
/**
 * @brief Landing point when a task entry function returns; deletes the task.
 *
 * @return None.
 */
static void os_arch_task_exit_trap(void)
{
    os_task_exit();

    /* os_task_exit never returns; trap just in case. */
    while (1)
    {
        __asm volatile("bkpt #0");
    }
}
