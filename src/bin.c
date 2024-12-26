/*
 * bin.c - 小块分配（bin/slab）管理实现
 *
 * 主要职责：
 *   - 管理小块内存分配，采用bin+slab分层结构
 *   - 每个bin按自身大小类切分region，支持多slab扩展
 *   - 维护扁平位图高效管理region分配状态
 *   - 支持多线程并发，bin级别加锁
 *   - 提供分配/回收、slab迁移/合并/拆分、统计导出等接口
 *
 * 关键数据结构：
 *   - bin_t: 管理一组slab，支持unfull/full/slabcur等状态
 *   - slab_t: 代表一块物理内存，包含region位图、辅助元数据
 *   - region_aux_meta_t: region分配/回收元信息，便于统计与调试
 *
 * 典型调用链：
 *   arena_malloc_small -> bin_malloc_region -> slab/bitmap
 *   arena_dalloc_small -> bin_dalloc_region
 */
#include "bin.h"
#include "arena.h"
#include "arena_struct.h"
#include "base.h"
#include "edata.h"
#include "ql.h"
#include <pthread.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LMAGIC_PAD             0xAA
#define LMAGIC_FREEFILL        0xDD
#define SLAB_MIGRATE_THRESHOLD 0.2 // slab 利用率低于 20% 触发迁移

// slab按2的幂对齐分配，便于region指针快速定位所属slab
#define SLAB_ALIGN 32768

// ---- 扁平位图辅助（每个bit对应一个region） ----
static inline bool region_used(slab_t* slab, size_t idx)
{
    return (slab->used_map[idx / 64] >> (idx % 64)) & 1UL;
}

static inline void region_set_used(slab_t* slab, size_t idx)
{
    slab->used_map[idx / 64] |= (1UL << (idx % 64));
}

static inline void region_set_free(slab_t* slab, size_t idx)
{
    slab->used_map[idx / 64] &= ~(1UL << (idx % 64));
}

static inline size_t region_find_free(slab_t* slab, size_t nregions)
{
    for (size_t i = 0; i < nregions; ++i)
    {
        if (!region_used(slab, i))
            return i;
    }
    return nregions; // 无空闲region
}

/**
 * @brief slab/extent释放时合并到extent_free_list
 */
static void arena_free_extent(arena_t* arena, void* addr, size_t size)
{
    arena_insert_extent_merge(arena, addr, size);
}

/**
 * @brief region指针快速定位所属slab（slab按SLAB_ALIGN对齐分配）
 */
static inline slab_t* region_to_slab(void* region_ptr)
{
    uintptr_t addr = (uintptr_t)region_ptr;
    return (slab_t*)(addr & ~(uintptr_t)(SLAB_ALIGN - 1));
}

/**
 * @brief slab初始化：按bin大小类切分region
 * @param bin 目标bin
 * @return 新分配的slab指针
 */
static slab_t* slab_new_arena(bin_t* bin)
{
    _Static_assert(sizeof(slab_t) <= SLAB_ALIGN,
                   "slab_t 大小不能超过 SLAB_ALIGN");
    size_t nregions = bin->nregions;
    _Static_assert(SLAB_MAX_REGIONS >= SLAB_DATA_SIZE / 8,
                   "SLAB_MAX_REGIONS 不足以覆盖最小region");
    void* mem = base_alloc(bin->arena->base, sizeof(slab_t), SLAB_ALIGN);
    slab_t* slab = (slab_t*)mem;
    memset(slab, 0, sizeof(slab_t));
    slab->nused = 0;
    slab->edata.nregions = nregions;
    ql_elm_new(slab, link);
    // region辅助元数据数组
    slab->edata.region_aux_arr = (region_aux_meta_t*)base_calloc(
        nregions, sizeof(region_aux_meta_t));
    return slab;
}

/**
 * @brief 初始化bin（分配slabcur，初始化slab链表）
 * @param bin 目标bin
 * @param reg_size 该bin的region大小（size class）
 */
void bin_init(bin_t* bin, size_t reg_size)
{
    bin->reg_size = reg_size;
    bin->nregions = SLAB_DATA_SIZE / reg_size;
    bin->slabcur = (edata_t*)slab_new_arena(bin);
    bin->unfull_slabs = NULL;
    ql_new(&bin->full_slabs);
}

