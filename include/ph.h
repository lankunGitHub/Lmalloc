#pragma once

#include "bit_util.h"

// 配对堆，是一个多叉树，相比于普通堆能更好支持合并操作

typedef int (*ph_cmp_t)(void*, void*);

typedef struct phn_link_s phn_link_t;
struct phn_link_s
{
    void* prev;
    void* next;
    void* lchild;
};

typedef struct ph_s ph_t;
struct ph_s
{
    void* root;      // 根节点
    size_t auxcount; // 统计自上次合并以来插入的元素数量，辅助合并
};

static inline phn_link_t* phn_link_get(void* phn, size_t offset)
{
    return (phn_link_t*)(((char*)phn) + offset);
}

static inline void phn_link_init(void* phn, size_t offset)
{
    phn_link_get(phn, offset)->prev = NULL;
    phn_link_get(phn, offset)->next = NULL;
    phn_link_get(phn, offset)->lchild = NULL;
}

static inline void* phn_lchild_get(void* phn, size_t offset)
{
    return phn_link_get(phn, offset)->lchild;
}

static inline void phn_lchild_set(void* phn, void* lchild, size_t offset)
{
    phn_link_get(phn, offset)->lchild = lchild;
}

static inline void* phn_next_get(void* phn, size_t offset)
{
    return phn_link_get(phn, offset)->next;
}

static inline void phn_next_set(void* phn, void* next, size_t offset)
{
    phn_link_get(phn, offset)->next = next;
}

static inline void* phn_prev_get(void* phn, size_t offset)
{
    return phn_link_get(phn, offset)->prev;
}

static inline void phn_prev_set(void* phn, void* prev, size_t offset)
{
    phn_link_get(phn, offset)->prev = prev;
}

static inline void
phn_merge_ordered(void* phn0, void* phn1, size_t offset, ph_cmp_t cmp)
{
    void* phn0child;

    assert(phn0 != NULL);
    assert(phn1 != NULL);
    assert(cmp(phn0, phn1) <= 0);

    phn_prev_set(phn1, phn0, offset);
    phn0child = phn_lchild_get(phn0, offset);
    phn_next_set(phn1, phn0child, offset);
    if (phn0child != NULL)
    {
        phn_prev_set(phn0child, phn1, offset);
    }
    phn_lchild_set(phn0, phn1, offset);
}

static inline void*
phn_merge(void* phn0, void* phn1, size_t offset, ph_cmp_t cmp)
{
    void* result;
    if (phn0 == NULL)
    {
        result = phn1;
    }
    else if (phn1 == NULL)
    {
        result = phn0;
    }
    else if (cmp(phn0, phn1) < 0)
    {
        phn_merge_ordered(phn0, phn1, offset, cmp);
        result = phn0;
    }
    else
    {
        phn_merge_ordered(phn1, phn0, offset, cmp);
        result = phn1;
    }
    return result;
}

static inline void* phn_merge_siblings(void* phn, size_t offset, ph_cmp_t cmp)
{
    void* head = NULL;
    void* tail = NULL;
    void* phn0 = phn;
    void* phn1 = phn_next_get(phn0, offset);

    if (phn1 == NULL)
    {
        return phn0;
    }

    void* phnrest = phn_next_get(phn1, offset);
    if (phnrest != NULL)
    {
        phn_prev_set(phnrest, NULL, offset);
    }
    phn_prev_set(phn0, NULL, offset);
    phn_next_set(phn0, NULL, offset);
    phn_prev_set(phn1, NULL, offset);
    phn_next_set(phn1, NULL, offset);
    phn0 = phn_merge(phn0, phn1, offset, cmp);
    head = tail = phn0;
    phn0 = phnrest;
    while (phn0 != NULL)
    {
        phn1 = phn_next_get(phn0, offset);
        if (phn1 != NULL)
        {
            phnrest = phn_next_get(phn1, offset);
            if (phnrest != NULL)
            {
                phn_prev_set(phnrest, NULL, offset);
            }
            phn_prev_set(phn0, NULL, offset);
            phn_next_set(phn0, NULL, offset);
            phn_prev_set(phn1, NULL, offset);
            phn_next_set(phn1, NULL, offset);
            phn0 = phn_merge(phn0, phn1, offset, cmp);
            phn_next_set(tail, phn0, offset);
            tail = phn0;
            phn0 = phnrest;
        }
        else
        {
            phn_next_set(tail, phn0, offset);
            tail = phn0;
            phn0 = NULL;
        }
    }
    phn0 = head;
    phn1 = phn_next_get(phn0, offset);
    if (phn1 != NULL)
    {
        while (true)
        {
            head = phn_next_get(phn1, offset);
            assert(phn_prev_get(phn0, offset) == NULL);
            phn_next_set(phn0, NULL, offset);
            assert(phn_prev_get(phn1, offset) == NULL);
            phn_next_set(phn1, NULL, offset);
            phn0 = phn_merge(phn0, phn1, offset, cmp);
            if (head == NULL)
            {
                break;
            }
            phn_next_set(tail, phn0, offset);
            tail = phn0;
            phn0 = head;
            phn1 = phn_next_get(phn0, offset);
        }
    }

    return phn0;
}

