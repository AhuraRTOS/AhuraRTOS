/**
 * @file os_main_list.c
 * @brief Ahura kernel example: intrusive doubly-linked list (os_list_*).
 *
 * The list module has no OS_CONFIG_ switch (the scheduler itself runs on
 * it, so it is always compiled in) and is "intrusive": the link node is
 * embedded inside the owning struct instead of the list allocating its own
 * wrapper, so no heap is needed. Builds a small list, removes and
 * re-inserts one item, then drains it front to back. Copy this file into
 * the application source tree as os_main.c to run it.
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

#include "ahura.h"

#include <stdio.h>

/*
 * ***********************************************************************************************************
 * Types
 * ***********************************************************************************************************
*/

typedef struct
{
    os_list_node_t node; /**< Must be first only by convention - os_list_* never assumes layout. */
    int            value;

} item_t;

/*
 * ***********************************************************************************************************
 * Public function implementations
 * ***********************************************************************************************************
*/

/******************************************************************************************************/
/**
 * @brief Default application task body: builds, edits and drains a small intrusive list.
 *
 * @return None.
 */
void os_main(void)
{
    static item_t items[4];
    os_list_t     list;
    uint32_t      i;

    os_list_init(&list);
    printf("[list] freshly initialized, empty=%d\r\n", (int)os_list_is_empty(&list));

    for (i = 0U; i < 4U; i++)
    {
        items[i].value = (int)i;
        os_list_push_back(&list, &items[i].node);
    }
    printf("[list] pushed 4 items, empty=%d\r\n", (int)os_list_is_empty(&list));

    /* Detach item 2 from wherever it lives, then re-insert it right before
     * the current head - os_list_remove/insert_before work on any node, not
     * just the ends, which is the whole point of a doubly-linked list. */
    os_list_remove(&list, &items[2].node);
    os_list_insert_before(&list, list.head, &items[2].node);

    while (!os_list_is_empty(&list))
    {
        item_t *front = (item_t *)os_list_pop_front(&list);

        printf("[list] popped value=%d\r\n", front->value);
    }
    printf("[list] drained, empty=%d\r\n", (int)os_list_is_empty(&list));

    while (1)
    {
        os_delay_ms(1000U);
    }
}
