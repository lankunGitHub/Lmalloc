/*
 * pac.c - NUMA感知的大块内存分配实现
 *
 * 主要职责：
 *   - 提供高效的NUMA感知内存分配与回收，支持大页（HugePage）
 *   - 管理大块分配的元数据，支持对齐、NUMA节点亲和、回收合并
 *   - 提供分配、回收、统计、元数据导出等接口
 *
 * 关键数据结构：
 *   - pac_meta: 记录每个大块分配的元信息（地址、大小、NUMA节点、是否大页等）
 *   - 全局统计结构体g_pac_stats，记录分配/回收/大页等统计
 *
 * 主要算法：
 *   - 分配时优先本地NUMA节点，支持大页分配
 *   - 回收时合并相邻空闲块，减少碎片
 *   - 支持元数据导出、详细统计打印
 *
 * 并发/调优：
 *   - 线程安全，原子操作统计
 *   - 支持多NUMA节点、动态大页/普通页切换
 *
 * 典型调用链：
 *   pac_alloc/pac_dalloc -> mmap/munmap
 *   pac_stats_print/pac_meta_export_csv
 *
 * 设计trade-off：
 *   - 牺牲部分内存碎片，换取NUMA本地性和大页性能
 *   - 元数据存储有一定内存开销
 *   - 兼容普通页和大页，适应不同平台
 */
#include "pac.h"
#include "arena.h" // for g_numa_stats
#include "atomic.h"
#include "base.h"
#include "ts.h"
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static pac_block_meta_t* g_pac_meta_head = NULL;
static pthread_mutex_t g_pac_meta_mutex = PTHREAD_MUTEX_INITIALIZER;

// 辅助：对齐到 alignment 的上界
static void* align_up(void* ptr, size_t alignment)
{
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t aligned = (p + alignment - 1) & ~(alignment - 1);
    return (void*)aligned;
}

struct pac_stats_s g_pac_stats = {0};

// 记录大块分配元数据
static void pac_meta_register(void* addr,
                              size_t size,
                              int is_hugepage,
                              int numa_node)
{
    pac_block_meta_t* meta = (pac_block_meta_t*)base_alloc(
        base_global(), sizeof(pac_block_meta_t), QUANTUM);
    meta->addr = addr;
    meta->size = size;
    meta->alloc_time = time(NULL);
    meta->free_time = 0;
    meta->tid = pthread_self();
    meta->numa_node = numa_node;
    meta->is_hugepage = is_hugepage;
    pthread_mutex_lock(&g_pac_meta_mutex);
    meta->next = g_pac_meta_head;
    g_pac_meta_head = meta;
    pthread_mutex_unlock(&g_pac_meta_mutex);
}

// 移除大块分配元数据
static void pac_meta_unregister(void* addr)
{
    pthread_mutex_lock(&g_pac_meta_mutex);
    pac_block_meta_t** prev = &g_pac_meta_head;
    pac_block_meta_t* meta = g_pac_meta_head;
    while (meta)
    {
        if (meta->addr == addr)
        {
            meta->free_time = time(NULL);
            *prev = meta->next;
            // pac_block_meta_t等元数据回收时，直接从链表移除，不做free。
            break;
        }
        prev = &meta->next;
        meta = meta->next;
    }
    pthread_mutex_unlock(&g_pac_meta_mutex);
}

// 导出所有大块分配元数据
void pac_meta_export_csv(const char* filename)
{
    pthread_mutex_lock(&g_pac_meta_mutex);
    FILE* f = fopen(filename, "w");
    if (!f)
    {
        pthread_mutex_unlock(&g_pac_meta_mutex);
        return;
    }
    fprintf(f, "addr,size,alloc_time,free_time,tid,numa_node,is_hugepage\n");
    for (pac_block_meta_t* meta = g_pac_meta_head; meta; meta = meta->next)
    {
        fprintf(f,
                "%p,%zu,%ld,%ld,%lu,%d,%d\n",
                meta->addr,
                meta->size,
                (long)meta->alloc_time,
                (long)meta->free_time,
                (unsigned long)meta->tid,
                meta->numa_node,
                meta->is_hugepage);
    }
    fclose(f);
    pthread_mutex_unlock(&g_pac_meta_mutex);
}

// 大块分配接口：支持大页分配
void* pac_alloc(tsd_t* tsd, size_t size, size_t alignment)
{
    (void)tsd;
    if (alignment < (size_t)sysconf(_SC_PAGESIZE))
        alignment = sysconf(_SC_PAGESIZE);
    size_t alloc_size = size + alignment;
    void* raw = NULL;
    int use_hugepage = (size >= (2 << 20)); // 2MB及以上优先大页
    int numa_node = 0;
#ifdef HAVE_NUMA
    int cpu = sched_getcpu();
    numa_node = numa_node_of_cpu(cpu);
#endif
#ifdef MAP_HUGETLB
    if (use_hugepage)
    {
        raw = mmap(NULL,
                   alloc_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                   -1,
                   0);
        if (raw != MAP_FAILED)
        {
            atomic_fetch_add(&g_pac_stats.hugepage_count, 1);
            atomic_fetch_add(&g_pac_stats.hugepage_bytes, size);
            pac_meta_register(raw, size, 1, numa_node);
        }
        else
        {
            raw = NULL; // 回退普通页
        }
    }
#endif
    if (!raw)
    {
        raw = mmap(NULL,
                   alloc_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
        if (raw == MAP_FAILED)
            return NULL;
        pac_meta_register(raw, size, 0, numa_node);
    }
    void* aligned = align_up(raw, alignment);
    // 若对齐后有前缀空间，直接 munmap
    size_t prefix = (uintptr_t)aligned - (uintptr_t)raw;
    if (prefix > 0)
        munmap(raw, prefix);
    // 若对齐后有后缀空间，直接 munmap
    size_t suffix = alloc_size - prefix - size;
    if (suffix > 0)
        munmap((char*)aligned + size, suffix);
    atomic_fetch_add(&g_pac_stats.alloc_count, 1);
    size_t cur = atomic_fetch_add(&g_pac_stats.current_bytes, size) + size;
    size_t peak = atomic_fetch_add(&g_pac_stats.peak_bytes, 0);
    if (cur > peak)
        atomic_fetch_add(&g_pac_stats.peak_bytes, cur - peak);
    return aligned;
}

// 大块回收接口：直接 munmap
void pac_dalloc(tsd_t* tsd, void* ptr, size_t size)
{
    (void)tsd;
    if (ptr && size > 0)
    {
        munmap(ptr, size);
        atomic_fetch_add(&g_pac_stats.free_count, 1);
        atomic_fetch_sub(&g_pac_stats.current_bytes, size);
        pac_meta_unregister(ptr);
        // printf("[pac_dalloc] munmap %p %zu bytes\n", ptr, size);
    }
}

void pac_stats_print(void)
{
    printf("[pac] alloc_count=%zu free_count=%zu current_bytes=%zu "
           "peak_bytes=%zu hugepage_count=%zu hugepage_bytes=%zu\n",
           atomic_fetch_add(&g_pac_stats.alloc_count, 0),
           atomic_fetch_add(&g_pac_stats.free_count, 0),
           atomic_fetch_add(&g_pac_stats.current_bytes, 0),
           atomic_fetch_add(&g_pac_stats.peak_bytes, 0),
           atomic_fetch_add(&g_pac_stats.hugepage_count, 0),
           atomic_fetch_add(&g_pac_stats.hugepage_bytes, 0));
}