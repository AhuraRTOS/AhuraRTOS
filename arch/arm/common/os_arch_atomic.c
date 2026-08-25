/**
 * @file os_arch_atomic.c
 * @brief Atomic read-modify-write of a single 32-bit word - the port half of the portable
 *        os_atomic_* API.
 *
 * Textually included by the shared port implementations (os_arch_port_v6m.c, _v7m.c, _v8m.c), the
 * same way each variant's os_arch_port.c includes those - it is not a separate compilation unit and
 * must never be added to a build as one.
 *
 * The port supplies the complete set the os_atomic_* API rests on, rather than one primitive it
 * composes from, because how a word updates indivisibly is a property of the core. Two backends,
 * chosen by OS_ARCH_ATOMIC_LOCK_FREE (see os_arch_port_common.h, which also states why ARMv8-M
 * baseline takes the second one despite having the exclusive instructions):
 *
 *   LDREX/STREX        One retry loop per operation. Lock-free: nothing is masked, so an ISR - or
 *                      another core - landing in the middle costs a second pass rather than costing
 *                      anyone correctness.
 *   Critical section   For cores that cannot express those loops. Costs the length of the update in
 *                      interrupt latency, and needs no retry, since nothing can interfere.
 *
 * One shared file rather than a copy in each port, because the split that matters here is the one
 * above - which follows the instruction set - and it does not line up with the v6m/v7m/v8m split
 * the context-switch code needs: v7m and v8m would hold identical exclusives loops, and v6m would
 * hold a critical-section set that ARMv8-M baseline shares with it for a different reason.
 *
 * Every operation below returns the value the word held BEFORE it ran, takes a pointer to a
 * naturally aligned 32-bit word, and is safe from tasks and from ISRs. os_arch_atomic_load() is the
 * one exception to all of this and is not here: a single aligned 32-bit load is already indivisible
 * on every core this port covers, so it is one LDR inlined in os_arch_port_common.h.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

#if (OS_ARCH_ATOMIC_LOCK_FREE == 1)

/*
 * ***********************************************************************************************************
 * Function implementations - lock-free backend (LDREX/STREX)
 * ***********************************************************************************************************
 *
 * Each operation is a single inline-assembly block: take a reservation on the word, compute from
 * what it held, store only if the reservation survived, and go round again if it did not.
 *
 * Written out per operation rather than funnelled through a shared helper or a CAS loop. Speed: the
 * whole sequence is five instructions, and CAS would re-load and compare what the reservation
 * already tells us. Safety: one asm block per loop is the same five instructions at -O0 as at -O2,
 * with no compiler spill between the LDREX and the STREX - ARM allows only one reservation per core
 * and leaves it implementation-defined whether other accesses clear it.
 *
 * "1:" and "1b" are local numeric labels, so each block stays correct wherever the compiler emits
 * it.
*/

/******************************************************************************************************/
/**
 * @brief Atomic store. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Value to store.
 * @return int32_t  Value held before the store.
 */
int32_t os_arch_atomic_exchange(__IO int32_t *target, int32_t value)
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
 * @brief Atomic add. See os_arch_port_common.h.
 *
 * ADD and SUB wrap rather than overflow: the assembler works on raw 32-bit registers, so the
 * signed-overflow undefined behaviour that the equivalent C expression would carry never arises.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to add.
 * @return int32_t  Value held before the addition.
 */
int32_t os_arch_atomic_add(__IO int32_t *target, int32_t value)
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
 * @brief Atomic subtract. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to subtract.
 * @return int32_t  Value held before the subtraction.
 */
int32_t os_arch_atomic_sub(__IO int32_t *target, int32_t value)
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
 * @brief Atomic bitwise OR. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to set.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_or(__IO int32_t *target, int32_t value)
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
 * @brief Atomic bitwise AND. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Mask to keep.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_and(__IO int32_t *target, int32_t value)
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
 * @brief Atomic bitwise XOR. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to flip.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_xor(__IO int32_t *target, int32_t value)
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
 * @brief Atomic bitwise NAND. See os_arch_port_common.h.
 *
 * The only operation needing two instructions inside the window: ARM has no single NAND, so the
 * AND result is inverted in place before the store.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Operand.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_nand(__IO int32_t *target, int32_t value)
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
 * @brief Atomic compare-and-swap. See os_arch_port_common.h.
 *
 * The only operation here that does not retry internally, and so the only one that needs CLREX: on
 * a mismatch the reservation is dropped rather than left set on a word this call is walking away
 * from, since a stale one can make an unrelated later STREX succeed when it should not.
 * store_failed is seeded with 1 so the mismatch path falls out reporting failure without the STREX
 * having run.
 *
 * @param[in,out] target    Word to update.
 * @param[in]     expected  Value the caller believes the word holds.
 * @param[in]     desired   Value to store if it still does.
 * @return bool  true if desired was stored.
 */
bool os_arch_atomic_cas(__IO int32_t *target, int32_t expected, int32_t desired)
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
 * ***********************************************************************************************************
 * Function implementations - critical-section backend
 * ***********************************************************************************************************
 *
 * Interference cannot be DETECTED here, so it has to be PREVENTED: each operation runs inside
 * os_critical_enter/exit, which costs the length of the update in interrupt latency and, on
 * multi-core, can wait on unrelated kernel work holding the same lock. Reusing the kernel's critical
 * section rather than a second private lock is deliberate - two locks over the same data is how
 * lock-ordering bugs start. No retry loop is needed, since nothing can interfere while the section
 * is held.
 *
 * ADD and SUB compute in the unsigned domain and convert back: signed overflow is undefined
 * behaviour, and unsigned wrapping reproduces the same two's-complement pattern anyway.
*/

/******************************************************************************************************/
/**
 * @brief Atomic store. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Value to store.
 * @return int32_t  Value held before the store.
 */
int32_t os_arch_atomic_exchange(__IO int32_t *target, int32_t value)
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
 * @brief Atomic add. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to add.
 * @return int32_t  Value held before the addition.
 */
int32_t os_arch_atomic_add(__IO int32_t *target, int32_t value)
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
 * @brief Atomic subtract. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to subtract.
 * @return int32_t  Value held before the subtraction.
 */
int32_t os_arch_atomic_sub(__IO int32_t *target, int32_t value)
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
 * @brief Atomic bitwise OR. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to set.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_or(__IO int32_t *target, int32_t value)
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
 * @brief Atomic bitwise AND. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Mask to keep.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_and(__IO int32_t *target, int32_t value)
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
 * @brief Atomic bitwise XOR. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to flip.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_xor(__IO int32_t *target, int32_t value)
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
 * @brief Atomic bitwise NAND. See os_arch_port_common.h.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Operand.
 * @return int32_t  Value held before the operation.
 */
int32_t os_arch_atomic_nand(__IO int32_t *target, int32_t value)
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
 * @brief Atomic compare-and-swap. See os_arch_port_common.h.
 *
 * Never fails spuriously on this backend - nothing could have interfered - but callers still loop,
 * because the portable contract is written to the weaker guarantee the exclusives backend gives.
 *
 * @param[in,out] target    Word to update.
 * @param[in]     expected  Value the caller believes the word holds.
 * @param[in]     desired   Value to store if it still does.
 * @return bool  true if desired was stored.
 */
bool os_arch_atomic_cas(__IO int32_t *target, int32_t expected, int32_t desired)
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
