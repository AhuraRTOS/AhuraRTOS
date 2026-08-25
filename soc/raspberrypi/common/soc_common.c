/**
 * @file soc_common.c
 * @brief SoC-owned kernel callbacks shared by every Raspberry Pi RP2 chip.
 *
 * Everything the kernel needs from an RP2 chip except one thing: the core id, the kernel
 * spinlock, the CPU clock symbol the SDK does not otherwise provide, the SysTick vector, and
 * booting core 1. All of it is SIO or plain SDK, and SIO is the same block on the RP2040 and the
 * RP2350 alike, so it lives here once rather than twice.
 *
 * The exception is how a core interrupts the other one - FIFO on the RP2040, doorbells on the
 * RP2350 - which each chip package supplies as soc_ipi_arm() and
 * os_arch_core_ipi_request_cb(). See soc_common.h.
 *
 * This file is compiled INTO whichever chip package the build selected, not into a library of its
 * own, so there is exactly one definition of each symbol in the link and no archive-extraction
 * question to get wrong.
 *
 * The application still copies template/os_cb.c for its own half of the callback contract: where
 * log output goes, what a failed assertion does, how a blown stack is reported. Nothing here
 * answers those.
 *
 * Most definitions are weak, so a strong definition anywhere in the application replaces that one
 * callback and leaves the rest in place. Two are strong, and each says why at its own definition:
 * isr_systick and os_arch_soc_init_cb both have to displace a weak definition that is always
 * linked - crt0.S's vector stub and the kernel's empty default - and between two weak definitions
 * the linker simply keeps whichever it saw first.
 *
 * Tick source: these packages expect the kernel default, OS_CONFIG_TICK_SOURCE_SYSTICK, where the
 * port programs SysTick itself. Selecting OS_CONFIG_TICK_SOURCE_EXTERNAL means supplying
 * os_arch_tick_init_cb() from the application, because the choice of timer is then the
 * application's; the package deliberately does not guess one, so the omission is a link error
 * naming the function rather than a kernel whose clock never advances.
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

#include "soc_common.h"

#include <stdint.h>

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/systick.h"
#include "hardware/uart.h"
#include "pico/multicore.h"

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

/* The kernel reads the CPU frequency through the CMSIS SystemCoreClock variable on every use, so
 * a clock change at runtime needs no kernel involvement. The Pico SDK defines that symbol only in
 * its optional CMSIS stub, which pico_stdlib does not pull in, so the package defines it here.
 *
 * Weak, so a project that DOES link the SDK's CMSIS stub gets the stub's strong definition and
 * this one steps aside rather than colliding. The compile-time default keeps it sane before
 * os_arch_soc_init_cb() replaces it with the live value; the kernel is not started before then. */
OS_WEAK uint32_t SystemCoreClock = SYS_CLK_HZ;

/* Resolved once in os_arch_soc_init_cb(): spin_lock_instance() is a computation the acquire path
 * should not repeat on every critical section. */
static spin_lock_t *soc_lock = NULL;

#if (SOC_CONFIG_FAULT_REPORT != 0U)
/* Set by a secondary core the instant it reaches its entry point, read by core 0 later. One byte,
 * written by one core and read by the other, so no lock is needed. */
static __IO uint8_t soc_core_reached = 0xFFU;

/* What a faulting core managed to record before parking. Deliberately captured rather than
 * printed: see soc_fault_report(). Written by the faulting core, read by the healthy one. */
typedef struct
{
    uint32_t taken;   /**< Non-zero once every field below is valid. Written last. */
    uint32_t core;    /**< Which core faulted - the whole point on a dual-core build. */
    uint32_t pc;      /**< Instruction that faulted. */
    uint32_t lr;      /**< Its caller. */
    uint32_t psr;     /**< Program status, including the exception number if any. */
    uint32_t cfsr;    /**< Configurable Fault Status: says WHICH fault. */
    uint32_t hfsr;    /**< HardFault Status: usually FORCED, meaning escalated from CFSR. */
    uint32_t sp;      /**< Stack pointer at the fault, so the words below can be located. */

    /* A slice of the faulting stack, above the exception frame. On a jump-to-zero the stacked LR
     * names only the innermost call, which is rarely the culprit - the return addresses further up
     * are what identify the path that got there. Eight words is enough to cross a few frames and
     * costs nothing to capture. */
    uint32_t stack[8];

} soc_fault_t;

