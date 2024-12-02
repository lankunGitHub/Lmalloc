#pragma once

#include "qr.h"

/*
 * 定义链表头结构，包含指向第一个元素的指针。
 */
#define ql_head(a_type)                                                        \
    struct                                                                     \
    {                                                                          \
        a_type* qlh_first;                                                     \
    }

/*
 * 初始化链表头，使其指向 NULL。
 */
#define ql_head_initializer(a_head) {NULL}

/*
 * 定义链表元素，使用 qr() 结构。
 */
#define ql_elm(a_type) qr(a_type)

/*
 * 获取链表的第一个元素。
 */
#define ql_first(a_head) ((a_head)->qlh_first)

/*
 * 初始化链表，使其为空。
 */
#define ql_new(a_head)                                                         \
    do                                                                         \
    {                                                                          \
        ql_first(a_head) = NULL;                                               \
    } while (0)

/*
 * 将一个链表的头移动到另一个链表。
 */
#define ql_move(a_head_dest, a_head_src)                                       \
    do                                                                         \
    {                                                                          \
        ql_first(a_head_dest) = ql_first(a_head_src);                          \
        ql_new(a_head_src);                                                    \
    } while (0)

/*
 * 检查链表是否为空。
 */
#define ql_empty(a_head) (ql_first(a_head) == NULL)

/*
 * 初始化链表元素。
 */
#define ql_elm_new(a_elm, a_field) qr_new((a_elm), a_field)

/*
 * 获取链表的最后一个元素。
 */
#define ql_last(a_head, a_field)                                               \
    (ql_empty(a_head) ? NULL : qr_prev(ql_first(a_head), a_field))

/*
 * 获取链表中某元素的下一个元素。
 */
#define ql_next(a_head, a_elm, a_field)                                        \
    ((ql_last(a_head, a_field) != (a_elm)) ? qr_next((a_elm), a_field) : NULL)

/*
 * 获取链表中某元素的前一个元素。
 */
#define ql_prev(a_head, a_elm, a_field)                                        \
    ((ql_first(a_head) != (a_elm)) ? qr_prev((a_elm), a_field) : NULL)

/*
 * 在链表中某元素之前插入新元素。
 */
#define ql_before_insert(a_head, a_qlelm, a_elm, a_field)                      \
    do                                                                         \
    {                                                                          \
        qr_before_insert((a_qlelm), (a_elm), a_field);                         \
        if (ql_first(a_head) == (a_qlelm))                                     \
        {                                                                      \
            ql_first(a_head) = (a_elm);                                        \
        }                                                                      \
    } while (0)

/*
 * 在链表中某元素之后插入新元素。
 */
#define ql_after_insert(a_qlelm, a_elm, a_field)                               \
    qr_after_insert((a_qlelm), (a_elm), a_field)

/*
 * 在链表头部插入元素。
 */
#define ql_head_insert(a_head, a_elm, a_field)                                 \
    do                                                                         \
    {                                                                          \
        if (!ql_empty(a_head))                                                 \
        {                                                                      \
            qr_before_insert(ql_first(a_head), (a_elm), a_field);              \
        }                                                                      \
        ql_first(a_head) = (a_elm);                                            \
    } while (0)

/*
 * 在链表尾部插入元素。
 */
#define ql_tail_insert(a_head, a_elm, a_field)                                 \
    do                                                                         \
    {                                                                          \
        if (!ql_empty(a_head))                                                 \
        {                                                                      \
            qr_before_insert(ql_first(a_head), (a_elm), a_field);              \
        }                                                                      \
        ql_first(a_head) = qr_next((a_elm), a_field);                          \
    } while (0)

/*
 * 连接两个链表。
 */
#define ql_concat(a_head_a, a_head_b, a_field)                                 \
    do                                                                         \
    {                                                                          \
        if (ql_empty(a_head_a))                                                \
        {                                                                      \
            ql_move(a_head_a, a_head_b);                                       \
        }                                                                      \
        else if (!ql_empty(a_head_b))                                          \
        {                                                                      \
            qr_meld(ql_first(a_head_a), ql_first(a_head_b), a_field);          \
            ql_new(a_head_b);                                                  \
        }                                                                      \
    } while (0)

/*
 * 从链表中移除元素。
 */
