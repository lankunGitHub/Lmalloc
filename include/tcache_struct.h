#pragma once

#include "cache_bin_struct.h"
#include "sc.h"
#include <stdatomic.h>

/*tcache 允许缓存的最大的内存，超过该大小使用arena*/
#define TCACHE_LG2_MAX_LIMIT 23
#define TCAHEE_MAX_LIMIT     ((size_t)1 << TCACHE_LG2_MAX_LIMIT)

#define TCACHE_NBINS_MAX                                                       \
    (SC_NBINS + SC_NGROUP * (TCACHE_LG2_MAX_LIMIT - SC_LG2_LARGE_MINCLASS) + 1)

typedef struct tcache_s tcache_t;
// jemalloc 仿制版 tcache 结构体
// 每个线程维护一个 tcache，管理各大小类的 cache_bin
struct tcache_s {
    cache_bin_t bins[TCACHE_NBINS_MAX]; // 按 bin 管理 cache，提升小对象分配性能
    atomic_int lock; // lock-free 支持（0=unlocked, 1=locked）
    // 统计信息
    size_t alloc_count;      // tcache分配次数
    size_t free_count;       // tcache回收次数
    size_t flush_count;      // tcache flush次数
    size_t gc_count;         // LRU GC次数
    size_t handoff_count;    // 跨线程handoff次数
    // 配置参数
    int max_capacity;        // 全局最大容量
    int min_capacity;        // 全局最小容量
    int adapt_interval;      // 自适应调整周期
    int gc_interval;         // LRU GC周期
    // 可扩展：NUMA/线程ID/调试信息等
} __attribute__((aligned(64)));