static __IO soc_fault_t soc_fault = { 0U, 0U, 0U, 0U, 0U, 0U, 0U };
#endif

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

/* The raw console is needed by fault reporting: printf must not be called from the fault handler,
 * which may be entered with the stdio mutex held by the very code that faulted. */
#define SOC_PANIC_OUTPUT        (SOC_CONFIG_FAULT_REPORT != 0U)

#if SOC_PANIC_OUTPUT

/******************************************************************************************************/
static void soc_panic_puts(const char *text);

/******************************************************************************************************/
static void soc_panic_hex(uint32_t value);

#endif /* SOC_PANIC_OUTPUT */

#if (OS_CONFIG_CORE_COUNT > 1U)

/******************************************************************************************************/
static void soc_core1_entry(void);

#endif /* OS_CONFIG_CORE_COUNT > 1U */

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Wait for work on an idle core.
 *
 * WFE, not WFI. SysTick runs from the processor clock on these chips (CLKSOURCE 1), so WFI gates
 * the core's clock and stops that core's tick with it: an idle secondary core then has nothing
 * but an inter-core interrupt left to wake it, and a missed signal sleeps it for good. WFE's
 * event register latches, so an SEV arriving before the instruction still wakes it - which is why
 * the IPI callbacks in both chip packages broadcast one before ringing the doorbell or pushing
 * the FIFO.
 *
 * The "memory" clobber stops the compiler hoisting the idle loop's own reads out across this
 * call: what it is waiting for is written by an interrupt or by another core.
 */
void os_arch_soc_idle_cb(void)
{
    OS_ARCH_WFE();
}

/******************************************************************************************************/
/**
 * @brief Bottom of the given core's handler (MSP) stack. STRONG, replacing the kernel's
 *        linker-derived default, which cannot describe more than one stack.
 *
 * The SDK gives each core its own pair of symbols and they are NOT interchangeable:
 *
 *     __StackBottom    .. __StackTop       core 0's handler stack
 *     __StackOneBottom .. __StackOneTop    core 1's handler stack
 *     __StackLimit                         the bottom of the LOWEST stack, i.e. core 1's
 *
 * That last one is the trap, and the kernel's default walks straight into it: on a dual-core
 * build __StackLimit is core 1's bottom, several kilobytes below core 0's, so an MSPLIM set from
 * it does not guard core 0 at all. Core 0's handler stack could overrun its own 2 KB and keep
 * going down through the whole of core 1's stack before anything objected - corrupting the other
 * core's saved state on the way, then finally faulting during exception stacking, which the fault
 * handler cannot itself stack for. The chip locks up with no fault recorded and both cores dead.
 *
 * Sizes come from PICO_STACK_SIZE and PICO_CORE1_STACK_SIZE; this only has to name the right
 * bottom for the right core, and the symbols are absolute, so their addresses are the values.
 */
uint32_t os_arch_handler_stack_limit_cb(uint32_t core_id)
{
    extern uint32_t __StackBottom;

#if (OS_CONFIG_CORE_COUNT > 1U)
    extern uint32_t __StackOneBottom;

    if (core_id == 1U)
    {
        return (uint32_t)(uintptr_t)&__StackOneBottom;
    }
#endif

    if (core_id != 0U)
    {
        /* No third core exists on any RP2 part; say so rather than guess an address. */
        return 0U;
    }

    return (uint32_t)(uintptr_t)&__StackBottom;
}

