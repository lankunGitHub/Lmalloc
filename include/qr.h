#pragma once

// 嵌入式双向链表

/*
struct my_s{
    int x;
    qr(my_s) my_links;
}
*/
#define qr(a_type)                                                             \
    struct                                                                     \
    {                                                                          \
        a_type* qre_next;                                                      \
        a_type* qre_prev;                                                      \
    }

/*
struct my_s{
    int x;
    qr(my_s) my_links;
}

qr_new(my_s, my_links)
*/
#define qr_new(a_qr, a_field)                                                  \
    do                                                                         \
    {                                                                          \
        (a_qr)->a_field.qre_next = (a_qr);                                     \
        (a_qr)->a_field.qre_prev = (a_qr);                                     \
    } while (0)

/*

*/
#define qr_next(a_qr, a_field) ((a_qr)->a_field.qre_next)
#define qr_prev(a_qr, a_field) ((a_qr)->a_field.qre_prev)

/*    a -> a_1 -> ... -> a_n --
 *    ^                       |
 *    |------------------------
 *
 *    b -> b_1 -> ... -> b_n --
 *    ^                       |
 *    |------------------------
 *
 *
 *   a -> a_1 -> ... -> a_n -> b -> b_1 -> ... -> b_n --
 *   ^                                                 |
 *   |-------------------------------------------------|
 *
 *   qr_meld(a_qr, b_qr, my_links);
 */
#define qr_meld(a_qr_a, a_qr_b, a_field)                                       \
    do                                                                         \
    {                                                                          \
        (a_qr_b)->a_field.qre_prev->a_field.qre_next =                         \
            (a_qr_a)->a_field.qre_prev;                                        \
        (a_qr_a)->a_field.qre_prev = (a_qr_b)->a_field.qre_prev;               \
        (a_qr_b)->a_field.qre_prev =                                           \
            (a_qr_b)->a_field.qre_prev->a_field.qre_next;                      \
        (a_qr_a)->a_field.qre_prev->a_field.qre_next = (a_qr_a);               \
        (a_qr_b)->a_field.qre_prev->a_field.qre_next = (a_qr_b);               \
    } while (0)

/**
 * @brief 在 a_qrelm 之前插入 a_qr
 *
 * @param a_qrelm 目标节点
 * @param a_qr 要插入的节点
 * @param a_field 链表字段
 *
 * 该宏调用 qr_meld，将 a_qr 连接到 a_qrelm 之前，形成新的循环双向链表。
 */
#define qr_before_insert(a_qrelm, a_qr, a_field)                               \
    qr_meld((a_qrelm), (a_qr), a_field)

/**
 * @brief 在 a_qrelm 之后插入 a_qr
 *
 * @param a_qrelm 目标节点
 * @param a_qr 要插入的节点
 * @param a_field 链表字段
 *
 * 该宏调用 qr_before_insert，在 a_qrelm 的下一个节点之前插入
 * a_qr，等效于在当前节点后插入 a_qr。
 */
#define qr_after_insert(a_qrelm, a_qr, a_field)                                \
    qr_before_insert(qr_next(a_qrelm, a_field), (a_qr), a_field)

/**
 * @brief 拆分循环双向链表
 *
 * @param a_qr_a 链表的一部分
 * @param a_qr_b 另一部分
 * @param a_field 链表字段
 *
 * 该宏调用 qr_meld，将 a_qr_a 和 a_qr_b 拆分，使 a_qr_b
 * 成为新的独立循环双向链表。
 */
#define qr_split(a_qr_a, a_qr_b, a_field) qr_meld((a_qr_a), (a_qr_b), a_field)

/**
 * @brief 删除 a_qr 节点
 *
 * @param a_qr 目标节点
 * @param a_field 链表字段
 *
 * 该宏调用 qr_split，将 a_qr 从链表中移除，并使其成为独立的循环双向链表。
 */
#define qr_remove(a_qr, a_field)                                               \
    qr_split(qr_next(a_qr, a_field), (a_qr), a_field)

/**
 * @brief 遍历循环双向链表
 *
 * @param var 遍历过程中指向当前节点的变量
 * @param a_qr 链表起始节点
 * @param a_field 链表字段
 *
 * 该宏从 a_qr 开始遍历循环双向链表，直到回到起点。
 */
#define qr_foreach(var, a_qr, a_field)                                         \
    for ((var) = (a_qr); (var) != NULL;                                        \
         (var) =                                                               \
             (((var)->a_field.qre_next != (a_qr)) ? (var)->a_field.qre_next    \
                                                  : NULL))

#define qr_reverse_foreach(var, a_qr, a_field)                                 \
    for ((var) = ((a_qr) != NULL) ? qr_prev(a_qr, a_field) : NULL;             \
         (var) != NULL;                                                        \
         (var) = (((var) != (a_qr)) ? (var)->a_field.qre_prev : NULL))
