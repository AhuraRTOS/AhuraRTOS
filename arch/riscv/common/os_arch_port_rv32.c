/**
 * @file os_arch_port_rv32.c
 * @brief Shared RV32 port implementation: context switch, task frame, trap entry, atomics.
 *
 * Included textually by each core folder's os_arch_port.c, exactly as the ARM ports are - so this
 * file is never added to a build directly. See arch/riscv/hazard3/os_arch_port.c.
 *
 * WHAT DIFFERS FROM THE ARM PORTS, AND WHY
 *
 * RISC-V stacks NOTHING on a trap. Cortex-M pushes r0-r3/r12/lr/pc/xPSR itself, so its PendSV
 * handler only has to add the callee-saved half. Here the trap entry is responsible for every
 * register the interrupted task could have been using, which is why the frame below is 30 words
 * rather than 17, and why the handler is written as one naked block instead of a few instructions.
 *
 * There is no second stack pointer. Cortex-M's MSP/PSP split gives the ARM port a free "no task has
 * run yet" sentinel (PSP == 0) and a handler that serves both the first start and every later
 * switch. RISC-V has one sp, so the two jobs are two functions here: os_arch_start_first_task()
 * restores a frame and mrets without ever taking a trap, and the software-interrupt handler always
 * performs a real switch. That is simpler than the ARM arrangement, not a workaround for it.
 *
 * There is no FPU context. The Pico SDK builds RISC-V with -mabi=ilp32 and no F extension, so there
 * are no floating-point registers to bank - the lazy-stacking dance the ARMv8-M port performs has
 * nothing to do here.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#include "os_arch_port.h"
#include "ahura.h"

/*
 * ***********************************************************************************************************
 * The task frame
 * ***********************************************************************************************************
 *
 * Laid down by os_arch_task_stack_initialize() and consumed by the restore path below; the two must
 * agree exactly, so the offsets live here as names rather than as numbers in two places.
 *
 * 30 registers, padded to 32 words so the frame is a multiple of the 16-byte stack alignment the
 * ilp32 ABI requires at every call boundary.
 *
 * gp (x3) and tp (x4) are deliberately absent. Both are set once by the startup code and are the
 * same for every task in a single-address-space kernel, so saving them per switch would cost two
 * words and two accesses to preserve a value that cannot differ. A port that ever grows per-task
 * thread-local storage would add tp here and nowhere else.
*/

#define OS_ARCH_FRAME_MSTATUS   0
#define OS_ARCH_FRAME_MEPC      1
#define OS_ARCH_FRAME_RA        2
#define OS_ARCH_FRAME_T0        3
#define OS_ARCH_FRAME_T1        4
#define OS_ARCH_FRAME_T2        5
#define OS_ARCH_FRAME_S0        6
#define OS_ARCH_FRAME_S1        7
#define OS_ARCH_FRAME_A0        8
#define OS_ARCH_FRAME_A1        9
#define OS_ARCH_FRAME_A2       10
#define OS_ARCH_FRAME_A3       11
#define OS_ARCH_FRAME_A4       12
#define OS_ARCH_FRAME_A5       13
#define OS_ARCH_FRAME_A6       14
#define OS_ARCH_FRAME_A7       15
#define OS_ARCH_FRAME_S2       16
#define OS_ARCH_FRAME_S3       17
#define OS_ARCH_FRAME_S4       18
#define OS_ARCH_FRAME_S5       19
#define OS_ARCH_FRAME_S6       20
#define OS_ARCH_FRAME_S7       21
#define OS_ARCH_FRAME_S8       22
#define OS_ARCH_FRAME_S9       23
#define OS_ARCH_FRAME_S10      24
#define OS_ARCH_FRAME_S11      25
#define OS_ARCH_FRAME_T3       26
#define OS_ARCH_FRAME_T4       27
#define OS_ARCH_FRAME_T5       28
#define OS_ARCH_FRAME_T6       29

