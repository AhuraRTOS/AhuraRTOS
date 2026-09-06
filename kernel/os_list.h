/**
 * @file os_list.h
 * @brief Intrusive doubly-linked list (os_list.c).
 *
 * Public API. Include <ahura.h>, which includes this and every other module header; this file is
 * not meant to be included on its own.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: GPL-3.0-or-later
 *            See LICENSE in the project root for the full license text.
 */

#ifndef OS_LIST_H
#define OS_LIST_H

#include "os_types.h"

/*
 * ***********************************************************************************************************
 * Intrusive list
 * ***********************************************************************************************************
 *
 * Always available: the scheduler and the blocking primitives run on these lists. Declared before
 * PART 2 because the kernel objects there embed waiter lists.
*/

/******************************************************************************************************/
/**
 * @brief Intrusive list node object.
 */
typedef struct os_list_node
{
    struct os_list_node *next;
    struct os_list_node *prev;

} os_list_node_t;

/******************************************************************************************************/
/**
 * @brief Intrusive list container.
 */
typedef struct
{
    os_list_node_t *head;
    os_list_node_t *tail;

} os_list_t;

/******************************************************************************************************/
/**
 * @brief Initialize list container.
 */
void os_list_init(os_list_t *list);

/******************************************************************************************************/
/**
 * @brief Check whether list is empty.
 */
bool os_list_is_empty(const os_list_t *list);

/******************************************************************************************************/
/**
 * @brief Push node at list tail.
 */
void os_list_push_back(os_list_t *list, os_list_node_t *node);

/******************************************************************************************************/
/**
 * @brief Pop one node from list head.
 */
os_list_node_t* os_list_pop_front(os_list_t *list);

/******************************************************************************************************/
/**
 * @brief Remove a node from anywhere in the list (detached nodes are ignored).
 */
void os_list_remove(os_list_t *list, os_list_node_t *node);

/******************************************************************************************************/
/**
 * @brief Insert a node before the given position (NULL position appends at the tail).
 */
void os_list_insert_before(os_list_t *list, os_list_node_t *position, os_list_node_t *node);

#endif /* OS_LIST_H */
