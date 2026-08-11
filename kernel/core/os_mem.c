/**
 * @file os_mem.c
 * @brief Kernel heap: os_mem_alloc/os_mem_free over a static heap array.
 *
 * First-fit allocator with an address-ordered free list and coalescing of
 * adjacent free blocks, so mixed-size alloc/free patterns do not fragment
 * the heap permanently. All operations run inside the kernel critical
 * section: safe from tasks and (though discouraged) from interrupts.
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

#if (OS_CONFIG_ALLOC_ENABLE == 1U)

/*
 * ***********************************************************************************************************
 * Macros
 * ***********************************************************************************************************
*/

#define OS_MEM_ALIGNMENT       8U
#define OS_MEM_ALIGN_MSK       ((size_t)(OS_MEM_ALIGNMENT - 1U))
#define OS_MEM_ALLOCATED_MSK   ((size_t)1 << ((sizeof(size_t) * 8U) - 1U))
#define OS_MEM_HEADER_SIZE     ((sizeof(os_mem_block_t) + OS_MEM_ALIGN_MSK) & ~OS_MEM_ALIGN_MSK)
#define OS_MEM_MIN_BLOCK_SIZE  (OS_MEM_HEADER_SIZE + OS_MEM_ALIGNMENT)

#if (OS_CONFIG_HEAP_SIZE < 64U)
#error "OS_CONFIG_HEAP_SIZE is too small to hold the allocator bookkeeping."
#endif

/*
 * ***********************************************************************************************************
 * Types
 * ***********************************************************************************************************
*/

/* Every heap block starts with this header; size covers header + payload and
 * carries the allocated flag in its top bit. */
typedef struct os_mem_block
{
    struct os_mem_block *next;
    size_t              size;

} os_mem_block_t;

/*
 * ***********************************************************************************************************
 * Global variables
 * ***********************************************************************************************************
*/

static uint8_t         os_mem_heap[OS_CONFIG_HEAP_SIZE];
static os_mem_block_t  os_mem_start;
static os_mem_block_t  *os_mem_end            = NULL;
static size_t          os_mem_free_bytes      = 0U;
static size_t          os_mem_min_free_bytes  = 0U;

/*
 * ***********************************************************************************************************
 * Private function prototypes
 * ***********************************************************************************************************
*/

static void os_mem_init(void);
static void os_mem_block_insert(os_mem_block_t *block);

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Allocate memory from the kernel heap.
 *
 * @param[in] size  Requested payload size in bytes.
 * @return void*    8-byte aligned memory, or NULL when size is 0 or no fitting block exists.
 */
void* os_mem_alloc(size_t size)
{
    void   *memory = NULL;
    size_t  need;

    /* Whole-block size: header + payload rounded up to the alignment. The
     * top bit is the allocated flag, so a size that reaches it (or wraps)
     * can never be satisfied. Computed for a zero size too, then discarded by
     * the guard below, so that this stays a single expression with one exit. */
    need = OS_MEM_HEADER_SIZE + ((size + OS_MEM_ALIGN_MSK) & ~OS_MEM_ALIGN_MSK);

    if ((size != 0U) && (need >= size) && ((need & OS_MEM_ALLOCATED_MSK) == 0U))
    {
        os_mem_block_t *prev;
        os_mem_block_t *block;

        os_critical_enter();

        if (os_mem_end == NULL)
        {
            os_mem_init();
        }

        /* First fit: the free list is address ordered and ends at the marker. */
        prev  = &os_mem_start;
        block = os_mem_start.next;
        while ((block != os_mem_end) && (block->size < need))
        {
            prev  = block;
            block = block->next;
        }

        if (block != os_mem_end)
        {
            os_mem_block_t *next_free = block->next; /* capture before either relink below */

            /* Split when the leftover still makes a usable free block. The
             * remainder's position in the address-ordered list is already known
             * (between prev and next_free) so it is relinked directly instead of
             * re-walking the whole free list via os_mem_block_insert: neither
             * merge could ever fire here (the predecessor is the block just
             * removed, and a touching successor would already have been merged
             * when this free block was inserted). */
            if ((block->size - need) >= OS_MEM_MIN_BLOCK_SIZE)
            {
                os_mem_block_t *remainder = (os_mem_block_t *)(void *)((uint8_t *)block + need);

                remainder->size = block->size - need;
                remainder->next = next_free;
                prev->next      = remainder;
                block->size     = need;
            }
            else
            {
                prev->next = next_free;
            }

            os_mem_free_bytes -= block->size;
            if (os_mem_free_bytes < os_mem_min_free_bytes)
            {
                os_mem_min_free_bytes = os_mem_free_bytes;
            }

            block->size |= OS_MEM_ALLOCATED_MSK;
            block->next = NULL;

            memory = (void *)((uint8_t *)block + OS_MEM_HEADER_SIZE);
        }

        os_critical_exit();
    }

    return memory;
}

/******************************************************************************************************/
/**
 * @brief Return memory obtained from os_mem_alloc to the kernel heap.
 *
 * NULL, foreign and double-freed pointers are ignored.
 *
 * @param[in] memory  Pointer previously returned by os_mem_alloc.
 * @return None.
 */
