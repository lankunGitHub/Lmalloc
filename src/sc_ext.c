#include "sc_ext.h"
#include "../include/sc.h"
#include "../include/ts.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sc_ext_data_t g_sc_ext_data;
sc_ext_policy_t* g_sc_ext_policy = NULL;

// ---- 静态策略 ----
static void sc_ext_static_policy_init(sc_ext_data_t* data,
                                      const sc_data_t* base)
{
    // 直接从sc唯一静态分布拷贝
    data->used = base->nsizes; // 假设 base->nsizes 为有效数量
    for (size_t i = 0; i < data->used; ++i)
    {
        data->entries[i].size =
            (ZU(1) << base->entries[i].lg2_base) +
            (ZU(base->entries[i].ndelta) << base->entries[i].lg2_delta);
        data->entries[i].align = 1UL << base->entries[i].lg2_base;
        data->entries[i].slab_regions =
            base->entries[i].slab ? base->entries[i].pgs : 0;
        data->entries[i].alloc_count = 0;
        data->entries[i].free_count = 0;
        data->entries[i].current_bytes = 0;
        data->entries[i].peak_bytes = 0;
        data->entries[i].is_hot = false;
        data->entries[i].is_dynamic = false;
    }
}

static size_t sc_ext_static_policy_find(const sc_ext_data_t* data, size_t size)
{
    // 有序数组二分查找
    size_t l = 0, r = data->used;
    while (l < r)
    {
        size_t m = (l + r) / 2;
        if (data->entries[m].size < size)
            l = m + 1;
        else
            r = m;
    }
    if (l < data->used)
        return l;
    return SIZE_MAX; // 未找到
}

static void sc_ext_static_policy_adapt(sc_ext_data_t* data)
{
    // 静态策略不自适应
    (void)data;
}
sc_ext_policy_t sc_ext_static_policy = {.name = "static",
                                        .init = sc_ext_static_policy_init,
                                        .find = sc_ext_static_policy_find,
                                        .adapt = sc_ext_static_policy_adapt};

// ---- 动态策略 ----
static void sc_ext_dynamic_policy_init(sc_ext_data_t* data,
                                       const sc_data_t* base)
{
    sc_ext_static_policy_init(data, base);
}

static size_t sc_ext_dynamic_policy_find(const sc_ext_data_t* data, size_t size)
{
    return sc_ext_static_policy_find(data, size);
}

static void sc_ext_dynamic_policy_adapt(sc_ext_data_t* data)
{
    // 动态策略：根据统计自动插入热点size class
    const size_t HOT_THRESHOLD = 10000;
    for (size_t i = 0; i + 1 < data->used; ++i)
    {
        sc_ext_t* e = &data->entries[i];
        if (e->alloc_count > HOT_THRESHOLD && !e->is_dynamic)
        {
            // 在该区间插入更细粒度size class
            if (data->used + 1 < SC_EXT_DATA_MAX)
            {
                size_t new_size = (e->size + data->entries[i + 1].size) / 2;
                memmove(&data->entries[i + 2],
                        &data->entries[i + 1],
                        (data->used - i - 1) * sizeof(sc_ext_t));
                data->entries[i + 1].size = new_size;
                data->entries[i + 1].align = e->align;
                data->entries[i + 1].slab_regions = 4096 / new_size;
                data->entries[i + 1].is_hot = false;
                data->entries[i + 1].is_dynamic = true;
                data->entries[i + 1].alloc_count = 0;
                data->entries[i + 1].free_count = 0;
                data->entries[i + 1].current_bytes = 0;
                data->entries[i + 1].peak_bytes = 0;
                data->used++;
                e->alloc_count = 0;
            }
        }
    }
}
sc_ext_policy_t sc_ext_dynamic_policy = {.name = "dynamic",
                                         .init = sc_ext_dynamic_policy_init,
                                         .find = sc_ext_dynamic_policy_find,
                                         .adapt = sc_ext_dynamic_policy_adapt};

// ---- 自适应策略 ----
static void sc_ext_adaptive_policy_init(sc_ext_data_t* data,
                                        const sc_data_t* base)
{
    sc_ext_dynamic_policy_init(data, base);
}

static size_t sc_ext_adaptive_policy_find(const sc_ext_data_t* data,
                                          size_t size)
{
    return sc_ext_dynamic_policy_find(data, size);
}

static void sc_ext_adaptive_policy_adapt(sc_ext_data_t* data)
{
    sc_ext_dynamic_policy_adapt(data);
    const size_t COLD_THRESHOLD = 10;
    for (size_t i = 1; i + 1 < data->used; ++i)
    {
        sc_ext_t* e = &data->entries[i];
        if (e->is_dynamic && e->alloc_count < COLD_THRESHOLD &&
            e->free_count < COLD_THRESHOLD)
        {
            memmove(e, e + 1, (data->used - i - 1) * sizeof(sc_ext_t));
            data->used--;
            i--;
        }
    }
}
sc_ext_policy_t sc_ext_adaptive_policy = {.name = "adaptive",
                                          .init = sc_ext_adaptive_policy_init,
                                          .find = sc_ext_adaptive_policy_find,
                                          .adapt =
                                              sc_ext_adaptive_policy_adapt};

// ---- 全局接口实现 ----
void sc_ext_data_init(sc_ext_data_t* data,
                      const sc_data_t* base,
                      sc_ext_policy_t* policy)
{
    data->used = 0;
    if (policy && policy->init)
        policy->init(data, base);
    g_sc_ext_policy = policy;
}

size_t sc_ext_data_find(const sc_ext_data_t* data, size_t size)
{
    if (g_sc_ext_policy && g_sc_ext_policy->find)
        return g_sc_ext_policy->find(data, size);
    return 0;
}
size_t sc_ext_data_index2size(const sc_ext_data_t* data, size_t index)
{
    if (index < data->used)
        return data->entries[index].size;
    return 0;
}
size_t sc_ext_data_align(const sc_ext_data_t* data, size_t size)
{
    size_t idx = sc_ext_data_find(data, size);
    // find未命中返回SIZE_MAX，直接当数组下标会越界读
    if (idx == SIZE_MAX || idx >= data->used)
        return QUANTUM;
    return data->entries[idx].align;
}
void sc_ext_data_adapt(sc_ext_data_t* data)
{
    if (g_sc_ext_policy && g_sc_ext_policy->adapt)
        g_sc_ext_policy->adapt(data);
}