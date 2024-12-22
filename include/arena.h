#pragma once

#include "arena_struct.h"

typedef struct tsd_s tsd_t;
typedef unsigned szind_t;

// NUMA统计结构体：记录每个NUMA节点的分配/回收/大页等统计信息
struct numa_stats_s {
    size_t alloc_count[64]; // 分配次数
    size_t alloc_bytes[64]; // 分配字节数
    size_t free_count[64];  // 回收次数
    size_t free_bytes[64];  // 回收字节数
    size_t hugepage_count[64]; // 大页分配次数
    size_t hugepage_bytes[64]; // 大页分配字节数
};
extern struct numa_stats_s g_numa_stats;

// 初始化全局 arena 列表（多线程支持）
void arena_boot(void);

// 选择当前线程使用的 arena（可支持多 arena 策略）
arena_t* arena_choose(tsd_t* tsd);

// 初始化单个 arena
arena_t* arena_new(unsigned arena_id);

// 小块分配（走 bin/slab 路径）
void* arena_malloc_small(arena_t* arena, szind_t ind /* size class index */);

// 小块回收
void arena_dalloc_small(arena_t* arena, void* ptr, szind_t ind);

// 大块分配（走 extent 红黑树/链表路径）
void* arena_malloc_large(arena_t* arena, size_t size);
// 大块回收
void arena_dalloc_large(arena_t* arena, void* ptr, size_t size);

// NUMA感知分配（简化接口）
void* arena_alloc_extent_numa(arena_t* arena, size_t size);
// NUMA感知分配（完整接口，支持node/大页）
void* arena_alloc_extent_numa_ex(arena_t* arena,
                                 size_t size,
                                 int node,
                                 int use_hugepage);
// NUMA统计打印
void numa_stats_print(void);

// 空闲extent合并（bin模块复用）
void arena_insert_extent_merge(arena_t* arena, void* addr, size_t size);

// ================== 统计、调试接口 ==================
// arena 统计信息快照
typedef struct arena_stats_s
{
    size_t alloc_count;    // 分配次数
    size_t free_count;     // 回收次数
    size_t current_bytes;  // 当前分配字节数
    size_t peak_bytes;     // 峰值分配字节数
    size_t slab_count;     // 当前活跃slab数量
    size_t extent_count;   // 当前空闲extent数量
    size_t slab_bytes;     // slab总字节数
    size_t extent_bytes;   // 空闲extent总字节数
    size_t decay_count;    // decay回收次数
    size_t migrate_count;  // slab迁移次数
    size_t compact_count;  // slab合并次数
    size_t split_count;    // slab拆分次数
    size_t n_threads;      // 绑定线程数
} arena_stats_t;

// 获取arena统计快照（遍历bin/slab实时计算）
void arena_stats_get(arena_t* arena, arena_stats_t* out);
// 打印单个arena统计
void arena_stats_print(arena_t* arena);
// 打印所有arena统计
void arena_stats_print_all(void);