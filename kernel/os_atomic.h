/**
 * @file os_atomic.h
 * @brief Atomic operations on a single 32-bit word (os_atomic.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_ATOMIC_H
#define OS_ATOMIC_H

#include "os_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ***********************************************************************************************************
 * Atomics            - OS_CONFIG_ATOMIC_ENABLE
 * ***********************************************************************************************************
*/

#if (OS_CONFIG_ATOMIC_ENABLE == 1U)

/******************************************************************************************************/
/**
 * @brief Atomic word: the type of every variable the os_atomic_* operations act on.
 *
 * Declare one as os_atomic_t, never as a plain int32_t, and touch it only through os_atomic_*.
 * An ordinary read or write of the same word is not ordered against these calls, which is the
 * usual way a counter that "uses atomics" still loses updates.
 */
typedef int32_t os_atomic_t;

/** Initializer for an os_atomic_t: static os_atomic_t counter = OS_ATOMIC_INIT(0); */
#define OS_ATOMIC_INIT(value)  ((os_atomic_t)(value))

/******************************************************************************************************/
/*
 * Atomic operations on an os_atomic_t (see the type above). Safe from tasks and from ISRs.
 *
 * Every read-modify-write returns the value the word held BEFORE the operation, so os_atomic_inc
 * returning 4 means the counter now reads 5.
 *
 * Where the instruction set has an exclusive load/store pair these are lock-free. Where it does
 * not, the port briefly excludes interrupts instead, which costs interrupt latency and is worth
 * knowing before putting one on a hot path. See doc/api.md for which cores fall on which side.
 */

/******************************************************************************************************/
/**
 * @brief Read the current value.
 */
int32_t os_atomic_get(const os_atomic_t *target);

/******************************************************************************************************/
/**
 * @brief Store a value, returning the previous one.
 */
int32_t os_atomic_set(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Store 0, returning the previous value.
 */
int32_t os_atomic_clear(os_atomic_t *target);

/******************************************************************************************************/
/**
 * @brief Add, returning the previous value.
 */
int32_t os_atomic_add(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Subtract, returning the previous value.
 */
int32_t os_atomic_sub(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Add 1, returning the previous value.
 */
int32_t os_atomic_inc(os_atomic_t *target);

/******************************************************************************************************/
/**
 * @brief Subtract 1, returning the previous value.
 */
int32_t os_atomic_dec(os_atomic_t *target);

/******************************************************************************************************/
/**
 * @brief Bitwise OR, returning the previous value.
 */
int32_t os_atomic_or(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Bitwise AND, returning the previous value.
 */
int32_t os_atomic_and(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Bitwise XOR, returning the previous value.
 */
int32_t os_atomic_xor(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Bitwise NAND (~(old & value)), returning the previous value.
 */
int32_t os_atomic_nand(os_atomic_t *target, int32_t value);

/******************************************************************************************************/
/**
 * @brief Compare-and-swap: store desired only if the word still holds expected.
 */
bool os_atomic_cas(os_atomic_t *target, int32_t expected, int32_t desired);

/******************************************************************************************************/
/**
 * @brief Test one bit.
 */
bool os_atomic_test_bit(const os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Set one bit, returning its previous state.
 */
bool os_atomic_test_and_set_bit(os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Clear one bit, returning its previous state.
 */
bool os_atomic_test_and_clear_bit(os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Set one bit.
 */
void os_atomic_set_bit(os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Clear one bit.
 */
void os_atomic_clear_bit(os_atomic_t *target, uint32_t bit);

/******************************************************************************************************/
/**
 * @brief Set one bit to the given state.
 */
void os_atomic_set_bit_to(os_atomic_t *target, uint32_t bit, bool value);

#endif /* OS_CONFIG_ATOMIC_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* OS_ATOMIC_H */
