/**
 * @file os_atomic.c
 * @brief Atomic operations on a single word.
 *
 * Portable half of the atomic API: argument validation, plus the operations that are just another
 * one renamed (increment is an add of 1, clearing a bit is an AND with its complement). How a word
 * updates indivisibly is the port's business, which lets a core with LDREX/STREX stay lock-free
 * while one without pays interrupt latency instead.
 *
 * Each function validates and makes exactly one call into the port rather than layering on another
 * os_atomic_* - incrementing calls the port's add directly, not os_atomic_add - so no call here
 * costs more than the one port call the operation needs, and no argument is checked twice. The cost
 * is the NULL check written out repeatedly below.
 *
 * All of them return the value the word held BEFORE the operation.
 *
 * MISRA C:2012: every function here has a single exit (Rule 15.5), so each one declares its result
 * initialised to the value a rejected argument yields and assigns over it on the success path.
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

#include "os_internal.h"

/*
 * ***********************************************************************************************************
 * Ordering
 * ***********************************************************************************************************
 *
 * Atomicity and ORDER are different guarantees, and this file used to give only the first. The
 * asm's "memory" clobber is a COMPILER barrier; the hardware stays free to let another core see
 * these accesses out of order. That never bothered the kernel - its cross-core ordering comes from
 * the spinlock - but it breaks the most natural application use there is:
 *
 *     shared.payload = value;              on core 0
 *     os_atomic_set(&ready, 1);
 *
 *     if (os_atomic_get(&ready) == 1)      on core 1
 *     {
 *         use(shared.payload);             <- may be the OLD payload
 *     }
 *
 * So every read-modify-write is bracketed by a DMB - order, not completion, so not the spinlock's
 * DSB. The two operations that only READ - os_atomic_get and os_atomic_test_bit - take the AFTER
 * half alone, which is the standard mapping for a load: the barrier after it is what gives acquire
 * ordering, while the one before is release ordering and orders prior writes against a following
 * store that a pure load does not have. Every other entry point here exchanges, adds or compares,
 * so all of them keep both.
 *
 * Single-core builds pay nothing: both macros compile away, which matters in a layer this thin.
 */
#if (OS_CONFIG_CORE_COUNT > 1U)
#define OS_ATOMIC_ORDER_BEFORE()   OS_ARCH_DMB()
#define OS_ATOMIC_ORDER_AFTER()    OS_ARCH_DMB()
#else
#define OS_ATOMIC_ORDER_BEFORE()   do { } while (0)
#define OS_ATOMIC_ORDER_AFTER()    do { } while (0)
#endif

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

/* Number of bits in an os_atomic_t, and so the exclusive upper bound for every bit index. */
#define OS_ATOMIC_BITS  32U

/* The operations below index bits and swap whole words, both of which assume this width, and the
 * port's atomics are declared over a 32-bit word. */
OS_STATIC_ASSERT(sizeof(os_atomic_t) == 4U,
                 "the atomic operations assume a 32-bit word");

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Read the current value.
 *
 * @param[in] target  Word to read.
 * @return int32_t  Current value, or 0 for a NULL target.
 */
int32_t os_atomic_get(const os_atomic_t *target)
{
    int32_t value = 0;

    if (target != NULL)
    {
        /* A load only needs the acquire half of the fence pair: the barrier AFTER the read stops
         * later accesses moving before it. The barrier BEFORE it is release ordering, which orders
         * prior writes against a following STORE and does nothing for a pure load. */
        value = os_arch_atomic_load((const __IO int32_t *)target);
        OS_ATOMIC_ORDER_AFTER();
    }

    return value;
}

/******************************************************************************************************/
/**
 * @brief Store a value, returning the previous one.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Value to store.
 * @return int32_t  Value held before the store.
 */
int32_t os_atomic_set(os_atomic_t *target, int32_t value)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_exchange((__IO int32_t *)target, value);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Store 0, returning the previous value.
 *
 * @param[in,out] target  Word to clear.
 * @return int32_t  Value held before the clear.
 */
int32_t os_atomic_clear(os_atomic_t *target)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_exchange((__IO int32_t *)target, 0);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Add, returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to add.
 * @return int32_t  Value held before the addition.
 */
int32_t os_atomic_add(os_atomic_t *target, int32_t value)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_add((__IO int32_t *)target, value);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Subtract, returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Amount to subtract.
 * @return int32_t  Value held before the subtraction.
 */
int32_t os_atomic_sub(os_atomic_t *target, int32_t value)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_sub((__IO int32_t *)target, value);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Add 1, returning the previous value.
 *
 * @param[in,out] target  Word to increment.
 * @return int32_t  Value held before the increment.
 */
int32_t os_atomic_inc(os_atomic_t *target)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_add((__IO int32_t *)target, 1);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Subtract 1, returning the previous value.
 *
 * @param[in,out] target  Word to decrement.
 * @return int32_t  Value held before the decrement.
 */
int32_t os_atomic_dec(os_atomic_t *target)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_sub((__IO int32_t *)target, 1);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Bitwise OR, returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to set.
 * @return int32_t  Value held before the operation.
 */
int32_t os_atomic_or(os_atomic_t *target, int32_t value)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_or((__IO int32_t *)target, value);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Bitwise AND, returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Mask to keep.
 * @return int32_t  Value held before the operation.
 */
int32_t os_atomic_and(os_atomic_t *target, int32_t value)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_and((__IO int32_t *)target, value);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Bitwise XOR, returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Bits to flip.
 * @return int32_t  Value held before the operation.
 */
int32_t os_atomic_xor(os_atomic_t *target, int32_t value)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_xor((__IO int32_t *)target, value);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Bitwise NAND (~(old & value)), returning the previous value.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     value   Operand.
 * @return int32_t  Value held before the operation.
 */
