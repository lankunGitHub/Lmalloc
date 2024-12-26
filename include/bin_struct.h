#pragma once

#include "edata.h"
#include "mutex.h"
#include "sz.h"

#define SLAB_DATA_SIZE   16384 // slab数据区大小，按bin大小类切分region
#define SLAB_MAX_REGIONS 2048  // used_map可管理的最大region数
#define SLAB_NREGION     SLAB_MAX_REGIONS // 兼容旧引用

typedef struct slab_s slab_t;
struct slab_s {
    edata_t edata;
    alignas(64) unsigned long used_map[SLAB_MAX_REGIONS / 64]; // region分配位图
    alignas(64) char data[SLAB_DATA_SIZE]; // 实际内存
    size_t nused;                                           // 已分配 region 数
    ql_elm(struct slab_s) link;                             // 链表节点
    // 迁移/合并/拆分统计
    size_t migrate_count;
    size_t compact_count;
    size_t split_count;
};

/*bin根据sc分为不同的类型，每种bin管理对应大小的edata*/
typedef struct bin_s bin_t;
// jemalloc 仿制版 bin 结构体
// 每个 bin 管理一个大小类的 slab 列表，负责小块分配
struct bin_s {
    malloc_mutex_t mutex;      // 保护 bin 内部结构
    struct arena_s* arena;     // 所属 arena 指针
    edata_t* slabcur;          // 当前正在使用的 slab，位于 unfull_slabs 中
    struct slab_s* unfull_slabs; // 未满、可继续分配的 slab 链表
    ql_head(slab_t) full_slabs;// 已满、不可继续分配的 slab 链表头
    size_t reg_size;           // 该bin的region大小（size class）
    size_t nregions;           // 每个slab的region数量
    size_t alloc_count;
    size_t free_count;
    size_t current_bytes;
    size_t peak_bytes;
    // 迁移/合并/拆分统计
    size_t migrate_count;
    size_t compact_count;
    size_t split_count;
} __attribute__((aligned(64)));