#define OS_ARCH_FRAME_WORDS    32
#define OS_ARCH_FRAME_BYTES    (OS_ARCH_FRAME_WORDS * 4)

/* mstatus.MPIE (bit 7) and mstatus.MPP (bits 12:11).
 *
 * mret copies MPIE into MIE and returns to the privilege level in MPP, so the value a task's frame
 * carries in mstatus is what decides whether it runs with interrupts on. A new task gets MPIE set -
 * anything else would start it with interrupts masked and never turn them back on - and MPP = 3
 * (machine mode), which is the only mode this kernel runs in. */
#define OS_ARCH_MSTATUS_MPIE_MSK   (1UL << 7)
#define OS_ARCH_MSTATUS_MPP_M      (3UL << 11)

/*
 * Trap nesting depth, per core. RISC-V has no IPSR to ask, so the port keeps the answer; see
 * os_arch_in_isr().
 */
__IO uint32_t os_arch_isr_nesting[OS_CONFIG_CORE_COUNT];

/*
 * ***********************************************************************************************************
 * Context switch
 * ***********************************************************************************************************
 *
 * Entered directly from mtvec slot 3 (trap cause 3, the machine software interrupt) with interrupts
 * already masked by hardware - mstatus.MIE is cleared on trap entry and the previous value banked
 * in MPIE, so the save below cannot be interrupted.
 *
 * The trap runs on the interrupted task's own stack. That is the natural arrangement with a single
 * stack pointer, and it is why every task's stack must have room for one frame on top of whatever
 * the task itself uses - os_task.c already budgets for exactly that.
*/

__asm(
".pushsection " OS_CONFIG_ARCH_SWI_SECTION ", \"ax\"\n"
".align 2\n"

".global " OS_ARCH_STRINGIFY(OS_CONFIG_ARCH_SWI_HANDLER) "\n"
".type   " OS_ARCH_STRINGIFY(OS_CONFIG_ARCH_SWI_HANDLER) ", %function\n"
OS_ARCH_STRINGIFY(OS_CONFIG_ARCH_SWI_HANDLER) ":\n"

"    addi    sp, sp, -128\n"               /* one frame; must match OS_ARCH_FRAME_BYTES */

"    sw      ra,   8(sp)\n"                /* caller-saved first: the C calls below clobber them */
"    sw      t0,  12(sp)\n"
"    sw      t1,  16(sp)\n"
"    sw      t2,  20(sp)\n"
"    sw      s0,  24(sp)\n"
"    sw      s1,  28(sp)\n"
"    sw      a0,  32(sp)\n"
"    sw      a1,  36(sp)\n"
"    sw      a2,  40(sp)\n"
"    sw      a3,  44(sp)\n"
"    sw      a4,  48(sp)\n"
"    sw      a5,  52(sp)\n"
"    sw      a6,  56(sp)\n"
"    sw      a7,  60(sp)\n"
"    sw      s2,  64(sp)\n"
"    sw      s3,  68(sp)\n"
"    sw      s4,  72(sp)\n"
"    sw      s5,  76(sp)\n"
"    sw      s6,  80(sp)\n"
"    sw      s7,  84(sp)\n"
"    sw      s8,  88(sp)\n"
"    sw      s9,  92(sp)\n"
"    sw      s10, 96(sp)\n"
"    sw      s11,100(sp)\n"
"    sw      t3, 104(sp)\n"
"    sw      t4, 108(sp)\n"
"    sw      t5, 112(sp)\n"
"    sw      t6, 116(sp)\n"

"    csrr    t0, mstatus\n"                /* the return state, banked by the trap */
"    csrr    t1, mepc\n"
"    sw      t0,   0(sp)\n"
"    sw      t1,   4(sp)\n"

/* Acknowledge the request at the platform. mip.MSIP is read-only to software and stays asserted
 * until the chip's own register is written, so skipping this re-enters the trap forever. */
"    call    os_arch_swi_clear_cb\n"

"    mv      a0, sp\n"                     /* a0 = outgoing task's stack pointer */
"    call    os_task_stack_save_current\n"
"    call    os_task_stack_select_next\n"  /* a0 = incoming task's stack pointer */
"    mv      sp, a0\n"

".global os_arch_context_restore_asm\n"
".type   os_arch_context_restore_asm, %function\n"
"os_arch_context_restore_asm:\n"           /* sp = frame of the task to resume */

"    lw      t0,   0(sp)\n"
"    lw      t1,   4(sp)\n"
"    csrw    mstatus, t0\n"
"    csrw    mepc, t1\n"

"    lw      ra,   8(sp)\n"
"    lw      t0,  12(sp)\n"
"    lw      t1,  16(sp)\n"
"    lw      t2,  20(sp)\n"
"    lw      s0,  24(sp)\n"
"    lw      s1,  28(sp)\n"
"    lw      a0,  32(sp)\n"
"    lw      a1,  36(sp)\n"
"    lw      a2,  40(sp)\n"
"    lw      a3,  44(sp)\n"
"    lw      a4,  48(sp)\n"
"    lw      a5,  52(sp)\n"
"    lw      a6,  56(sp)\n"
"    lw      a7,  60(sp)\n"
"    lw      s2,  64(sp)\n"
"    lw      s3,  68(sp)\n"
"    lw      s4,  72(sp)\n"
"    lw      s5,  76(sp)\n"
"    lw      s6,  80(sp)\n"
"    lw      s7,  84(sp)\n"
"    lw      s8,  88(sp)\n"
"    lw      s9,  92(sp)\n"
"    lw      s10, 96(sp)\n"
"    lw      s11,100(sp)\n"
"    lw      t3, 104(sp)\n"
"    lw      t4, 108(sp)\n"
"    lw      t5, 112(sp)\n"
"    lw      t6, 116(sp)\n"

"    addi    sp, sp, 128\n"
"    mret\n"".popsection\n"
);

