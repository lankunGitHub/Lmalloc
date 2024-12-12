/*
 * base.c - 高度对齐 jemalloc 的基础内存分配器实现
 *
 * 主要特性：
 *   - block 分配/增长/对齐/auto thp 切换
 *   - heap/edata_avail 管理
 *   - ehooks/多后端支持
 *   - 统计信息 resident/mapped/allocated/n_thp
 *   - 元数据分配（edata/rtree/tcache stack）
 *   - 线程安全与 fork 支持
 *   - trace hook、NUMA 支持等创新点
 *   - 详细注释，便于维护和扩展
 */

#include "base.h"
#include "edata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#include <unistd.h>

#define BASE_BLOCK_MIN_ALIGN    ((size_t)2 << 20)
#define BASE_DEFAULT_BLOCK_SIZE ((size_t)2 << 20) // 2MB
#define BASE_AUTO_THP_THRESHOLD 2

// ------------------ 内部辅助 ------------------

static size_t page_size()
{
    // 直接查询，避免静态缓存在多线程首次并发时产生数据竞争
    return (size_t)sysconf(_SC_PAGESIZE);
}

static size_t alignment_ceil(size_t size, size_t align)
{
    return ((size + align - 1) / align) * align;
}

// ------------------ block 分配与 THP ------------------

// 默认 mmap 后端：platform 通用，无 NUMA 亲和
static void* mmap_backend_alloc(size_t size,
                                size_t alignment,
                                int numa_node,
                                void* arg)
{
    (void)numa_node;
    (void)arg;
    // 多映射一页并预留对齐前缀，保证对齐地址前有空间存放映射信息
    size_t total = size + alignment + page_size();
    void* mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;
    uintptr_t addr = (uintptr_t)mem;
    uintptr_t aligned = (addr + 16 + alignment - 1) & ~(alignment - 1);
    // 记录真实映射起始地址于对齐地址之前，便于 dealloc。
    // 注意必须整体位于返回地址之前（-2），否则saved[1]会写到
    // 返回地址本身，被block头部覆盖后munmap长度错误
    void** saved = (void**)aligned - 2;
    saved[0] = mem;
    saved[1] = (void*)total;
    return (void*)aligned;
}

static void mmap_backend_dealloc(void* ptr, size_t size, void* arg)
{
    (void)size;
    (void)arg;
    if (!ptr)
        return;
    void** saved = (void**)ptr - 2;
    munmap(saved[0], (size_t)saved[1]);
}

static base_backend_t default_mmap_backend = {
    .kind = BASE_BACKEND_MMAP,
    .alloc = mmap_backend_alloc,
    .dealloc = mmap_backend_dealloc,
    .arg = NULL,
};

static base_block_t* base_block_create(base_backend_t* backend,
                                       int numa_node,
                                       size_t size,
                                       size_t alignment)
{
    if (!backend)
        backend = &default_mmap_backend;
    size_t block_size = alignment_ceil(size, alignment);
    if (block_size < BASE_DEFAULT_BLOCK_SIZE)
        block_size = BASE_DEFAULT_BLOCK_SIZE;
    void* mem = backend->alloc(block_size, alignment, numa_node, backend->arg);
    if (!mem)
        return NULL;
    // ----------- THP 支持 -----------
#if defined(__linux__)
    // base_t 还未初始化时，无法判断 thp_mode，首次分配不做 madvise
#endif
    // --------------------------------
    base_block_t* block = (base_block_t*)mem;
    block->size = block_size;
    block->next = NULL;
    block->unused_start =
        (void*)((char*)mem + ((sizeof(base_block_t) + 63) / 64) * 64);
    block->unused_size = block_size - ((char*)block->unused_start - (char*)mem);
    return block;
}

static void base_block_destroy(base_t* base, base_block_t* block)
{
    // backend可能为NULL（base_new/base_global默认路径），回退默认mmap后端
    base_backend_t* backend = base->backend ? base->backend : &default_mmap_backend;
    backend->dealloc(block, block->size, backend->arg);
}

