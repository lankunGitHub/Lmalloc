#pragma once

#include <pthread.h>
#include <stddef.h>
#include <time.h>

#include "atomic.h"

typedef struct tsd_s tsd_t;

typedef struct pac_s pac_t;
typedef struct pac_block_meta_s pac_block_meta_t;

struct pac_block_meta_s
{
    void* addr;                    // 分配地址
    size_t size;                   // 分配大小
    time_t alloc_time;             // 分配时间
    time_t free_time;              // 回收时间
    pthread_t tid;                 // 分配线程
    int numa_node;                 // NUMA节点
    int is_hugepage;               // 是否大页
    struct pac_block_meta_s* next; // 元数据链表
};

// pac 统计信息（原子计数，多线程安全）
struct pac_stats_s {
    ATOMIC_VAR(size_t) alloc_count;
    ATOMIC_VAR(size_t) free_count;
    ATOMIC_VAR(size_t) current_bytes;
    ATOMIC_VAR(size_t) peak_bytes;
    ATOMIC_VAR(size_t) hugepage_count;
    ATOMIC_VAR(size_t) hugepage_bytes;
};

// jemalloc 仿制版 pac 结构体
// 管理大块（大于某阈值）直接从操作系统分配/回收
struct pac_s {
    // extent tree、free list 等元数据
    pac_block_meta_t* meta_head; // 分配元数据链表头
    pthread_mutex_t meta_mutex;  // 保护元数据链表
    // 统计信息
    struct pac_stats_s stats;
};

extern struct pac_stats_s g_pac_stats;

// 大块分配接口
void* pac_alloc(tsd_t* tsd, size_t size, size_t alignment);
// 大块回收接口
void pac_dalloc(tsd_t* tsd, void* ptr, size_t size);