/* Declared through the configured name so the boot-time vector check compares against exactly the
 * symbol the vector table is expected to reference. */
extern void OS_CONFIG_ARCH_SWI_HANDLER(void);

/*
 * ***********************************************************************************************************
 * First start
 * ***********************************************************************************************************
 *
 * Not a trap. mret outside a trap is well defined - it sets pc from mepc and restores MIE from MPIE
 * - which is exactly the "become a task" step, so the first start is an ordinary function that
 * never returns rather than a second personality bolted onto the switch handler.
 *
 * The boot context (whatever stack main() was using) is abandoned here, deliberately: every core
 * that reaches this point is committing to run tasks and will never unwind back out.
*/

__asm(
".pushsection " OS_CONFIG_ARCH_SWI_SECTION ", \"ax\"\n"
".align 2\n"
".global os_arch_start_first_task\n"
".type   os_arch_start_first_task, %function\n"
"os_arch_start_first_task:\n"
"    call    os_task_stack_select_next\n"  /* a0 = first task's stack pointer; never NULL */
"    mv      sp, a0\n"
"    j       os_arch_context_restore_asm\n"".popsection\n"
);

/*
 * ***********************************************************************************************************
 * Task frame construction
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Lay a task's initial frame so the restore path can start it as though it had been switched
 *        out normally.
 *
 * @param[in] stack_base   Lowest address of the task's stack memory.
 * @param[in] stack_bytes  Size of that memory.
 * @param[in] entry        Task entry function.
 * @param[in] context      Argument handed to the entry function.
 * @return Stack pointer to hand the scheduler.
 */