/**
 * @brief 判断slab是否在full_slabs链表中
 */
static bool bin_full_contains(bin_t* bin, slab_t* target)
{
    slab_t* s = (slab_t*)bin->full_slabs.qlh_first;
    while (s)
    {
        if (s == target)
            return true;
        s = s->link.qre_next;
    }
    return false;
}

/**
 * @brief 从unfull单链表摘除slab（如存在）
 */
static void bin_unfull_remove(bin_t* bin, slab_t* target)
{
    slab_t** pp = (slab_t**)&bin->unfull_slabs;
    slab_t* s = (slab_t*)bin->unfull_slabs;
    while (s)
    {
        if (s == target)
        {
            *pp = s->link.qre_next;
            s->link.qre_next = NULL;
            return;
        }
        pp = (slab_t**)&s->link.qre_next;
        s = s->link.qre_next;
    }
}

/**
 * @brief 辅助：切换slabcur到unfull slab或新建slab
 */
static slab_t* bin_pick_slab(bin_t* bin)
{
    // 先尝试unfull slabs
    if (bin->unfull_slabs)
    {
        slab_t* slab = (slab_t*)bin->unfull_slabs;
        bin->slabcur = (edata_t*)slab;
        bin->unfull_slabs = slab->link.qre_next;
        slab->link.qre_next = NULL;
        return slab;
    }
    // 没有可用slab，新建
    slab_t* slab = slab_new_arena(bin);
    bin->slabcur = (edata_t*)slab;
    return slab;
}

/**
 * @brief 从bin分配一个region
 * @param bin 目标bin
 * @return 分配到的region指针
 */
void* bin_malloc_region(bin_t* bin)
{
    slab_t* slab = (slab_t*)bin->slabcur;
    if (!slab)
        slab = bin_pick_slab(bin);
    size_t idx = region_find_free(slab, bin->nregions);
    if (idx >= bin->nregions)
    {
        // 当前slab已满，挂到full_slabs，切换新slab重试一次
        if (!bin_full_contains(bin, slab))
            ql_tail_insert(&bin->full_slabs, slab, link);
        slab = bin_pick_slab(bin);
        idx = region_find_free(slab, bin->nregions);
        if (idx >= bin->nregions)
            return NULL;
    }
    region_set_used(slab, idx);
    slab->nused++;
    if (slab->nused == bin->nregions && !bin_full_contains(bin, slab))
    {
        // slabcur变满，登记到full_slabs
        ql_tail_insert(&bin->full_slabs, slab, link);
    }
    region_aux_meta_t* meta = &slab->edata.region_aux_arr[idx];
    meta->type = 1; // bin分配
    meta->alloc_tid = (uint32_t)pthread_self();
    meta->alloc_time = (uint32_t)time(NULL);
    meta->alloc_count++;
    void* region = slab->data + idx * bin->reg_size;
    memset(region, LMAGIC_PAD, bin->reg_size);
    return region;
}

/**
 * @brief 回收一个region到bin
 * @param bin 目标bin
 * @param ptr region指针
 */
void bin_dalloc_region(bin_t* bin, void* ptr)
{
    slab_t* slab = region_to_slab(ptr);
    size_t offset = (char*)ptr - slab->data;
    size_t idx = offset / bin->reg_size;
    // 安全校验：region 必须处于已分配状态
    if (idx >= bin->nregions || !region_used(slab, idx))
    {
        fprintf(stderr,
                "[lmalloc] double free or region corruption detected!\n");
        abort();
    }
    region_set_free(slab, idx);
    region_aux_meta_t* meta = &slab->edata.region_aux_arr[idx];
    meta->type = 0;
    meta->free_tid = (uint32_t)pthread_self();
    meta->free_time = (uint32_t)time(NULL);
    meta->free_count++;
    slab->nused--;
    if (slab->nused == 0)
    {
        // 从full/unfull链表摘除后回收整块slab
        if (bin_full_contains(bin, slab))
            ql_remove(&bin->full_slabs, slab, link);
        bin_unfull_remove(bin, slab);
        arena_free_extent(bin->arena, slab, sizeof(slab_t));
        if (bin->slabcur == (edata_t*)slab)
            bin->slabcur = NULL;
    }
    else if (slab->nused == bin->nregions - 1)
    {
        if (slab == (slab_t*)bin->slabcur)
        {
            // 仍是当前slab，无需调整
        }
        else
        {
            // 从full变unfull：移出full_slabs，挂到unfull链表
            if (bin_full_contains(bin, slab))
                ql_remove(&bin->full_slabs, slab, link);
            bin_unfull_remove(bin, slab);
            slab->link.qre_next = (slab_t*)bin->unfull_slabs;
            bin->unfull_slabs = slab;
        }
    }
    memset(ptr, LMAGIC_FREEFILL, bin->reg_size);
}