static inline void ph_merge_aux(ph_t* ph, size_t offset, ph_cmp_t cmp)
{
    ph->auxcount = 0;
    void* phn = phn_next_get(ph->root, offset);
    if (phn != NULL)
    {
        phn_prev_set(ph->root, NULL, offset);
        phn_next_set(ph->root, NULL, offset);
        phn_prev_set(phn, NULL, offset);
        phn = phn_merge_siblings(phn, offset, cmp);
        assert(phn_next_get(phn, offset) == NULL);
        phn_merge_ordered(ph->root, phn, offset, cmp);
    }
}

static inline void* ph_merge_children(void* phn, size_t offset, ph_cmp_t cmp)
{
    void* result;
    void* lchild = phn_lchild_get(phn, offset);
    if (lchild == NULL)
    {
        result = NULL;
    }
    else
    {
        result = phn_merge_siblings(lchild, offset, cmp);
    }
    return result;
}

static inline void ph_new(ph_t* ph)
{
    ph->root = NULL;
    ph->auxcount = 0;
}

static inline bool ph_empty(ph_t* ph) { return ph->root == NULL; }

static inline void* ph_first(ph_t* ph, size_t offset, ph_cmp_t cmp)
{
    if (ph->root == NULL)
    {
        return NULL;
    }
    ph_merge_aux(ph, offset, cmp);
    return ph->root;
}

static inline void* ph_any(ph_t* ph, size_t offset)
{
    if (ph->root == NULL)
    {
        return NULL;
    }
    void* aux = phn_next_get(ph->root, offset);
    if (aux != NULL)
    {
        return aux;
    }
    return ph->root;
}

static inline bool ph_try_aux_merge_pair(ph_t* ph, size_t offset, ph_cmp_t cmp)
{
    assert(ph->root != NULL);
    void* phn0 = phn_next_get(ph->root, offset);
    if (phn0 == NULL)
    {
        return true;
    }
    void* phn1 = phn_next_get(phn0, offset);
    if (phn1 == NULL)
    {
        return true;
    }
    void* next_phn1 = phn_next_get(phn1, offset);
    phn_next_set(phn0, NULL, offset);
    phn_prev_set(phn0, NULL, offset);
    phn_next_set(phn1, NULL, offset);
    phn_prev_set(phn1, NULL, offset);
    phn0 = phn_merge(phn0, phn1, offset, cmp);
    phn_next_set(phn0, next_phn1, offset);
    if (next_phn1 != NULL)
    {
        phn_prev_set(next_phn1, phn0, offset);
    }
    phn_next_set(ph->root, phn0, offset);
    phn_prev_set(phn0, ph->root, offset);
    return next_phn1 == NULL;
}

static inline void ph_insert(ph_t* ph, void* phn, size_t offset, ph_cmp_t cmp)
{
    phn_link_init(phn, offset);

    if (ph->root == NULL)
    {
        ph->root = phn;
        return;
    }

    if (cmp(phn, ph->root) < 0)
    {
        phn_lchild_set(phn, ph->root, offset);
        phn_prev_set(ph->root, phn, offset);
        ph->root = phn;
        ph->auxcount = 0;
        return;
    }

    phn_next_set(phn, phn_next_get(ph->root, offset), offset);
    if (phn_next_get(ph->root, offset) != NULL)
    {
        phn_prev_set(phn_next_get(ph->root, offset), phn, offset);
    }
    phn_prev_set(phn, ph->root, offset);
    phn_next_set(ph->root, phn, offset);

    ph->auxcount++;
    unsigned nmerges = ffs_zu(ph->auxcount);
    bool done = false;
    for (unsigned i = 0; i < nmerges && !done; i++)
    {
        done = ph_try_aux_merge_pair(ph, offset, cmp);
    }
}

static inline void* ph_remove_first(ph_t* ph, size_t offset, ph_cmp_t cmp)
{
    void* ret;

    if (ph->root == NULL)
    {
        return NULL;
    }
    ph_merge_aux(ph, offset, cmp);
    ret = ph->root;
    ph->root = ph_merge_children(ph->root, offset, cmp);

    return ret;
}

