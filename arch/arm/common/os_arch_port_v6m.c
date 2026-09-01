/**
 * @file os_arch_port_v6m.c
 * @brief Shared port for ARMv6-M (Cortex-M0, M0+) and ARMv8-M baseline (Cortex-M23): Thumb-1
 *        subset only, no FPU, no DWT (the cycle counter is synthesized from SysTick), and no
 *        PSPLIM - non-secure ARMv8-M baseline has no stack-limit registers.
 *
 * Textually included by each variant's os_arch_port.c wrapper. TrustZone (Cortex-M23 only, via
 * OS_CONFIG_TRUSTZONE) may be disabled, secure, or non-secure - in which case the context switch
 * banks per-task secure state through the tz_context callbacks and the initial frames use the
 * non-secure EXC_RETURN.
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

#if !defined(__ARM_ARCH_6M__) && !defined(__ARM_ARCH_8M_BASE__)
#error "os_arch_port_v6m.c targets ARMv6-M / ARMv8-M baseline cores (check -mcpu / -march)."
#endif

/* This port's only cycle counter: ARMv6-M has no DWT. Textual include, like
 * this file itself. */
#include "os_arch_cycle_systick.c"

/* The atomic operation set, shared by all three ports: its backend follows the
 * instruction set (OS_ARCH_ATOMIC_LOCK_FREE), not the v6m/v7m/v8m split - which
 * is why ARMv8-M baseline lands on the critical-section one here. Textual
 * include as well. */
#include "os_arch_atomic.c"
#include "os_arch_tickless.c"

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_ARCH_REG_SHPR3                    (*(__IO uint32_t *)0xE000ED20UL)

/* SVCall's priority field (SHPR2) is deliberately absent: the kernel does not
 * use SVC, so it has no business changing that exception's priority. */
#define OS_ARCH_SHPR3_PENDSV_PRI_POS         16U
#define OS_ARCH_SHPR3_SYSTICK_PRI_POS        24U

#define OS_ARCH_PRIORITY_LOWEST              255U
#define OS_ARCH_XPSR_THUMB                   (1UL << 24)

/*
 * EXC_RETURN for the initial task frame: return to thread mode, use PSP.
 * ARMv6-M has no FPU, so no frame-type handling is needed, but storing
 * EXC_RETURN keeps the frame layout identical to the mainline port. A
 * non-secure TrustZone kernel (v8-M baseline) returns with the S and ES bits
 * clear (0xFFFFFFBC); the secure and TrustZone-less encodings are both
 * 0xFFFFFFFD.
 */
#if (OS_CONFIG_TRUSTZONE == OS_CONFIG_TRUSTZONE_NON_SECURE)
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFBCUL
#else
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFFDUL
#endif

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint32_t os_arch_sleep_entry_cycles = 0U;
static uint32_t os_arch_planned_idle_ticks = 0U;

/*
 * ***********************************************************************************************************
 * Context switch handler (PendSV does everything)
 * ***********************************************************************************************************
 *
 * Software-saved frame layout on a task stack (low address first):
 *   r4-r11, EXC_RETURN
 *   [ hardware frame: r0-r3, r12, lr, pc, xpsr ]
 *
 * Same layout as the mainline port, built with the Thumb-1 subset: high
 * registers are staged through r4-r7 because ARMv6-M LDM/STM only address
 * low registers, and there is no CBZ/IT/MOVW/MOVT.
 *
 * PendSV is the ONLY exception this kernel takes over, and the PSP == 0
 * sentinel is what lets one handler serve both jobs: zero means no task has
 * run yet, so there is no outgoing context to save and the handler simply
 * installs the first task (the "first start" path below). Every later entry
 * finds a real PSP and performs an ordinary switch. See os_arch_port_v7m.c's
 * equivalent block for why SVC is deliberately left to the application.
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
"    cmp     r0, #0\n"                     /* PSP == 0: no task has run yet, go start the first */
"    beq     os_arch_first_start\n"
"    subs    r0, r0, #36\n"                /* reserve r4-r11 + EXC_RETURN (9 words) */
"    stmia   r0!, {r4-r7}\n"               /* save r4-r7 */
"    mov     r4, r8\n"                     /* stage and save r8-r11 */
"    mov     r5, r9\n"
"    mov     r6, r10\n"
"    mov     r7, r11\n"
"    stmia   r0!, {r4-r7}\n"
"    mov     r4, lr\n"                     /* save EXC_RETURN */
"    stmia   r0!, {r4}\n"
"    subs    r0, r0, #36\n"                /* r0 = base of the software frame */
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

