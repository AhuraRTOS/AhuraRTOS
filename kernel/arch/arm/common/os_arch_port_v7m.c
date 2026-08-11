/**
 * @file os_arch_port_v7m.c
 * @brief Shared port implementation for ARMv7-M (Cortex-M3) and ARMv7E-M
 *        (Cortex-M4, M7) cores. Thumb-2, FPU support is compile-time
 *        conditional (saves s16-s31 and a per-task EXC_RETURN when built
 *        with a hard/softfp float ABI).
 *
 * This file is textually included by each variant's os_arch_port.c wrapper.
 * ARMv8-M mainline / ARMv8.1-M cores use os_arch_port_v8m.c, which extends
 * this implementation with PSPLIM/MSPLIM stack limits and TrustZone.
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

#if !defined(__ARM_ARCH_7M__) && !defined(__ARM_ARCH_7EM__)
#error "os_arch_port_v7m.c targets ARMv7-M / ARMv7E-M cores (check -mcpu / -march)."
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
 * so each task carries its own frame type across switches.
 */
#define OS_ARCH_EXC_RETURN_THREAD_PSP        0xFFFFFFFDUL

#define OS_ARCH_CONTROL_FPCA_MSK             (1UL << 2)

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint32_t os_arch_sleep_entry_cycles = 0U;
static uint32_t os_arch_planned_idle_ticks = 0U;

/* Whether DWT CYCCNT is present and actually counting on this device; decided
 * once in os_arch_init(). False routes os_arch_cycle_count_get() to the
 * SysTick-derived counter instead. */
static bool     os_arch_dwt_available      = false;

/*
 * ***********************************************************************************************************
 * Context switch handler (PendSV does everything)
 * ***********************************************************************************************************
 *
 * Software-saved frame layout on a task stack (low address first):
 *   [ s16-s31 ]  only when the task was using the FPU (EXC_RETURN bit 4 clear)
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
 * finds a real PSP and performs an ordinary switch.
 *
 * That sentinel is also why SVC is not used. Starting the first task through
 * "svc 0" is the traditional Cortex-M approach - it is what FreeRTOS does -
 * but it costs the kernel a second exception vector permanently, in exchange
 * for one action performed once at boot. SVC is the most contended vector on
 * the architecture: Nordic's SoftDevice reserves part of the SVC number space,
 * TF-M and other secure firmware use SVC for gateway calls, vendor bootloaders
 * and ROM APIs use it, and vendor IDEs generate a default SVC_Handler into
 * every project. A handler that decodes no SVC immediate - which is all a
 * "start the first task" handler needs - can share it with none of them, and
 * would additionally re-enter scheduling on any stray SVC later in the run.
 * Folding the boot path into PendSV leaves SVC entirely to the application.
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
"    stmdb   r0!, {r4-r11, lr}\n"
"    bl      os_task_stack_save_current\n" /* r0 = stack pointer of outgoing task */
"    bl      os_task_stack_select_next\n"  /* r0 = stack pointer of incoming task */
"    b       os_arch_context_restore_asm\n"

"1:\n"                                     /* first start: nothing to save */
"    bl      os_task_stack_select_next\n"  /* r0 = first task stack pointer */
"    movw    r1, #0xED08\n"                /* reset MSP to the vector-table initial value; */
"    movt    r1, #0xE000\n"                /* the boot (main) context is abandoned here,   */
"    ldr     r1, [r1]\n"                   /* including the frame this exception pushed -  */
"    ldr     r1, [r1]\n"                   /* the return below unstacks from PSP instead   */
"    msr     msp, r1\n"
"    b       os_arch_context_restore_asm\n"

".global os_arch_context_restore_asm\n"
".type   os_arch_context_restore_asm, %function\n"
".thumb_func\n"
"os_arch_context_restore_asm:\n"           /* r0 = stack pointer of task to restore */
"    clrex\n"                              /* drop any LDREX reservation the outgoing task left */
"    ldmia   r0!, {r4-r11, lr}\n"
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
     * CoreSight software lock (Cortex-M7); it is ignored elsewhere. */
    OS_ARCH_REG_DEMCR   |= OS_ARCH_DEMCR_TRCENA_MSK;
    OS_ARCH_REG_DWT_LAR  = OS_ARCH_DWT_LAR_UNLOCK_KEY;

    os_arch_dwt_available = os_arch_dwt_enable();

    os_arch_cycle_systick_reset();
    os_arch_sleep_entry_cycles = os_arch_cycle_count_get();
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
    /* The application owns the tick hardware; the port programs nothing. */
    os_arch_tick_init_cb();