/**
 * @brief slab region批量迁移到新slab，释放原slab（加锁，多线程安全）
 */
static void bin_migrate_slab(bin_t* bin, slab_t* old_slab)
{
    malloc_mutex_lock(NULL, &bin->mutex);
    // 新slab
    slab_t* new_slab = slab_new_arena(bin);
    size_t nregions = bin->nregions;
    for (size_t i = 0; i < nregions; ++i)
    {
        region_aux_meta_t* meta = &old_slab->edata.region_aux_arr[i];
        if (meta->type == 1)
        { // 只迁移bin分配的region
            void* region = old_slab->data + i * bin->reg_size;
            // 在新slab分配region
            for (size_t j = 0; j < nregions; ++j)
            {
                region_aux_meta_t* nmeta = &new_slab->edata.region_aux_arr[j];
                if (nmeta->type == 0)
                { // 在新slab找到未分配的region
                    memcpy(new_slab->data + j * bin->reg_size,
                           region,
                           bin->reg_size);
                    nmeta->type = 1; // 标记为bin分配
                    nmeta->alloc_tid = meta->alloc_tid;
                    nmeta->alloc_time = meta->alloc_time;
                    region_set_used(new_slab, j);
                    new_slab->nused++;
                    break;
                }
            }
            // 回收原region
            meta->type = 0;
            meta->alloc_tid = 0;
            meta->alloc_time = 0;
        }
    }
    // 统计
    bin->migrate_count++;
    old_slab->migrate_count++;
    new_slab->migrate_count++;
    // 释放原slab
    arena_free_extent(bin->arena, old_slab, sizeof(slab_t));
    // 将新slab挂到bin->unfull_slabs
    new_slab->link.qre_next = (slab_t*)bin->unfull_slabs;
    bin->unfull_slabs = new_slab;
    malloc_mutex_unlock(NULL, &bin->mutex);
}

/**
 * @brief bin compaction（合并碎片slab，多线程安全）
 */
void bin_compact(bin_t* bin)
{
    malloc_mutex_lock(NULL, &bin->mutex);
    // 简化：将所有unfull slab的region迁移到新slab，释放原slab
    slab_t* slab = (slab_t*)bin->unfull_slabs;
    while (slab)
    {
        slab_t* next = slab->link.qre_next;
        if (slab->nused > 0 && slab->nused < bin->nregions)
        {
            bin_migrate_slab(bin, slab);
            bin->compact_count++;
            slab->compact_count++;
        }
        slab = next;
    }
    malloc_mutex_unlock(NULL, &bin->mutex);
}

/**
 * @brief bin split（拆分大slab为小slab，多线程安全）
 */
void bin_split(bin_t* bin, slab_t* big_slab)
{
    malloc_mutex_lock(NULL, &bin->mutex);
    // 简化：将big_slab拆分为多个小slab
    size_t n = big_slab->nused;
    for (size_t i = 0; i < n; ++i)
    {
        slab_t* new_slab = slab_new_arena(bin);
        // 迁移一个region
        for (size_t j = 0; j < bin->nregions; ++j)
        {
            region_aux_meta_t* meta = &big_slab->edata.region_aux_arr[j];
            if (meta->type == 1)
            {
                memcpy(new_slab->data,
                       big_slab->data + j * bin->reg_size,
                       bin->reg_size);
                region_aux_meta_t* nmeta = &new_slab->edata.region_aux_arr[0];
                nmeta->type = 1;
                nmeta->alloc_tid = meta->alloc_tid;
                nmeta->alloc_time = meta->alloc_time;
                region_set_used(new_slab, 0);
                new_slab->nused++;
                meta->type = 0;
                break;
            }
        }
        new_slab->split_count++;
        bin->split_count++;
        // 挂到unfull_slabs
        new_slab->link.qre_next = (slab_t*)bin->unfull_slabs;
        bin->unfull_slabs = new_slab;
    }
    arena_free_extent(bin->arena, big_slab, sizeof(slab_t));
    malloc_mutex_unlock(NULL, &bin->mutex);
}