void os_mem_free(void *memory)
{
    /* The payload must leave room for its own header BELOW it, not merely lie
     * somewhere in the heap: a pointer into the first header's worth of bytes
     * would pass a plain range check and then be validated through a header
     * that sits before the array entirely - an out-of-bounds read that links a
     * fabricated block into the free list whenever the bytes preceding the
     * heap happen to look allocated. Every real payload starts at least
     * OS_MEM_HEADER_SIZE into the heap, so nothing legitimate is rejected. */
    if ((memory != NULL) && (os_mem_end != NULL) &&
        ((uint8_t *)memory >= &os_mem_heap[OS_MEM_HEADER_SIZE]) &&
        ((uint8_t *)memory < &os_mem_heap[OS_CONFIG_HEAP_SIZE]))
    {
        os_mem_block_t *block = (os_mem_block_t *)(void *)((uint8_t *)memory - OS_MEM_HEADER_SIZE);

        os_critical_enter();

        /* An os_mem block carries the allocated flag and a cleared link.
         * Validated and cleared inside the critical section: a racing free of
         * the same pointer (a higher-priority ISR, or another core) must never
         * observe the flag still set after this check passes - otherwise both
         * callers complete the free and the block gets linked into the free
         * list twice (self-loop, or a live allocation freed out from under its
         * owner). */
        if (((block->size & OS_MEM_ALLOCATED_MSK) != 0U) && (block->next == NULL))
        {
            block->size &= ~OS_MEM_ALLOCATED_MSK;
            os_mem_free_bytes += block->size;
            os_mem_block_insert(block);
        }

        os_critical_exit();
    }
}

/******************************************************************************************************/
/**
 * @brief Get the number of bytes currently free in the kernel heap.
 *
 * @return size_t  Free bytes (including per-block headers).
 */
size_t os_mem_free_get(void)
{
    size_t free_bytes;

    os_critical_enter();

    if (os_mem_end == NULL)
    {
        os_mem_init();
    }

    free_bytes = os_mem_free_bytes;

    os_critical_exit();

    return free_bytes;
}

/******************************************************************************************************/
/**
 * @brief Get the smallest amount of free heap ever observed (worst case since boot).
 *
 * @return size_t  Minimum free bytes watermark.
 */
size_t os_mem_watermark_get(void)
{
    size_t min_free_bytes;

    os_critical_enter();

    if (os_mem_end == NULL)
    {
        os_mem_init();
    }

    min_free_bytes = os_mem_min_free_bytes;

    os_critical_exit();

    return min_free_bytes;
}

/*
 * ***********************************************************************************************************
 * Private function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Lazily set up the free list: one block spanning the heap plus the end marker.
 *
 * Runtime alignment of the array bounds keeps the heap storage free of
 * compiler-specific attributes. Called inside the critical section.
 *
 * @return None.
 */
static void os_mem_init(void)
{
    uintptr_t       heap_start = ((uintptr_t)os_mem_heap + OS_MEM_ALIGN_MSK) & ~(uintptr_t)OS_MEM_ALIGN_MSK;
    uintptr_t       heap_end   = ((uintptr_t)os_mem_heap + OS_CONFIG_HEAP_SIZE - OS_MEM_HEADER_SIZE) &
                                 ~(uintptr_t)OS_MEM_ALIGN_MSK;
    os_mem_block_t *first      = (os_mem_block_t *)heap_start;

    os_mem_end       = (os_mem_block_t *)heap_end;
    os_mem_end->next = NULL;
    os_mem_end->size = 0U;

    first->size = (size_t)(heap_end - heap_start);
    first->next = os_mem_end;

    os_mem_start.next = first;
    os_mem_start.size = 0U;

    os_mem_free_bytes     = first->size;
    os_mem_min_free_bytes = first->size;
}

/******************************************************************************************************/
/**
 * @brief Insert a free block into the address-ordered list, merging with touching neighbors.
 *
 * Called inside the critical section; the block's allocated flag must be clear.
 *
 * @param[in] block  Free block to insert.
 * @return None.
 */
static void os_mem_block_insert(os_mem_block_t *block)
{
    os_mem_block_t *iter;
    uint8_t        *address;

    /* Find the free block we insert after (list is address ordered). */
    for (iter = &os_mem_start; iter->next < block; iter = iter->next)
    {
    }

    /* Merge with the predecessor when they touch (never the list head:
     * it lives outside the heap, so the addresses cannot line up). */
    address = (uint8_t *)iter;
    if ((address + iter->size) == (uint8_t *)block)
    {
        iter->size += block->size;
        block      = iter;
    }

    /* Merge with the successor when they touch; the end marker only ever
     * becomes the link target, never part of a block. */
    address = (uint8_t *)block;
    if ((address + block->size) == (uint8_t *)iter->next)
    {
        if (iter->next != os_mem_end)
        {
            block->size += iter->next->size;
            block->next  = iter->next->next;
        }
        else
        {
            block->next = os_mem_end;
        }
    }
    else
    {
        block->next = iter->next;
    }

    if (iter != block)
    {
        iter->next = block;
    }
}

#endif /* OS_CONFIG_ALLOC_ENABLE */