"os_arch_first_start:\n"                   /* first start: nothing to save */
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
"    ldr     r1, os_arch_vtor_addr\n"      /* is exactly this core's stack, and always was.      */
"    ldr     r1, [r1]\n"                   /* the boot (main) context is abandoned here, including */
"    ldr     r1, [r1]\n"                   /* the frame this exception pushed - the return        */
"    msr     msp, r1\n"                    /* below unstacks from PSP instead                     */
#endif
"    b       os_arch_context_restore_asm\n"
".align 2\n"
"os_arch_vtor_addr:\n"                     /* VTOR reads as zero on cores without it, which is */
"    .word   0xE000ED08\n"                 /* the fixed table address anyway                   */

".global os_arch_context_restore_asm\n"
".type   os_arch_context_restore_asm, %function\n"
".thumb_func\n"
"os_arch_context_restore_asm:\n"           /* r0 = stack pointer of task to restore */
"    mov     r1, r0\n"                     /* keep frame base for the r4-r7 reload */
"    adds    r0, r0, #16\n"
"    ldmia   r0!, {r4-r7}\n"               /* stage and restore r8-r11 */
"    mov     r8, r4\n"
"    mov     r9, r5\n"
"    mov     r10, r6\n"
"    mov     r11, r7\n"
"    ldmia   r0!, {r2}\n"                  /* restore EXC_RETURN */
"    mov     lr, r2\n"
"    mov     r2, r0\n"                     /* r2 = new PSP (frame base + 36) */
"    mov     r0, r1\n"
"    ldmia   r0!, {r4-r7}\n"               /* restore the task's real r4-r7 */
"    msr     psp, r2\n"
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
void os_arch_init(void)
{
    uint32_t shpr3 = OS_ARCH_REG_SHPR3;

    /* Before anything else: confirm the vector table really routes PendSV
     * here. Everything below assumes the kernel owns that exception, and a
     * table that does not is a silent hang rather than a fault. */
    os_arch_vector_check(OS_CONFIG_ARCH_PENDSV_HANDLER);

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
     * application set it: the kernel does not use SVC.
     *
     * Both fields are merged into one local and stored once below, because ARMv6-M requires WORD
     * access to the SHPR registers - there is no byte-wide write to reach a single field. */
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

    os_arch_cycle_systick_reset();
    os_arch_sleep_entry_cycles = 0U;
    os_arch_planned_idle_ticks = 0U;
}

/******************************************************************************************************/
/**
 * @brief Start the first task context. Does not return.
 *
 * @return None.
 */
void os_arch_start_first_task(void)
{
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
    /* The application owns the tick hardware; the port programs nothing. */
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
    }

    return stack_top;
}

/******************************************************************************************************/
/**
 * @brief Read the free-running core cycle counter, synthesized from SysTick.
 *
 * ARMv6-M has no DWT cycle counter, so the SysTick-derived one is all there is here - see
 * os_arch_cycle_systick.c for what it guarantees. The mainline ports use the same code as their
 * fallback when DWT CYCCNT turns out to be unavailable.
 *
 * @return uint32_t  Current cycle count.
 */
uint32_t os_arch_cycle_count_get(void)
{
    return os_arch_cycle_systick_get();
}

/******************************************************************************************************/
/**
 * @brief Never: ARMv6-M has no DWT at all, so the counter is always the synthesized one.
 *
 * @return bool  False.
 */
bool os_arch_cycle_is_independent(void)
{
    return false;
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
