#pragma once

#include "bin_struct.h"
#include "cache_bin_struct.h"

static inline void*
cache_bin_alloc_impl(cache_bin_t* bin, bool* success, bool adjust_low_water)
{

    void* ret = bin->stack[bin->head];
    uint16_t low_bits = (uint16_t)(uintptr_t)bin->head;
    void** new_head = bin->stack + bin->head + 1;

    if (likely(low_bits != bin->low_water))
    {
        bin->head = (uint16_t)(uintptr_t)new_head;
        *success = true;
        return ret;
    }
    if (!adjust_low_water)
    {
        *success = false;
        return NULL;
    }

    if (likely(low_bits != bin->empty))
    {
        bin->head = (uint16_t)(uintptr_t)new_head;
        bin->low_water = (uint16_t)(uintptr_t)new_head;
        *success = true;
        return ret;
    }
    *success = false;
    return NULL;
}

static inline void* cache_bin_alloc_easy(cache_bin_t* bin, bool* success)
{
    return cache_bin_alloc_impl(bin, success, false);
}

static inline void* cache_bin_alloc(cache_bin_t* bin, bool* success)
{
    return cache_bin_alloc_impl(bin, success, true);
}

// 初始化 bin（分配 slab 管理结构等，reg_size为该bin的size class）
void bin_init(bin_t* bin, size_t reg_size);

// 从 bin 分配一个小块（region）
void* bin_malloc_region(bin_t* bin);

// 回收一个小块（region）到 bin
void bin_dalloc_region(bin_t* bin, void* ptr);

// ================== bin 统计、调试接口 ==================
// bin 统计信息快照
typedef struct bin_stats_s
{
    size_t alloc_count;    // 分配次数
    size_t free_count;     // 回收次数
    size_t current_bytes;  // 当前分配字节数
    size_t peak_bytes;     // 峰值分配字节数
    size_t slab_count;     // 活跃slab数量
    size_t nused;          // 已使用region数量
    size_t nregions;       // region总数量
    size_t slab_bytes;     // slab总字节数
    size_t migrate_count;  // slab迁移次数
    size_t compact_count;  // slab合并次数
    size_t split_count;    // slab拆分次数
} bin_stats_t;

// 获取bin统计快照
void bin_stats_get(bin_t* bin, bin_stats_t* out);
// 打印bin统计信息
void bin_stats_print(bin_t* bin);