/**
 * @brief 导出bin/slab迁移/合并/拆分统计为CSV
 * @param filename 输出文件名
 */
void bin_slab_migrate_stats_export_csv(const char* filename)
{
    FILE* f = fopen(filename, "w");
    if (!f)
        return;
    fprintf(f, "arena,bin,slab,migrate_count,compact_count,split_count\n");
    for (unsigned a = 0; a < n_arenas; ++a)
    {
        arena_t* arena = arenas[a];
        for (unsigned b = 0; b < arena->n_bins; ++b)
        {
            bin_t* bin = &arena->bins[b];
            slab_t* slab = (slab_t*)bin->slabcur;
            if (slab)
                fprintf(f,
                        "%u,%u,%p,%zu,%zu,%zu\n",
                        a,
                        b,
                        slab,
                        slab->migrate_count,
                        slab->compact_count,
                        slab->split_count);
            slab = (slab_t*)bin->unfull_slabs;
            while (slab)
            {
                fprintf(f,
                        "%u,%u,%p,%zu,%zu,%zu\n",
                        a,
                        b,
                        slab,
                        slab->migrate_count,
                        slab->compact_count,
                        slab->split_count);
                slab = slab->link.qre_next;
            }
            slab = (slab_t*)bin->full_slabs.qlh_first;
            while (slab)
            {
                fprintf(f,
                        "%u,%u,%p,%zu,%zu,%zu\n",
                        a,
                        b,
                        slab,
                        slab->migrate_count,
                        slab->compact_count,
                        slab->split_count);
                slab = slab->link.qre_next;
            }
        }
    }
    fclose(f);
}

/**
 * @brief 获取bin统计快照（遍历slab实时计算）
 * @param bin 目标bin
 * @param out 输出统计结构体
 */
void bin_stats_get(bin_t* bin, bin_stats_t* out)
{
    memset(out, 0, sizeof(*out));
    if (!bin)
        return;
    out->alloc_count = bin->alloc_count;
    out->free_count = bin->free_count;
    out->current_bytes = bin->current_bytes;
    out->peak_bytes = bin->peak_bytes;
    out->migrate_count = bin->migrate_count;
    out->compact_count = bin->compact_count;
    out->split_count = bin->split_count;
    // 统计slab数量与region使用情况
    slab_t* slabs[3] = {(slab_t*)bin->slabcur,
                        (slab_t*)bin->unfull_slabs,
                        (slab_t*)bin->full_slabs.qlh_first};
    for (int s = 0; s < 3; ++s)
    {
        slab_t* slab = slabs[s];
        while (slab)
        {
            out->slab_count++;
            out->nused += slab->nused;
            out->nregions += slab->edata.nregions;
            slab = slab->link.qre_next;
        }
    }
    out->slab_bytes = out->slab_count * sizeof(slab_t);
}

/**
 * @brief 打印bin统计信息
 * @param bin 目标bin
 */
void bin_stats_print(bin_t* bin)
{
    bin_stats_t s;
    bin_stats_get(bin, &s);
    printf("[bin %p] alloc=%zu free=%zu cur=%zu peak=%zu slabs=%zu "
           "used=%zu/%zu (%.1f%%) migrate=%zu compact=%zu split=%zu\n",
           (void*)bin,
           s.alloc_count,
           s.free_count,
           s.current_bytes,
           s.peak_bytes,
           s.slab_count,
           s.nused,
           s.nregions,
           s.nregions ? (double)s.nused * 100.0 / s.nregions : 0.0,
           s.migrate_count,
           s.compact_count,
           s.split_count);
}