uint32_t* os_arch_task_stack_initialize(uint8_t *stack_base, size_t stack_bytes,
                                        void (*entry)(void *context), void *context)
{
    uintptr_t  top;
    uint32_t  *frame;
    uint32_t   index;

    /* Round the top down to the ABI's 16-byte alignment before subtracting the frame, so the
     * resulting sp is aligned on entry - the compiler assumes that at every call boundary and
     * generates accesses that fault or silently misbehave if it is not true. */
    top   = ((uintptr_t)stack_base + (uintptr_t)stack_bytes) & ~((uintptr_t)OS_ARCH_STACK_ALIGNMENT_BYTES - 1U);
    frame = (uint32_t *)(top - (uintptr_t)OS_ARCH_FRAME_BYTES);

    for (index = 0U; index < (uint32_t)OS_ARCH_FRAME_WORDS; index++)
    {
        frame[index] = 0U;
    }

    /* mret returns to mepc with MIE taken from MPIE, in the mode named by MPP. */
    frame[OS_ARCH_FRAME_MSTATUS] = (uint32_t)(OS_ARCH_MSTATUS_MPIE_MSK | OS_ARCH_MSTATUS_MPP_M);
    frame[OS_ARCH_FRAME_MEPC]    = (uint32_t)(uintptr_t)entry;

    /* a0 carries the entry function's single argument, per the ilp32 calling convention. */
    frame[OS_ARCH_FRAME_A0]      = (uint32_t)(uintptr_t)context;

    /* Where a task lands if its entry function ever returns. A well-formed task never does, but
     * leaving ra at zero would turn that mistake into a jump to address 0 with no explanation. */
    frame[OS_ARCH_FRAME_RA]      = (uint32_t)(uintptr_t)os_arch_task_exit_trap;

    return frame;
}

/*
 * ***********************************************************************************************************
 * Trap context
 * ***********************************************************************************************************
*/

/* Declared here rather than taken from a kernel header: it is internal to os_task.c and the port is
 * the only thing outside it that needs the symbol - the same arrangement the ARM ports use. */
extern void os_task_exit(void);

/******************************************************************************************************/
/**
 * @brief Where a task lands if its entry function returns.
 */
void os_arch_task_exit_trap(void)
{
    os_task_exit();

    /* os_task_exit() does not return. If it ever did, stopping here is the only safe answer. */
    os_arch_config_fault_trap();
}

/*
 * ***********************************************************************************************************
 * Start-up
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Verify that mtvec really routes the software interrupt to the kernel's handler, and park in
 *        os_arch_config_fault_trap() if it does not.
 *
 * The RISC-V counterpart of the ARM port's PendSV vector check, and it exists for the same reason:
 * if another definition of the configured name won at link time, the build still succeeds and the
 * symptom is a board that reaches os_start() and stops dead - no trap, no output, nothing to attach
 * a debugger to.
 *
 * mtvec in vectored mode holds the table base in bits 31:2 with MODE=1 in bits 1:0, and entry N is
 * a jump instruction at base + N*4. The check therefore reads the jump rather than a pointer: it
 * decodes the JAL immediate at the software-interrupt slot and confirms the target is the handler
 * this port assembled. In direct mode (MODE=0) there is no per-cause slot to inspect, so the check
 * cannot run and is skipped rather than guessed at.
 */
static void os_arch_vector_check(void (*swi_handler)(void))
{
#if (OS_CONFIG_ARCH_VECTOR_CHECK != 0U)
    uint32_t  mtvec = OS_ARCH_CSR_READ(mtvec);
    uint32_t  instruction;
    uint32_t  offset;
    uintptr_t slot;
    uintptr_t target;

    /* Vectored mode only, and only a JAL slot this can decode. Anything else is a table
     * shape the check cannot read, which is not a fault - it simply has nothing to say. */
    if ((mtvec & 0x3UL) == 1UL)
    {
        slot        = (uintptr_t)(mtvec & ~0x3UL) + ((uintptr_t)OS_ARCH_TRAP_CAUSE_SWI * 4U);
        instruction = *(const volatile uint32_t *)slot;

        if ((instruction & 0x7FUL) == 0x6FUL)
        {
    /* Reassemble the JAL immediate, which the encoding scatters across the instruction word:
     * imm[20] at bit 31, imm[10:1] at 30:21, imm[11] at 20, imm[19:12] at 19:12, and bit 0 is
     * always zero. */
            offset = ((instruction >> 21) & 0x3FFUL) << 1;
            offset |= ((instruction >> 20) & 0x1UL) << 11;
            offset |= ((instruction >> 12) & 0xFFUL) << 12;
            offset |= ((instruction >> 31) & 0x1UL) << 20;

            if ((offset & (1UL << 20)) != 0UL)
            {
                offset |= ~((1UL << 21) - 1UL);     /* sign-extend the 21-bit displacement */
            }

            target = slot + (uintptr_t)(int32_t)offset;

            if (target != (uintptr_t)swi_handler)
            {
                os_arch_config_fault_trap();
            }
        }
    }
#else
    (void)swi_handler;
#endif
}