/******************************************************************************************************/
/**
 * @brief Top of the given core's handler (MSP) stack. STRONG, replacing the kernel's missing
 *        default: the vector table only names core 0's initial stack pointer, so the kernel
 *        cannot answer this one for itself on a multi-core build.
 *
 * The kernel's context switch resets MSP to this value on its first start, abandoning the boot
 * context. If a secondary core answered with the vector table instead, it would reset its MSP
 * to core 0's stack top and the two cores would push handler frames into the same region,
 * overwriting each other's saved state - a hard fault whose frame reads as garbage. This is the
 * exact failure that motivated the callback.
 *
 * Sizes come from PICO_STACK_SIZE and PICO_CORE1_STACK_SIZE; the symbols are absolute, so their
 * addresses are the values. Unknown core ids fall back to core 0's stack: the only other stack
 * this package has placed, and a guess of nothing would be far worse than a shared region.
 */
uint32_t os_arch_handler_stack_top_cb(uint32_t core_id)
{
    extern uint32_t __StackTop;

#if (OS_CONFIG_CORE_COUNT > 1U)
    extern uint32_t __StackOneTop;

    if (core_id == 1U)
    {
        return (uint32_t)(uintptr_t)&__StackOneTop;
    }
#else
    (void)core_id;
#endif

    return (uint32_t)(uintptr_t)&__StackTop;
}

/******************************************************************************************************/
/**
 * @brief Prepare the SoC for the kernel. Called by os_init(), first thing.
 *
 * Three things the kernel cannot do for itself on these chips: publish the CPU clock it will
 * program SysTick from, ready the hardware spinlock its critical sections take, and arm this
 * core's inter-core interrupt. None may wait until os_start(), because os_init() already needs
 * the clock and may already take the lock - which is exactly why the kernel calls this itself
 * rather than leaving a step for the application to remember.
 *
 * STRONG, displacing the kernel's weak empty default in os_kernel.c. Two weak definitions would
 * leave the linker keeping whichever it saw first, and the kernel's is always linked. Losing that
 * distinction is not subtle: with the kernel's default in place soc_lock stays NULL, and the
 * first critical section dereferences it.
 */
void os_arch_soc_init_cb(void)
{
#if (SOC_CONFIG_CLOCK_AUTO_UPDATE != 0U)
    /* Read the live clock rather than trusting the compile-time default: a board that overrode
     * the system clock, or called set_sys_clock_khz() in main(), is otherwise off by that factor
     * in every tick period and every busy-wait delay. */
    SystemCoreClock = clock_get_hz(clk_sys);
#endif

    /* Leaves the lock free and returns its address. Safe on core 0 before core 1 exists; core 1
     * must NOT repeat it, or it would release a lock core 0 may be holding. */
    soc_lock = spin_lock_init(SOC_CONFIG_SPINLOCK_ID);

#if (OS_CONFIG_CORE_COUNT > 1U)
    soc_ipi_arm();
#endif
}

#if (OS_CONFIG_TICK_SOURCE == OS_CONFIG_TICK_SOURCE_SYSTICK) && (SOC_CONFIG_SYSTICK_VECTOR != 0U)

/******************************************************************************************************/
/**
 * @brief SysTick vector: advance the kernel clock.
 *
 * The kernel owns exactly one vector, PendSV, and routes the tick through the application instead
 * - on a CMSIS part that means writing os_tick_handler() into the generated SysTick_Handler. The
 * SDK generates nothing of the sort, and its vector table calls the entry isr_systick, so on
 * these chips every project would have to write this same three-line function from the datasheet.
 * The package writes it once.
 *
 * The port has already programmed and enabled SysTick by the time this can fire; all that is
 * missing is the vector, and crt0.S declares its own isr_systick weak precisely so it can be
 * replaced.
 *
 * STRONG, and the exception is load-bearing. crt0.S's stub is itself weak, and between two weak
 * definitions the linker keeps whichever it saw first - which is crt0.o, always, because the
 * runtime is linked ahead of this library. A weak definition here therefore loses in silence and
 * the first tick hits crt0.S's `bkpt`. Only a strong definition displaces a weak one regardless
 * of order. This is the same reason the kernel's port defines isr_pendsv strong.
 *
 * The cost is that an application cannot override this one by defining it too - that is a
 * duplicate-symbol error rather than a silent replacement. An application that needs its own work
 * on the tick selects OS_CONFIG_TICK_SOURCE_EXTERNAL, which removes this definition along with
 * the port's SysTick programming and hands the whole timer over.
 */