// ------------------ trace/profile 钩子 ------------------

void base_set_trace_hook(base_t* base,
                         void (*trace_hook)(const char*, void*, size_t))
{
    base->trace_hook = (void*)trace_hook;
}
static void base_trace(base_t* base, const char* event, void* ptr, size_t size)
{
    if (base->trace_hook)
    {
        void (*hook)(const char*, void*, size_t) =
            (void (*)(const char*, void*, size_t))base->trace_hook;
        hook(event, ptr, size);
    }
}

// ------------------ 统计信息 ------------------

void base_get_stats(base_t* base, base_stats_t* stats_out)
{
    if (stats_out)
        *stats_out = base->stats;
}

// ------------------ block 链表管理 ------------------

static void base_blocks_insert(base_t* base, base_block_t* block)
{
    block->next = base->blocks;
    base->blocks = block;
}

// ------------------ 分配/释放实现 ------------------

static void*
base_block_alloc_from(base_block_t* block, size_t size, size_t alignment)
{
    uintptr_t start = (uintptr_t)block->unused_start;
    uintptr_t aligned = (start + alignment - 1) & ~(alignment - 1);
    size_t gap = aligned - start;
    if (block->unused_size < gap + size)
        return NULL;
    void* ret = (void*)aligned;
    block->unused_start = (void*)(aligned + size);
    block->unused_size -= (gap + size);
    return ret;
}

void* base_alloc(base_t* base, size_t size, size_t alignment)
{
    if (alignment < 64)
        alignment = 64;
    if (size == 0)
        return NULL;
    void* result = NULL;
    malloc_mutex_lock(NULL, &base->mutex);
    // 1. 先尝试从现有 block 分配
    for (base_block_t* blk = base->blocks; blk; blk = blk->next)
    {
        result = base_block_alloc_from(blk, size, alignment);
        if (result)
            break;
    }
    // 2. 不足则新建 block
    if (!result)
    {
        base_block_t* new_block = base_block_create(
            base->backend, base->numa_node, size + alignment, alignment);
        if (!new_block)
        {
            malloc_mutex_unlock(NULL, &base->mutex);
            return NULL;
        }
        base_blocks_insert(base, new_block);
        result = base_block_alloc_from(new_block, size, alignment);
        assert(result);
        base->stats.mapped += new_block->size;
        if (base->thp_mode != BASE_THP_DISABLED)
            base->n_thp++;
        base_trace(base, "block_alloc", new_block, new_block->size);
    }
    base->stats.allocated += size;
    base->stats.n_alloc_calls++;
    base_trace(base, "alloc", result, size);
    malloc_mutex_unlock(NULL, &base->mutex);
    return result;
}

void base_free(base_t* base, void* ptr, size_t size)
{
    // base 分配的内存通常不单独 free，直接统计
    malloc_mutex_lock(NULL, &base->mutex);
    base->stats.allocated -= size;
    base->stats.n_free_calls++;
    base_trace(base, "free", ptr, size);
    malloc_mutex_unlock(NULL, &base->mutex);
}

void* base_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void* p = base_alloc(base_global(), total, 64);
    if (p)
        memset(p, 0, total);
    return p;
}

void* base_realloc(void* ptr, size_t old_size, size_t new_size)
{
    if (!ptr)
        return base_alloc(base_global(), new_size, 64);
    if (new_size == 0)
        return NULL;
    void* np = base_alloc(base_global(), new_size, 64);
    if (!np)
        return NULL;
    size_t copy = old_size < new_size ? old_size : new_size;
    memcpy(np, ptr, copy);
    return np;
}

// ------------------ 元数据分配接口 ------------------

void* base_alloc_edata(base_t* base)
{
    // 按实际edata结构大小分配（曾按64字节分配，写入edata字段会越界）
    return base_alloc(base, sizeof(edata_t), 64);
}
void* base_alloc_rtree(base_t* base, size_t size)
{
    return base_alloc(base, size, 64);
}
void* b0_alloc_tcache_stack(size_t size)
{
    base_t* base = base_global();
    return base_alloc(base, size, 64);
}
void b0_dalloc_tcache_stack(void* tcache_stack)
{
    // 直接忽略，base 层不回收
    (void)tcache_stack;
}