int32_t os_atomic_nand(os_atomic_t *target, int32_t value)
{
    int32_t previous = 0;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        previous = os_arch_atomic_nand((__IO int32_t *)target, value);
        OS_ATOMIC_ORDER_AFTER();
    }

    return previous;
}

/******************************************************************************************************/
/**
 * @brief Compare-and-swap: store desired only if the word still holds expected.
 *
 * A false return is the answer the caller asked for: the word no longer held expected. Both
 * lock-free backends retry a spurious exclusive-access loss internally, so false here never means
 * "the port could not try" - but loop if only the final state matters, and re-read the value to
 * tell "someone else won" from the (now-unreachable) spurious case.
 *
 * @param[in,out] target    Word to update.
 * @param[in]     expected  Value the caller believes the word holds.
 * @param[in]     desired   Value to store if it still does.
 * @return bool  true if desired was stored.
 */
bool os_atomic_cas(os_atomic_t *target, int32_t expected, int32_t desired)
{
    bool swapped = false;

    OS_ASSERT(target != NULL);

    if (target != NULL)
    {
        OS_ATOMIC_ORDER_BEFORE();
        swapped = os_arch_atomic_cas((__IO int32_t *)target, expected, desired);
        OS_ATOMIC_ORDER_AFTER();
    }

    return swapped;
}

/******************************************************************************************************/
/**
 * @brief Test one bit.
 *
 * @param[in] target  Word to read.
 * @param[in] bit     Bit index, 0 to 31.
 * @return bool  true if the bit is set. false for a NULL target or an out-of-range index.
 */
bool os_atomic_test_bit(const os_atomic_t *target, uint32_t bit)
{
    bool is_set = false;

    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target != NULL) && (bit < OS_ATOMIC_BITS))
    {
        int32_t mask = (int32_t)(1UL << bit);

        /* The acquire half only, for the same reason as os_atomic_get: this reads and never
         * writes, and the barrier BEFORE a pure load orders prior writes against a following
         * STORE that does not exist here. The one AFTER is what stops the payload being read
         * before the bit that advertises it. */
        is_set = ((os_arch_atomic_load((const __IO int32_t *)target) & mask) != 0);

        OS_ATOMIC_ORDER_AFTER();
    }

    return is_set;
}

/******************************************************************************************************/
/**
 * @brief Set one bit, returning its previous state.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @return bool  true if the bit was already set.
 */
bool os_atomic_test_and_set_bit(os_atomic_t *target, uint32_t bit)
{
    bool was_set = false;

    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target != NULL) && (bit < OS_ATOMIC_BITS))
    {
        int32_t mask     = (int32_t)(1UL << bit);
        OS_ATOMIC_ORDER_BEFORE();
        int32_t previous = os_arch_atomic_or((__IO int32_t *)target, mask);
        OS_ATOMIC_ORDER_AFTER();
        was_set = ((previous & mask) != 0);
    }

    return was_set;
}

/******************************************************************************************************/
/**
 * @brief Clear one bit, returning its previous state.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @return bool  true if the bit was set before the call.
 */
bool os_atomic_test_and_clear_bit(os_atomic_t *target, uint32_t bit)
{
    bool was_set = false;

    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target != NULL) && (bit < OS_ATOMIC_BITS))
    {
        int32_t mask     = (int32_t)(1UL << bit);
        OS_ATOMIC_ORDER_BEFORE();
        int32_t previous = os_arch_atomic_and((__IO int32_t *)target, (int32_t)(~mask));
        OS_ATOMIC_ORDER_AFTER();
        was_set = ((previous & mask) != 0);
    }

    return was_set;
}

/******************************************************************************************************/
/**
 * @brief Set one bit.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @return None.
 */
void os_atomic_set_bit(os_atomic_t *target, uint32_t bit)
{
    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target != NULL) && (bit < OS_ATOMIC_BITS))
    {
        OS_ATOMIC_ORDER_BEFORE();

        (void)os_arch_atomic_or((__IO int32_t *)target, (int32_t)(1UL << bit));

        OS_ATOMIC_ORDER_AFTER();
    }
}

/******************************************************************************************************/
/**
 * @brief Clear one bit.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @return None.
 */
void os_atomic_clear_bit(os_atomic_t *target, uint32_t bit)
{
    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target != NULL) && (bit < OS_ATOMIC_BITS))
    {
        OS_ATOMIC_ORDER_BEFORE();

        (void)os_arch_atomic_and((__IO int32_t *)target, (int32_t)(~(1UL << bit)));

        OS_ATOMIC_ORDER_AFTER();
    }
}

/******************************************************************************************************/
/**
 * @brief Set one bit to the given state.
 *
 * @param[in,out] target  Word to update.
 * @param[in]     bit     Bit index, 0 to 31.
 * @param[in]     value   true to set the bit, false to clear it.
 * @return None.
 */
void os_atomic_set_bit_to(os_atomic_t *target, uint32_t bit, bool value)
{
    OS_ASSERT(target != NULL);
    OS_ASSERT(bit < OS_ATOMIC_BITS);

    if ((target != NULL) && (bit < OS_ATOMIC_BITS))
    {
        int32_t mask = (int32_t)(1UL << bit);

        OS_ATOMIC_ORDER_BEFORE();

        if (value)
        {
            (void)os_arch_atomic_or((__IO int32_t *)target, mask);
        }
        else
        {
            (void)os_arch_atomic_and((__IO int32_t *)target, (int32_t)(~mask));
        }

        OS_ATOMIC_ORDER_AFTER();
    }
}

#endif /* OS_CONFIG_ATOMIC_ENABLE */