void isr_systick(void)
{
    os_tick_handler();
}

#endif /* OS_CONFIG_TICK_SOURCE_SYSTICK && SOC_CONFIG_SYSTICK_VECTOR */

#if (OS_CONFIG_CORE_COUNT > 1U)

/******************************************************************************************************/
/**
 * @brief Boot a secondary core so it reaches os_core_start(). Called by os_start().
 *
 * multicore_launch_core1() points core 1 at the same vector table core 0 is using, which is what
 * the kernel requires of a secondary core: entry 14 there is the kernel's PendSV handler, so the
 * context switch behaves identically on both cores.
 *
 * The kernel calls this itself, once per core above 0, so there is nothing for the application to
 * call and nothing to forget - setting OS_CONFIG_CORE_COUNT to 2 is the whole of it. That is also
 * why the id is checked rather than assumed: these chips have exactly one secondary core, and a
 * request for any other is a configuration error the kernel cannot see (it only knows the count
 * the application asked for) but this package can.
 */
void os_arch_core_launch_cb(uint32_t core_id)
{
    if (core_id != 1U)
    {
        /* Unreachable while SOC_CORE_COUNT holds OS_CONFIG_CORE_COUNT to 2, which soc_common.h
         * enforces at compile time. Kept as a guard rather than an assert because a wrong id here
         * would launch nothing at all, silently. */
        return;
    }

    multicore_launch_core1(soc_core1_entry);
}

/******************************************************************************************************/
/**
 * @brief Report what this package knows about core 1's bring-up. Called by the kernel only after
 *        something has already gone wrong.
 *
 * Nothing is printed at launch time on purpose. os_arch_core_launch_cb() runs inside os_start(),
 * and on a USB console the host has not opened the port yet - anything written there is dropped by
 * stdio_usb and never seen. That is not a theory: an earlier version of this file printed exactly
 * these lines from the launch and neither ever appeared, which cost a whole diagnostic round.
 *
 * STRONG, displacing the kernel's weak empty default.
 */
void os_arch_soc_diagnose_cb(void)
{
#if (SOC_CONFIG_FAULT_REPORT != 0U)
    /* The fault first, because it explains everything else when it is there. */
    if (soc_fault.taken != 0U)
    {
        printf("         [soc] *** core %lu HARD FAULTED and is parked ***\r\n",
               (unsigned long)soc_fault.core);
        printf("               pc=0x%08lX  lr=0x%08lX  psr=0x%08lX\r\n",
               (unsigned long)soc_fault.pc, (unsigned long)soc_fault.lr,
               (unsigned long)soc_fault.psr);
        printf("               CFSR=0x%08lX  HFSR=0x%08lX  sp=0x%08lX\r\n",
               (unsigned long)soc_fault.cfsr, (unsigned long)soc_fault.hfsr,
               (unsigned long)soc_fault.sp);

        printf("               stack above the frame:\r\n");
        for (uint32_t i = 0U; i < 8U; i++)
        {
            printf("                 [%lu] 0x%08lX\r\n",
                   (unsigned long)i, (unsigned long)soc_fault.stack[i]);
        }
        printf("               That is the whole cause: the core did not fail to start, it\r\n");
        printf("               started and then died. Decode CFSR for which fault, and look\r\n");
        printf("               up pc in the .elf to find where.\r\n");
        (void)fflush(stdout);
        return;
    }

    printf("         [soc] ");

    if (soc_core_reached == 0xFFU)
    {
        printf("core 1 NEVER reached its entry point.\r\n");
        printf("         multicore_launch_core1() did not deliver it, even though the same call\r\n");
        printf("         works from main() in the SDK's own example - so the difference is the\r\n");
        printf("         context os_start() calls it from, not the SDK.\r\n");
    }
    else
    {
        printf("core %u DID reach its entry point.\r\n", (unsigned)soc_core_reached);
        printf("         So the launch, the vector table and this package are fine, and the\r\n");
        printf("         fault is in what follows: soc_ipi_arm() or the kernel's own\r\n");
        printf("         os_core_start() path.\r\n");
    }

    printf("         (no HARDFAULT line above means neither core faulted)\r\n");
    (void)fflush(stdout);
#endif
}