/******************************************************************************************************/
/**
 * @brief Per-core architecture start-up.
 *
 * Enables the machine software interrupt, which is the one source the kernel owns. Nothing else is
 * touched: the external interrupt controller belongs to the application and the SDK, and the timer
 * belongs to whatever drives the tick.
 *
 * mstatus.MIE stays as it is. Interrupts are turned on when the first task is entered, by the MPIE
 * its frame carries - enabling them here would let the tick fire before there is a task to preempt.
 */
void os_arch_init(void)
{
    os_arch_vector_check(OS_CONFIG_ARCH_SWI_HANDLER);

    /* Start the cycle counter.
     *
     * mcountinhibit gates the hardware performance counters, and the privileged spec leaves its
     * reset value implementation-defined - Hazard3 comes out of reset with CY set, so mcycle does
     * not count until this line runs. Without it os_arch_cycle_count_get() returns a constant 0,
     * and every caller that measures ELAPSED cycles waits for a difference that never appears:
     * os_delay_us() and the self-test's benchmarks spin forever. The board does not fault, it
     * simply stops making progress, which is why this is worth a comment rather than one line.
     *
     * Written by number because mcountinhibit postdates some assemblers' CSR tables, and this file
     * has to build on whatever toolchain the target ships with. Bit 0 is CY (cycle); bit 2 is IR
     * (instret), left as found since the kernel does not read it. */
    OS_ARCH_CSR_CLEAR(0x320, 1UL << 0);

    OS_ARCH_CSR_SET(mie, OS_ARCH_MIE_MSIE_MSK);
}

/******************************************************************************************************/
/**
 * @brief Program the periodic tick.
 *
 * Always the SoC's job on RISC-V. Cortex-M has SysTick at a fixed address, so the ARM port can
 * program the tick itself; the RISC-V privileged spec defines mtime/mtimecmp but deliberately not
 * where they live, so there is no address for an architecture layer to write. See
 * OS_CONFIG_TICK_SOURCE in os_arch_port_common.h.
 */
void os_arch_tick_init(void)
{
    os_arch_tick_init_cb();
}

/******************************************************************************************************/
/**
 * @brief Whole ticks elapsed since the last tick interrupt.
 *
 * Zero until tickless idle is implemented for this port: the kernel treats that as "no time beyond
 * the ticks already counted", which is exactly right for a build whose tick never stops.
 */
uint32_t os_arch_elapsed_ticks_get(void)
{
    return 0U;
}

/*
 * ***********************************************************************************************************
 * Cycle counter
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Free-running cycle count.
 *
 * mcycle is architectural - the privileged spec requires it - so unlike the ARM port there is no
 * DWT-present check and no fallback to a SysTick-derived estimate. The low half is all the kernel's
 * sampling needs; wrapping at 2^32 is handled by the unsigned subtraction at the call site.
 */
uint32_t os_arch_cycle_count_get(void)
{
    return OS_ARCH_CSR_READ(mcycle);
}

/*
 * ***********************************************************************************************************
 * Atomics
 * ***********************************************************************************************************
 *
 * Included rather than compiled separately, matching arch/arm/common/os_arch_port_v8m.c: the build
 * adds exactly one .c per core folder, and everything else is pulled in through it.
*/

#include "os_arch_atomic.c"
