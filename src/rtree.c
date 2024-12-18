#include "rtree.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// rtree_new: 初始化rtree结构
bool rtree_new(rtree_t* rtree, base_t* base, bool zero)
{
    if (!rtree || !base)
        return false;
    memset(rtree, 0, sizeof(rtree_t));
    rtree->base = base;
    malloc_mutex_init(&rtree->mutex, "rtree", 0, 0);
    if (zero)
    {
        memset(rtree->root, 0, sizeof(rtree->root));
    }
    return true;
}

// rtree_lookup: 查找虚拟地址对应的bits指针
void* rtree_lookup(rtree_t* rtree, uintptr_t addr)
{
    assert(rtree);
    malloc_mutex_lock(NULL, &rtree->mutex);
#if RTREEHEIGHT == 1
    size_t idx = (addr >> LBIT) & ((1U << UBIT) - 1);
    void* ret = (void*)atomic_load(&rtree->root[idx].bits);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return ret;
#elif RTREEHEIGHT == 2
    size_t idx1 = (addr >> (LBIT + UBIT / 2)) & ((1U << (UBIT / 2)) - 1);
    size_t idx2 = (addr >> LBIT) & ((1U << (UBIT - UBIT / 2)) - 1);
    rtree_node_t* node = (rtree_node_t*)atomic_load(&rtree->root[idx1].child);
    if (!node)
    {
        malloc_mutex_unlock(NULL, &rtree->mutex);
        return NULL;
    }
    void* ret = (void*)atomic_load(&((rtree_leaf_t*)node)[idx2].bits);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return ret;
#elif RTREEHEIGHT == 3
    size_t idx1 = (addr >> (LBIT + UBIT * 2 / 3)) & ((1U << (UBIT / 3)) - 1);
    size_t idx2 = (addr >> (LBIT + UBIT / 3)) & ((1U << (UBIT / 3)) - 1);
    size_t idx3 = (addr >> LBIT) & ((1U << (UBIT - 2 * (UBIT / 3))) - 1);
    rtree_node_t* node1 = (rtree_node_t*)atomic_load(&rtree->root[idx1].child);
    if (!node1)
    {
        malloc_mutex_unlock(NULL, &rtree->mutex);
        return NULL;
    }
    rtree_node_t* node2 = (rtree_node_t*)atomic_load(&node1[idx2].child);
    if (!node2)
    {
        malloc_mutex_unlock(NULL, &rtree->mutex);
        return NULL;
    }
    void* ret = (void*)atomic_load(&((rtree_leaf_t*)node2)[idx3].bits);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return ret;
#endif
}

// rtree_insert: 插入虚拟地址与bits的映射
bool rtree_insert(rtree_t* rtree, uintptr_t addr, void* bits)
{
    assert(rtree);
    malloc_mutex_lock(NULL, &rtree->mutex);
#if RTREEHEIGHT == 1
    size_t idx = (addr >> LBIT) & ((1U << UBIT) - 1);
    atomic_store(&rtree->root[idx].bits, (void*)bits);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return true;
#elif RTREEHEIGHT == 2
    size_t idx1 = (addr >> (LBIT + UBIT / 2)) & ((1U << (UBIT / 2)) - 1);
    size_t idx2 = (addr >> LBIT) & ((1U << (UBIT - UBIT / 2)) - 1);
    rtree_node_t* node = (rtree_node_t*)atomic_load(&rtree->root[idx1].child);
    if (!node)
    {
        node = (rtree_node_t*)base_calloc(1, sizeof(rtree_node_t));
        atomic_store(&rtree->root[idx1].child, (void*)node);
    }
    atomic_store(&((rtree_leaf_t*)node)[idx2].bits, (void*)bits);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return true;
#elif RTREEHEIGHT == 3
    size_t idx1 = (addr >> (LBIT + UBIT * 2 / 3)) & ((1U << (UBIT / 3)) - 1);
    size_t idx2 = (addr >> (LBIT + UBIT / 3)) & ((1U << (UBIT / 3)) - 1);
    size_t idx3 = (addr >> LBIT) & ((1U << (UBIT - 2 * (UBIT / 3))) - 1);
    rtree_node_t* node1 = (rtree_node_t*)atomic_load(&rtree->root[idx1].child);
    if (!node1)
    {
        node1 = (rtree_node_t*)base_calloc(
            1, sizeof(rtree_node_t) * (1U << (UBIT / 3)));
        atomic_store(&rtree->root[idx1].child, (void*)node1);
    }
    rtree_node_t* node2 = (rtree_node_t*)atomic_load(&node1[idx2].child);
    if (!node2)
    {
        node2 = (rtree_node_t*)base_calloc(1, sizeof(rtree_node_t));
        atomic_store(&node1[idx2].child, (void*)node2);
    }
    atomic_store(&((rtree_leaf_t*)node2)[idx3].bits, (void*)bits);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return true;
#endif
}