/******************************************************************************************************/
/**
 * @brief Index of the calling core.
 *
 * Reads the SIO CPUID register, which is the only place a Cortex-M can learn this: the
 * architecture has no core-id register, which is why the kernel asks the SoC.
 */
OS_WEAK uint32_t os_arch_core_id_get_cb(void)
{
    return (uint32_t)get_core_num();
}

#endif /* OS_CONFIG_CORE_COUNT > 1U */

#if (OS_ARCH_SPINLOCK_USE_CB)

/******************************************************************************************************/
/**
 * @brief Take the kernel spinlock, busy-waiting until it is free.
 *
 * The lock argument is the kernel's own one-word spinlock and is deliberately unused: this
 * backend replaces that word with whatever the SDK considers correct on this chip, which is not
 * the same answer on both. On the RP2040 it is a SIO hardware spinlock. On the RP2350 the SDK
 * defaults PICO_USE_SW_SPIN_LOCKS to 1 because of errata E2 and uses a software lock built on
 * LDAEXB/STREXB instead. Calling the SDK's API rather than touching SIO directly is what makes
 * the kernel inherit that workaround for free, and what would carry a future one.
 *
 * There is exactly one kernel lock (os_critical_kernel_lock), and the kernel counts nesting per
 * core before it calls here, so a non-recursive lock is the right shape either way.
 *
 * The "unsafe" in the SDK's name means "does not touch the interrupt mask", which is what is
 * wanted here: the kernel has already masked interrupts before calling.
 */
OS_WEAK void os_arch_spinlock_acquire_cb(os_arch_spinlock_t *lock)
{
    (void)lock;

    spin_lock_unsafe_blocking(soc_lock);
}

/******************************************************************************************************/
/**
 * @brief Release the kernel spinlock taken by os_arch_spinlock_acquire_cb.
 */
OS_WEAK void os_arch_spinlock_release_cb(os_arch_spinlock_t *lock)
{
    (void)lock;

    spin_unlock_unsafe(soc_lock);
}

#endif /* OS_ARCH_SPINLOCK_USE_CB */

#if (OS_CONFIG_TICKLESS_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Called right before the idle sleep.
 *
 * Left empty, which selects a plain SLEEP: the CPU clock stops and every peripheral clock keeps
 * running, so nothing needs saving here and the post-sleep hook has nothing to restore. The
 * deeper modes on these chips (DORMANT, or gating clk_sys) stop SysTick itself, which the kernel
 * cannot yet measure a sleep against - see the tickless section of doc/porting.md.
 */
OS_WEAK void os_tickless_pre_sleep_cb(void)
{
}

/******************************************************************************************************/
/**
 * @brief Called right after wakeup.
 */
OS_WEAK void os_tickless_post_sleep_cb(void)
{
}

#endif /* OS_CONFIG_TICKLESS_ENABLE */

#if (SOC_CONFIG_FAULT_REPORT != 0U)

/******************************************************************************************************/
/**
 * @brief Report a fault and park, instead of the SDK's silent breakpoint.
 *
 * crt0.S points every fault vector at a `bkpt`. With a debugger attached that is exactly right.
 * With none, the breakpoint escalates into a second fault and the core stops dead - no output, no
 * address, and on a dual-core build no indication of WHICH core died. That last part is the
 * expensive one: a core that faulted and a core that was never started look identical from the
 * other core, and telling them apart is otherwise a long afternoon.
 *
 * Printing from fault context is best-effort, not guaranteed - the transport may already be
 * wedged, and this runs with the faulting core's state half gone. It costs nothing to try.
 *
 * @param frame The exception frame, picked from MSP or PSP by the naked wrapper below.
 */
