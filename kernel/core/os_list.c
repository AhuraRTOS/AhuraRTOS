/**
 * @file os_list.c
 * @brief Intrusive list utility implementation.
 *
 * Always compiled: the scheduler (ready lists, delay list) runs on these
 * lists, so this module has no configuration switch.
 *
 * @copyright (c) 2026 Ahura Project Contributors
 *            SPDX-License-Identifier: MIT
 *            See LICENSE.md in the project root for the full license text.
 */

/*
 * ***********************************************************************************************************
 * Includes
 * ***********************************************************************************************************
*/

#include "../ahura.h"

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Initialize list container.
 *
 * @param[in,out] list  List object.
 * @return None.
 */
void os_list_init(os_list_t *list)
{
    if (list != NULL)
    {
        list->head = NULL;
        list->tail = NULL;
    }
}

/******************************************************************************************************/
/**
 * @brief Check whether list is empty.
 *
 * @param[in] list  List object.
 * @return bool     True when list has no nodes.
 */
bool os_list_is_empty(const os_list_t *list)
{
    bool is_empty = true;

    if (list != NULL)
    {
        is_empty = (list->head == NULL);
    }

    return is_empty;
}

/******************************************************************************************************/
/**
 * @brief Push node at list tail.
 *
 * @param[in,out] list  List object.
 * @param[in,out] node  Node to append.
 * @return None.
 */
void os_list_push_back(os_list_t *list, os_list_node_t *node)
{
    if ((list != NULL) && (node != NULL))
    {
        node->next = NULL;
        node->prev = list->tail;

        if (list->tail != NULL)
        {
            list->tail->next = node;
        }
        else
        {
            list->head = node;
        }

        list->tail = node;
    }
}

/******************************************************************************************************/
/**
 * @brief Pop one node from list head.
 *
 * @param[in,out] list  List object.
 * @return os_list_node_t* Head node or NULL when empty.
 */
os_list_node_t* os_list_pop_front(os_list_t *list)
{
    os_list_node_t *node = NULL;

    if ((list != NULL) && (list->head != NULL))
    {
        node       = list->head;
        list->head = node->next;

        if (list->head != NULL)
        {
            list->head->prev = NULL;
        }
        else
        {
            list->tail = NULL;
        }

        node->next = NULL;
        node->prev = NULL;
    }

    return node;
}

/******************************************************************************************************/
/**
 * @brief Remove a node from anywhere in the list.
 *
 * A node that is not linked (detached: NULL neighbors and not the head) is
 * left untouched, so removing twice is harmless.
 *
 * @param[in,out] list  List object.
 * @param[in,out] node  Node to remove.
 * @return None.
 */
void os_list_remove(os_list_t *list, os_list_node_t *node)
{
    /* A detached node (NULL neighbours and not the head) is left alone, so removing
     * twice is harmless - expressed as one guarded block for a single exit. */
    if ((list != NULL) && (node != NULL) &&
        ((node->prev != NULL) || (node->next != NULL) || (list->head == node)))
    {
        if (node->prev != NULL)
        {
            node->prev->next = node->next;
        }
        else
        {
            list->head = node->next;
        }

        if (node->next != NULL)
        {
            node->next->prev = node->prev;
        }
        else
        {
            list->tail = node->prev;
        }

        node->next = NULL;
        node->prev = NULL;
    }
}

/******************************************************************************************************/
/**
 * @brief Insert a node before the given position.
 *
 * A NULL position appends at the tail, so a walk that found no successor
 * can pass its NULL cursor straight in.
 *
 * @param[in,out] list      List object.
 * @param[in,out] position  Node to insert in front of, or NULL for the tail.
 * @param[in,out] node      Node to insert.
 * @return None.
 */
void os_list_insert_before(os_list_t *list, os_list_node_t *position, os_list_node_t *node)
{
    if ((list != NULL) && (node != NULL))
    {
        if (position == NULL)
        {
            os_list_push_back(list, node);
        }
        else
        {
            node->next = position;
            node->prev = position->prev;

            if (position->prev != NULL)
            {
                position->prev->next = node;
            }
            else
            {
                list->head = node;
            }

            position->prev = node;
        }
    }
}