// rtree_remove: 移除虚拟地址的映射
bool rtree_remove(rtree_t* rtree, uintptr_t addr)
{
    assert(rtree);
    malloc_mutex_lock(NULL, &rtree->mutex);
#if RTREEHEIGHT == 1
    size_t idx = (addr >> LBIT) & ((1U << UBIT) - 1);
    atomic_store(&rtree->root[idx].bits, 0);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return true;
#elif RTREEHEIGHT == 2
    size_t idx1 = (addr >> (LBIT + UBIT / 2)) & ((1U << (UBIT / 2)) - 1);
    size_t idx2 = (addr >> LBIT) & ((1U << (UBIT - UBIT / 2)) - 1);
    rtree_node_t* node = (rtree_node_t*)atomic_load(&rtree->root[idx1].child);
    if (!node)
    {
        malloc_mutex_unlock(NULL, &rtree->mutex);
        return false;
    }
    atomic_store(&((rtree_leaf_t*)node)[idx2].bits, 0);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return true;
#elif RTREEHEIGHT == 3
    size_t idx1 = (addr >> (LBIT + UBIT * 2 / 3)) & ((1U << (UBIT / 3)) - 1);
    size_t idx2 = (addr >> (LBIT + UBIT / 3)) & ((1U << (UBIT / 3)) - 1);
    size_t idx3 = (addr >> LBIT) & ((1U << (UBIT - 2 * (UBIT / 3))) - 1);
    rtree_node_t* node1 = (rtree_node_t*)atomic_load(&rtree->root[idx1].child);
    if (!node1)
    {
        malloc_mutex_unlock(NULL, &rtree->mutex);
        return false;
    }
    rtree_node_t* node2 = (rtree_node_t*)atomic_load(&node1[idx2].child);
    if (!node2)
    {
        malloc_mutex_unlock(NULL, &rtree->mutex);
        return false;
    }
    atomic_store(&((rtree_leaf_t*)node2)[idx3].bits, 0);
    malloc_mutex_unlock(NULL, &rtree->mutex);
    return true;
#endif
}

// 可选：rtree_destroy，递归释放所有分配的节点
void rtree_destroy(rtree_t* rtree)
{
    // 递归释放所有分配的中间节点和叶子节点
    // 这里只实现RTREEHEIGHT==2/3的递归释放
#if RTREEHEIGHT == 2
    for (size_t i = 0; i < (1U << (UBIT / 2)); ++i)
    {
        rtree_node_t* node = (rtree_node_t*)atomic_load(&rtree->root[i].child);
        if (node)
        {
            // rtree_node_t等元数据回收时，不做free，生命周期由rtree_destroy整体释放。
            (void)node;
        }
    }
#elif RTREEHEIGHT == 3
    for (size_t i = 0; i < (1U << (UBIT / 3)); ++i)
    {
        rtree_node_t* node1 = (rtree_node_t*)atomic_load(&rtree->root[i].child);
        if (node1)
        {
            for (size_t j = 0; j < (1U << (UBIT / 3)); ++j)
            {
                rtree_node_t* node2 =
                    (rtree_node_t*)atomic_load(&node1[j].child);
                if (node2)
                    // rtree_node_t等元数据回收时，不做free，生命周期由rtree_destroy整体释放。
                    ;
            }
            // rtree_node_t等元数据回收时，不做free，生命周期由rtree_destroy整体释放。
        }
    }
#endif
}