void soc_fault_report(const uint32_t *frame)
{
    /* RECORD, do not print. The first version of this printed directly, and on a dual-core build
     * that is actively wrong: the other core is usually mid-printf of its own, the two outputs
     * interleave, and every field comes out corrupted - the core number arrived as a flash
     * address. Worse, the transport's mutex is held by that other core, so printing here can block
     * a core that has already faulted.
     *
     * So this captures and parks. os_arch_soc_diagnose_cb() prints it later, from the healthy
     * core, with the console to itself. */
    uint32_t sp = (uint32_t)(uintptr_t)frame;

    /* The ORDER here is load-bearing, and getting it wrong hid this fault completely.
     *
     * Everything that can be read without touching the frame goes down FIRST, and taken is set
     * before the frame is dereferenced at all. The frame pointer comes from the stack the fault
     * happened on, and a fault whose cause is a bad stack pointer hands this function a bad
     * pointer - so reading frame[6] faults again, inside the HardFault handler, which the
     * architecture cannot escalate any further. The core goes to LOCKUP and taken is still zero:
     * a fault that happened, was detected, and left no trace of itself.
     *
     * That is exactly what this board was doing. CFSR and HFSR are memory-mapped registers, not
     * stack, so they are always safe and they are the two that say WHICH fault it was. */
    soc_fault.core = (uint32_t)get_core_num();
    soc_fault.sp   = sp;
    soc_fault.cfsr = *(volatile uint32_t *)0xE000ED28UL;
    soc_fault.hfsr = *(volatile uint32_t *)0xE000ED2CUL;
    soc_fault.taken = 1U;

    /* Now the frame, and only if it can be trusted: inside RAM, word aligned, and with room for
     * the eight words the architecture guarantees are there. Two separate bounds, because the
     * frame and the slice above it need different amounts of headroom - one bound sized for the
     * larger read rejects a perfectly good frame sitting near the top of a stack, which is exactly
     * where a shallow exception puts it. That mistake cost a whole run: sp was 0x20081FD0, 48
     * bytes under the stack top, entirely valid, and the report threw the PC away. */
    if ((sp >= (uint32_t)SRAM_BASE) && (sp <= ((uint32_t)SRAM_END - 32U)) && ((sp & 3U) == 0U))
    {
        soc_fault.pc  = frame[6];   /* architectural layout: r0-r3, r12, lr, pc, xpsr */
        soc_fault.lr  = frame[5];
        soc_fault.psr = frame[7];

        /* Two means the frame was readable and every field above is real. One means only the
         * fault-status registers are. */
        soc_fault.taken = 2U;

        /* The words just above the exception frame: whatever the faulting code had on its stack,
         * return addresses included. Optional, and skipped rather than risked when the frame sits
         * too close to the top for them to exist. */
        if (sp <= ((uint32_t)SRAM_END - 64U))
        {
            for (uint32_t i = 0U; i < 8U; i++)
            {
                soc_fault.stack[i] = frame[8U + i];
            }
        }
    }

    /* Core 0 prints; a secondary core only records.
     *
     * The asymmetry is the point. When core 1 faults, core 0 is usually mid-printf and holds the
     * transport's mutex - printing from the faulted core then interleaves into garbage, which is
     * exactly what happened the first time this fired. But when CORE 0 faults, nothing else is
     * using the console and nobody is left to report on its behalf: recording silently just hangs
     * the board with no output at all, which is worse than the breakpoint this replaced. */
    if ((uint32_t)get_core_num() == 0U)
    {
        /* soc_panic_puts, NOT printf, and that is the whole difference between a board that says
         * why it died and one that just dies.
         *
         * printf goes through the SDK's stdio, which takes a mutex and keeps buffered state. A
         * fault can land anywhere, including inside stdio itself with that mutex held and the
         * transport half-written - and the report then blocks on a lock the faulted core is
         * itself holding. It cost several runs here: core 0 faulted, recorded everything below
         * correctly, reached this line, and produced not one character. The board looked frozen
         * for no reason, and the fault that explained it was sitting in memory unread.
         *
         * Polling the UART needs no lock and no state, so it works from fault context by
         * construction. */
        soc_panic_puts((soc_fault.taken == 1U)
                       ? "\r\n*** core 0 HARD FAULT *** (frame unreadable - bad stack pointer)\r\n    pc=0x"
                       : "\r\n*** core 0 HARD FAULT *** pc=0x");
        soc_panic_hex(soc_fault.pc);
        soc_panic_puts(" lr=0x");
        soc_panic_hex(soc_fault.lr);
        soc_panic_puts(" psr=0x");
        soc_panic_hex(soc_fault.psr);
        soc_panic_puts("\r\n    CFSR=0x");
        soc_panic_hex(soc_fault.cfsr);
        soc_panic_puts(" HFSR=0x");
        soc_panic_hex(soc_fault.hfsr);
        soc_panic_puts(" sp=0x");
        soc_panic_hex(soc_fault.sp);
        soc_panic_puts("\r\n");
    }

    while (1)
    {
    }
}

