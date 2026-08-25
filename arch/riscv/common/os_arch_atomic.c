/**
 * @file os_arch_atomic.c
 * @brief The kernel's atomic set for RV32.
 *
 * Included textually by os_arch_port_rv32.c, exactly as the ARM tree includes its own; never added
 * to a build directly.
 *
 * WHY MOST OF THIS IS ONE INSTRUCTION
 *
 * The ARM port implements every operation as an LDREX/modify/STREX retry loop, because ARM has no
 * read-modify-write instruction. RISC-V's A extension does: amoadd.w, amoor.w, amoand.w, amoxor.w
 * and amoswap.w each perform the whole operation atomically and return the previous value, which is
 * exactly this API's contract. There is no loop to retry and no reservation to lose, so these are
 * both smaller and constant-time - a real advantage of the architecture, not a workaround.
 *
 * Two operations do not map to an AMO and keep the reservation loop:
 *
 *   - NAND, because there is no amonand.w. AND and NOT cannot be fused into one atomic step.
 *   - CAS, because the A extension has no compare-and-swap. (Zacas adds amocas.w, but it is not in
 *     the ISA string the Pico SDK builds with, so relying on it would break the build it targets.)
 *
 * The .aqrl suffix on every operation gives acquire-release ordering: no access after the atomic may
 * be hoisted above it and none before it may sink below. That is what the kernel's users of these
 * expect, and it costs nothing on a core that does not reorder.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

#if (OS_ARCH_HAS_EXCLUSIVES == 1)

/******************************************************************************************************/
/**
 * @brief Atomic exchange. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Value to store.
 * @return int32_t  Value held before the exchange.
 */
int32_t os_arch_atomic_exchange(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    __asm volatile("amoswap.w.aqrl %0, %2, (%1)"
                   : "=&r"(previous) : "r"(target), "r"(value) : "memory");

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic add. See os_arch_port_common.h.
 *
 * Wraps rather than overflows: the instruction works on a raw 32-bit register, so the signed
 * overflow that the equivalent C expression would carry never arises.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to add.
 * @return int32_t  Value held before the addition.
 */
int32_t os_arch_atomic_add(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    __asm volatile("amoadd.w.aqrl %0, %2, (%1)"
                   : "=&r"(previous) : "r"(target), "r"(value) : "memory");

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic subtract. See os_arch_port_common.h.
 *
 * There is no amosub.w, and none is needed: subtracting is adding the two's-complement negation,
 * computed in a register before the atomic rather than inside it. The negation of INT32_MIN wraps
 * to itself, which is the same answer the add path gives, so no input is special.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to subtract.
 * @return int32_t  Value held before the subtraction.
 */
int32_t os_arch_atomic_sub(volatile int32_t *target, int32_t value)
{
    int32_t previous;
    int32_t negated = (int32_t)(0U - (uint32_t)value);

    __asm volatile("amoadd.w.aqrl %0, %2, (%1)"
                   : "=&r"(previous) : "r"(target), "r"(negated) : "memory");

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise OR. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to set.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_or(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    __asm volatile("amoor.w.aqrl %0, %2, (%1)"
                   : "=&r"(previous) : "r"(target), "r"(value) : "memory");

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise AND. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Mask to apply.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_and(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    __asm volatile("amoand.w.aqrl %0, %2, (%1)"
                   : "=&r"(previous) : "r"(target), "r"(value) : "memory");

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise XOR. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to toggle.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_xor(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    __asm volatile("amoxor.w.aqrl %0, %2, (%1)"
                   : "=&r"(previous) : "r"(target), "r"(value) : "memory");

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise NAND. See os_arch_port_common.h.
 *
 * The one logical operation with no AMO form, so this is the reservation loop the ARM port uses for
 * everything: reserve with lr.w, compute, and let sc.w fail if anything touched the word meanwhile.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Mask to apply.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_nand(volatile int32_t *target, int32_t value)
{
    int32_t  previous;
    int32_t  updated;
    uint32_t failed;

    __asm volatile(
        "1:  lr.w.aq   %0, (%3)      \n"
        "    and       %1, %0, %4    \n"
        "    not       %1, %1        \n"
        "    sc.w.rl   %2, %1, (%3)  \n"
        "    bnez      %2, 1b        \n"
        : "=&r"(previous), "=&r"(updated), "=&r"(failed)
        : "r"(target), "r"(value)
        : "memory");

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Compare-and-swap. See os_arch_port_common.h.
 *
 * Fails and returns false on a spurious sc.w failure as well as on a genuine mismatch, which is
 * permitted: this is the weak form, and every caller in the kernel retries.
 *
 * @param[in,out] target    Word to update.
 * @param[in]     expected  Value the caller believes is there.
 * @param[in]     desired   Value to store if it is.
 * @return bool  True when the swap happened.
 */
bool os_arch_atomic_cas(volatile int32_t *target, int32_t expected, int32_t desired)
{
    int32_t  current;
    uint32_t failed = 1U;

    __asm volatile(
        "    lr.w.aq   %0, (%2)      \n"
        "    bne       %0, %3, 1f    \n"   /* not what the caller expected: leave failed set */
        "    sc.w.rl   %1, %4, (%2)  \n"
        "1:                          \n"
        : "=&r"(current), "+r"(failed)
        : "r"(target), "r"(expected), "r"(desired)
        : "memory");

    return (failed == 0U);
}

#else /* no A extension: exclude with the kernel's own critical section */

/******************************************************************************************************/
/**
 * @brief Atomic exchange. See os_arch_port_common.h.
 */
int32_t os_arch_atomic_exchange(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    os_critical_enter();

    previous = *target;
    *target  = value;

    os_critical_exit();

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic add. See os_arch_port_common.h.
 */
int32_t os_arch_atomic_add(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    os_critical_enter();

    previous = *target;
    *target  = (int32_t)((uint32_t)previous + (uint32_t)value);

    os_critical_exit();

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic subtract. See os_arch_port_common.h.
 */
int32_t os_arch_atomic_sub(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    os_critical_enter();

    previous = *target;
    *target  = (int32_t)((uint32_t)previous - (uint32_t)value);

    os_critical_exit();

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise OR. See os_arch_port_common.h.
 */
int32_t os_arch_atomic_or(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    os_critical_enter();

    previous = *target;
    *target  = (int32_t)((uint32_t)previous | (uint32_t)value);

    os_critical_exit();

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise AND. See os_arch_port_common.h.
 */
int32_t os_arch_atomic_and(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    os_critical_enter();

    previous = *target;
    *target  = (int32_t)((uint32_t)previous & (uint32_t)value);

    os_critical_exit();

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise XOR. See os_arch_port_common.h.
 */
int32_t os_arch_atomic_xor(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    os_critical_enter();

    previous = *target;
    *target  = (int32_t)((uint32_t)previous ^ (uint32_t)value);

    os_critical_exit();

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Atomic bitwise NAND. See os_arch_port_common.h.
 */
int32_t os_arch_atomic_nand(volatile int32_t *target, int32_t value)
{
    int32_t previous;

    os_critical_enter();

    previous = *target;
    *target  = (int32_t)~((uint32_t)previous & (uint32_t)value);

    os_critical_exit();

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Compare-and-swap. See os_arch_port_common.h.
 */
bool os_arch_atomic_cas(volatile int32_t *target, int32_t expected, int32_t desired)
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

#endif /* OS_ARCH_HAS_EXCLUSIVES */

#endif /* OS_CONFIG_ATOMIC_ENABLE */