#else
    uint32_t clock_hz = os_arch_clock_hz_get();
    uint32_t reload_value;

    if ((clock_hz == 0U) || (OS_CONFIG_TICK_HZ == 0U))
    {
        return;
    }

    reload_value = (clock_hz / OS_CONFIG_TICK_HZ);
    if ((reload_value == 0U) || (reload_value > (OS_ARCH_SYST_RVR_RELOAD_MSK + 1UL)))
    {
        return;
    }

    OS_ARCH_REG_SYST_CSR = 0U;
    OS_ARCH_REG_SYST_RVR = reload_value - 1UL;
    OS_ARCH_REG_SYST_CVR = 0U;
    OS_ARCH_REG_SYST_CSR = OS_ARCH_SYST_CSR_CLKSOURCE_MSK |
                           OS_ARCH_SYST_CSR_TICKINT_MSK |
                           OS_ARCH_SYST_CSR_ENABLE_MSK;
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
    uint32_t *stack_top;

    if ((stack_base == NULL) || (entry == (void (*)(void *))0) || (stack_bytes < OS_CONFIG_MIN_STACK_SIZE))
    {
        return NULL;
    }

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

    return stack_top;
}

/******************************************************************************************************/
/**
 * @brief Read the free-running core cycle counter: DWT CYCCNT where the device implements it,
 *        the SysTick-derived counter otherwise.
 *
 * DWT is an OPTIONAL unit on Cortex-M3/M4/M7, and CYCCNT is optional even within it, so assuming
 * it exists is not portable across vendors - on a part without it, CYCCNT reads a constant and
 * every busy-wait built on it (os_delay_us, os_delay_ms below one tick) would spin forever.
 * See os_arch_dwt_enable().
 *
 * @return uint32_t  Current cycle count.
 */
uint32_t os_arch_cycle_count_get(void)
{
    if (os_arch_dwt_available)
    {
        return OS_ARCH_REG_DWT_CYCCNT;
    }

    return os_arch_cycle_systick_get();
}

/******************************************************************************************************/
/**
 * @brief Return elapsed ticks while in low-power mode.
 *
 * @return uint32_t  Elapsed ticks since sleep entry.
 */
uint32_t os_arch_elapsed_ticks_get(void)
{
    /* Always 0 on this port, and deliberately so.
     *
     * os_arch_max_suppressed_ticks_get() returns 0 here: ticking is never suppressed, so the WFI
     * runs with SysTick still counting and its interrupt still enabled, and the interrupt that
     * ends the sleep is normally the very next tick. Every tick that passed was therefore already
     * counted, one at a time, by os_tick_handler().
     *
     * Returning a measured duration as well would have os_tick.c announce time the ISR had just
     * accounted for, advancing os_tick_count at roughly twice real time for the whole idle period
     * - delays and timers would then fire early in proportion to how long the system sat idle. A
     * suppression-capable port (v8m) returns a real figure precisely because there the ISR did not
     * run. Measuring the sleep only becomes meaningful here once this port learns to reprogram
     * SysTick, at which point os_arch_sleep_prepare's recorded entry cycle count is what it needs.
     */
    os_arch_planned_idle_ticks = 0U;

    return 0U;
}

/******************************************************************************************************/
/**
 * @brief Close the tickless window. Nothing to release on this port.
 *
 * @return None.
 */
void os_arch_sleep_finish(void)
{
    /* os_arch_sleep_prepare takes no interrupt mask here: a plain WFI needs interrupts left
     * enabled in order to wake at all, so there is nothing to hand back. */
}


/******************************************************************************************************/
/**
 * @brief Record low-power entry context for elapsed tick accounting.
 *
 * @param[in] planned_ticks  Planned idle duration in kernel ticks.
 * @return None.
 */
void os_arch_sleep_prepare(uint32_t planned_ticks)
{
    os_arch_planned_idle_ticks = planned_ticks;
    os_arch_sleep_entry_cycles = os_arch_cycle_count_get();
}

/******************************************************************************************************/
/**
 * @brief Maximum ticks this port can suppress in a single tickless window (see
 *        os_arch_port_common.h for the full contract).
 *
 * This port does not yet reprogram SysTick's reload for real suppression (see os_arch_port_v8m.c,
 * which does) - os_arch_sleep_prepare/os_arch_elapsed_ticks_get above still measure via the DWT
 * cycle counter around a plain WFI, so there is no register-width-limited window to report here.
 * 0 tells callers (os_tick.c, tests) not to expect a real suppressed sleep on this port yet.
 *
 * @return uint32_t  Always 0 until this port gets the same fix as os_arch_port_v8m.c.
 */
uint32_t os_arch_max_suppressed_ticks_get(void)
{
    return 0U;
}

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

    if ((control & OS_ARCH_DWT_CTRL_NOCYCCNT_MSK) != 0U)
    {
        return false; /* not implemented */
    }

    OS_ARCH_REG_DWT_CYCCNT = 0U;
    OS_ARCH_REG_DWT_CTRL   = control | OS_ARCH_DWT_CTRL_CYCCNTENA_MSK;

    if ((OS_ARCH_REG_DWT_CTRL & OS_ARCH_DWT_CTRL_CYCCNTENA_MSK) == 0U)
    {
        return false; /* enable refused (debug power domain down) */
    }

    /* Confirm it counts. The bound is generous next to the handful of cycles a working counter
     * needs to move, and it runs exactly once, so the cost is invisible against boot. */
    first_sample = OS_ARCH_REG_DWT_CYCCNT;

    for (attempt = 0U; attempt < 64U; attempt++)
    {
        if (OS_ARCH_REG_DWT_CYCCNT != first_sample)
        {
            return true;
        }
    }

    return false;
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