/******************************************************************************************************/
/**
 * @brief HardFault vector. Naked, because the stacked frame must be located before the compiler
 *        is allowed to touch the stack.
 *
 * Bit 2 of EXC_RETURN says which stack holds the frame: clear means the fault came from handler
 * mode (MSP), set means thread mode (PSP). Reading the wrong one prints whatever happens to sit
 * under the other pointer, which is worse than printing nothing.
 *
 * STRONG, like isr_systick: crt0.S's own stub is weak and always linked, and between two weak
 * definitions the linker keeps whichever it saw first.
 */
__attribute__((naked)) void isr_hardfault(void)
{
    __asm volatile (
        "tst lr, #4          \n"
        "ite eq              \n"
        "mrseq r0, msp       \n"
        "mrsne r0, psp       \n"
        "b soc_fault_report  \n"
    );
}

#endif /* SOC_CONFIG_FAULT_REPORT */

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_CORE_COUNT > 1U)

/******************************************************************************************************/
/**
 * @brief Core 1's entry point. Arms this core's IPI, then enters the scheduler and stays there.
 */
static void soc_core1_entry(void)
{
#if (SOC_CONFIG_FAULT_REPORT != 0U)
    /* A flag, deliberately NOT a printf. The SDK's stdio takes a mutex and core 0 holds it almost
     * continuously while producing output, so printing from here would not report progress - it
     * would block the very core being diagnosed. Core 0 reads this and reports it after launch. */
    soc_core_reached = (uint8_t)get_core_num();
#endif

    soc_ipi_arm();

    /* Does not return. */
    os_core_start();
}

#endif /* OS_CONFIG_CORE_COUNT > 1U */

#if SOC_PANIC_OUTPUT

/******************************************************************************************************/
/**
 * @brief Write a string straight at the UART, with no stdio and no locks.
 *
 * printf is unusable on every path that calls this. The SDK's stdio takes a mutex whose owner is
 * recorded as a CORE, and these callers run either with interrupts masked, or while the kernel
 * spinlock is held, or after the other core has already stopped - all states in which waiting for
 * a mutex is waiting forever. Polling the TX FIFO needs nothing but the UART itself, so it works
 * from an exception, from a masked region, and from a core whose peer is dead.
 *
 * Output can interleave if both cores write at once. That is accepted: a garbled line still names
 * the fault, and the alternative is a lock that may already be the reason we are here.
 *
 * Silently does nothing on a board with no default UART, which is correct - the diagnostic is a
 * bonus, never a dependency.
 */
static void soc_panic_puts(const char *text)
{
#ifdef PICO_DEFAULT_UART
    uart_hw_t *hw = uart_get_hw(uart_default);

    if (text == NULL)
    {
        return;
    }

    while (*text != '\0')
    {
        while ((hw->fr & UART_UARTFR_TXFF_BITS) != 0U)
        {
        }

        hw->dr = (uint32_t)(uint8_t)*text;
        text++;
    }
#else
    (void)text;
#endif
}

/******************************************************************************************************/
/**
 * @brief Write a 32-bit value as eight hex digits through soc_panic_puts.
 */
static void soc_panic_hex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    char     text[9];
    uint32_t index;

    for (index = 0U; index < 8U; index++)
    {
        text[7U - index] = digits[value & 0xFU];
        value >>= 4U;
    }

    text[8] = '\0';

    soc_panic_puts(text);
}

#endif /* SOC_PANIC_OUTPUT */