#define ql_remove(a_head, a_elm, a_field)                                      \
    do                                                                         \
    {                                                                          \
        if (ql_first(a_head) == (a_elm))                                       \
        {                                                                      \
            ql_first(a_head) = qr_next(ql_first(a_head), a_field);             \
        }                                                                      \
        if (ql_first(a_head) != (a_elm))                                       \
        {                                                                      \
            qr_remove((a_elm), a_field);                                       \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            ql_new(a_head);                                                    \
        }                                                                      \
    } while (0)

/*
 * 删除链表头部元素。
 */
#define ql_head_remove(a_head, a_type, a_field)                                \
    do                                                                         \
    {                                                                          \
        a_type* t = ql_first(a_head);                                          \
        ql_remove((a_head), t, a_field);                                       \
    } while (0)

/*
 * 删除链表尾部元素。
 */
#define ql_tail_remove(a_head, a_type, a_field)                                \
    do                                                                         \
    {                                                                          \
        a_type* t = ql_last(a_head, a_field);                                  \
        ql_remove((a_head), t, a_field);                                       \
    } while (0)

/*
 * 拆分链表。
 */
#define ql_split(a_head_a, a_elm, a_head_b, a_field)                           \
    do                                                                         \
    {                                                                          \
        if (ql_first(a_head_a) == (a_elm))                                     \
        {                                                                      \
            ql_move(a_head_b, a_head_a);                                       \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            qr_split(ql_first(a_head_a), (a_elm), a_field);                    \
            ql_first(a_head_b) = (a_elm);                                      \
        }                                                                      \
    } while (0)

/*
 * 旋转链表，使头部元素变为尾部。
 */
#define ql_rotate(a_head, a_field)                                             \
    do                                                                         \
    {                                                                          \
        ql_first(a_head) = qr_next(ql_first(a_head), a_field);                 \
    } while (0)

/*
 * 遍历链表。
 */
#define ql_foreach(a_var, a_head, a_field)                                     \
    qr_foreach((a_var), ql_first(a_head), a_field)

/*
 * 逆序遍历链表。
 */
#define ql_reverse_foreach(a_var, a_head, a_field)                             \
    qr_reverse_foreach((a_var), ql_first(a_head), a_field)

#define TYPED_LIST(list_type, el_type, linkage)                                \
    typedef struct {                                                           \
        ql_head(el_type) head;                                                 \
    } list_type##_t;                                                           \
    static inline void list_type##_init(list_type##_t* list)                   \
    { ql_new(&list->head); }                                                   \
    static inline el_type* list_type##_first(const list_type##_t* list)        \
    { return ql_first(&list->head); }                                          \
    static inline el_type* list_type##_last(const list_type##_t* list)         \
    { return ql_last(&list->head, linkage); }                                  \
    static inline void list_type##_append(list_type##_t* list, el_type* item)  \
    { ql_elm_new(item, linkage); ql_last(&list->head, linkage) ?               \
        (ql_last(&list->head, linkage)->linkage.ql_link = item) :              \
        (list->head.qlh_first = item); }                                       \
    static inline void list_type##_prepend(list_type##_t* list, el_type* item) \
    { ql_elm_new(item, linkage); item->linkage.ql_link = list->head.qlh_first; \
      list->head.qlh_first = item; }                                           \
    static inline void list_type##_replace(                                    \
        list_type##_t* list, el_type* to_remove, el_type* to_insert)           \
    { el_type* prev = NULL; el_type* cur = list->head.qlh_first;               \
      while (cur && cur != to_remove) { prev = cur; cur = cur->linkage.ql_link; }\
      if (cur) { if (prev) prev->linkage.ql_link = to_insert;                  \
        else list->head.qlh_first = to_insert;                                 \
        to_insert->linkage.ql_link = cur->linkage.ql_link; } }                 \
    static inline void list_type##_remove(list_type##_t* list, el_type* item)  \
    { el_type* prev = NULL; el_type* cur = list->head.qlh_first;               \
      while (cur && cur != item) { prev = cur; cur = cur->linkage.ql_link; }   \
      if (cur) { if (prev) prev->linkage.ql_link = cur->linkage.ql_link;       \
        else list->head.qlh_first = cur->linkage.ql_link; } }                  \
    static inline bool list_type##_empty(list_type##_t* list)                  \
    { return ql_empty(&list->head); }                                          \
    static inline void list_type##_concat(list_type##_t* list_a,               \
                                         list_type##_t* list_b)                \
    { if (ql_empty(&list_a->head)) list_a->head.qlh_first = list_b->head.qlh_first;\
      else ql_last(&list_a->head, linkage)->linkage.ql_link = list_b->head.qlh_first;\
      list_b->head.qlh_first = NULL; }
/* end of ql.h */