static inline void ph_remove(ph_t* ph, void* phn, size_t offset, ph_cmp_t cmp)
{
    if (ph->root == phn)
    {
        ph_merge_aux(ph, offset, cmp);
        ph->root = ph_merge_children(phn, offset, cmp);
        return;
    }

    void* prev = phn_prev_get(phn, offset);
    void* next = phn_next_get(phn, offset);

    void* replace = ph_merge_children(phn, offset, cmp);
    if (replace != NULL)
    {
        phn_next_set(replace, next, offset);
        if (next != NULL)
        {
            phn_prev_set(next, replace, offset);
        }

        next = replace;
    }

    if (next != NULL)
    {
        phn_prev_set(next, prev, offset);
    }

    assert(prev != NULL);
    if (phn_lchild_get(prev, offset) == phn)
    {
        phn_lchild_set(prev, next, offset);
    }
    else
    {
        phn_next_set(prev, next, offset);
    }
}

#define ph_structs(a_prefix, a_type)                                           \
    typedef struct                                                             \
    {                                                                          \
        phn_link_t link;                                                       \
    } a_prefix##_link_t;                                                       \
                                                                               \
    typedef struct                                                             \
    {                                                                          \
        ph_t ph;                                                               \
    } a_prefix##_t;

#define ph_proto(a_attr, a_prefix, a_type)                                     \
                                                                               \
    a_attr void a_prefix##_new(a_prefix##_t* ph);                              \
    a_attr bool a_prefix##_empty(a_prefix##_t* ph);                            \
    a_attr a_type* a_prefix##_first(a_prefix##_t* ph);                         \
    a_attr a_type* a_prefix##_any(a_prefix##_t* ph);                           \
    a_attr void a_prefix##_insert(a_prefix##_t* ph, a_type* phn);              \
    a_attr a_type* a_prefix##_remove_first(a_prefix##_t* ph);                  \
    a_attr void a_prefix##_remove(a_prefix##_t* ph, a_type* phn);              \
    a_attr a_type* a_prefix##_remove_any(a_prefix##_t* ph);

#define ph_gen(a_attr, a_prefix, a_type, a_field, a_cmp)                       \
    static inline int a_prefix##_ph_cmp(void* a, void* b)                      \
    {                                                                          \
        return a_cmp((a_type*)a, (a_type*)b);                                  \
    }                                                                          \
                                                                               \
    a_attr void a_prefix##_new(a_prefix##_t* ph) { ph_new(&ph->ph); }          \
                                                                               \
    a_attr bool a_prefix##_empty(a_prefix##_t* ph)                             \
    {                                                                          \
        return ph_empty(&ph->ph);                                              \
    }                                                                          \
                                                                               \
    a_attr a_type* a_prefix##_first(a_prefix##_t* ph)                          \
    {                                                                          \
        return ph_first(                                                       \
            &ph->ph, offsetof(a_type, a_field), &a_prefix##_ph_cmp);           \
    }                                                                          \
                                                                               \
    a_attr a_type* a_prefix##_any(a_prefix##_t* ph)                            \
    {                                                                          \
        return ph_any(&ph->ph, offsetof(a_type, a_field));                     \
    }                                                                          \
                                                                               \
    a_attr void a_prefix##_insert(a_prefix##_t* ph, a_type* phn)               \
    {                                                                          \
        ph_insert(&ph->ph, phn, offsetof(a_type, a_field), a_prefix##_ph_cmp); \
    }                                                                          \
                                                                               \
    a_attr a_type* a_prefix##_remove_first(a_prefix##_t* ph)                   \
    {                                                                          \
        return ph_remove_first(                                                \
            &ph->ph, offsetof(a_type, a_field), a_prefix##_ph_cmp);            \
    }                                                                          \
                                                                               \
    a_attr void a_prefix##_remove(a_prefix##_t* ph, a_type* phn)               \
    {                                                                          \
        ph_remove(&ph->ph, phn, offsetof(a_type, a_field), a_prefix##_ph_cmp); \
    }                                                                          \
                                                                               \
    a_attr a_type* a_prefix##_remove_any(a_prefix##_t* ph)                     \
    {                                                                          \
        a_type* ret = a_prefix##_any(ph);                                      \
        if (ret != NULL)                                                       \
        {                                                                      \
            a_prefix##_remove(ph, ret);                                        \
        }                                                                      \
        return ret;                                                            \
    }
/* end of ph.h */