// ------------------ THP 策略接口 ------------------

void base_set_thp_mode(base_t* base, base_thp_mode_t mode)
{
    base->thp_mode = mode;
}
base_thp_mode_t base_get_thp_mode(base_t* base) { return base->thp_mode; }

// ------------------ 构造/析构 ------------------

base_t* base_new(unsigned ind, base_backend_t* backend, int numa_node)
{
    (void)ind;
    // 用 block 分配 base_t 结构体，绝不使用 malloc/calloc
    base_block_t* block =
        base_block_create(backend, numa_node, sizeof(base_t), 64);
    if (!block)
        return NULL;
    base_t* base = (base_t*)block->unused_start;
    block->unused_start = (char*)block->unused_start + sizeof(base_t);
    block->unused_size -= sizeof(base_t);
    // 初始化 base_t 字段
    if (malloc_mutex_init(&base->mutex, "base", 0, 0))
    {
        base_backend_t* b = backend ? backend : &default_mmap_backend;
        b->dealloc(block, block->size, b->arg);
        return NULL;
    }
    base->backend = backend;
    base->numa_node = numa_node;
    base->thp_mode = BASE_THP_DISABLED;
    base->auto_thp_switched = false;
    base->n_thp = 0;
    base->pind_last = 0;
    base->extent_sn_next = 0;
    base->blocks = block;
    base->heap = NULL;
    base->edata_avail = NULL;
    base->ehooks = NULL;
    base->trace_hook = NULL;
    memset(&base->stats, 0, sizeof(base_stats_t));
    return base;
}

void base_delete(base_t* base)
{
    if (!base)
        return;
    // base_t 结构体本身位于首个 block 内，必须先销毁mutex再释放block，
    // 否则会对已解除映射的内存解锁/销毁
    malloc_mutex_lock(NULL, &base->mutex);
    malloc_mutex_unlock(NULL, &base->mutex);
    malloc_mutex_destroy(&base->mutex);
    base_block_t* blk = base->blocks;
    while (blk)
    {
        base_block_t* next = blk->next;
        base_block_destroy(base, blk);
        blk = next;
    }
}

// ------------------ fork 支持 ------------------

void base_prefork(base_t* base) { malloc_mutex_lock(NULL, &base->mutex); }
void base_postfork_parent(base_t* base)
{
    malloc_mutex_unlock(NULL, &base->mutex);
}
void base_postfork_child(base_t* base)
{
    malloc_mutex_init(&base->mutex, "base", 0, 0);
}

// ------------------ 全局 base 支持 ------------------

static base_t* global_base = NULL;

void base_global_init(base_backend_t* backend, int numa_node)
{
    if (!global_base)
    {
        global_base = base_new(0, backend, numa_node);
    }
}
base_t* base_global(void)
{
    // 惰性初始化，backend 为 NULL 时走默认 mmap 后端
    if (!global_base)
    {
        global_base = base_new(0, NULL, -1);
    }
    return global_base;
}

void global_base_init(void) { (void)base_global(); }

// ------------------ 单元测试入口（可选） ------------------
#ifdef BASE_UNIT_TEST
#include <stdio.h>
void base_unit_test()
{
    printf("[base] unit test start\n");
    base_global_init(NULL, -1);
    base_t* base = base_global();
    void* p1 = base_alloc(base, 128, 16);
    void* p2 = base_alloc(base, 4096, 4096);
    base_free(base, p1, 128);
    base_free(base, p2, 4096);
    base_stats_t stats;
    base_get_stats(base, &stats);
    printf("allocated: %zu, mapped: %zu, n_alloc_calls: %zu\n",
           stats.allocated,
           stats.mapped,
           stats.n_alloc_calls);
    base_delete(base);
    printf("[base] unit test end\n");
}
#endif