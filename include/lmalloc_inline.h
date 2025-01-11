#pragma once

#include "arena.h"
#include "base.h"
#include "bin.h"
#include "mutex.h"
#include "pac.h"
#include "sc_ext.h"
#include "sz.h"
#include "tcache.h"
#include "tsd.h"

#define LMAGIC_ALLOC     0xDEADBEEF
#define LMAGIC_FREED     0xBADC0DE
#define LMAGIC_TAIL      0xFEEDFACE
#define LMAGIC_PAD       0xAA
#define LMAGIC_FREEFILL  0xDD
#define LMAGIC_HEAD_SIZE 16
#define LMAGIC_TAIL_SIZE 16

// 分配块头部元数据结构
// 设计说明：
//   - 记录分配大小、magic、标签、头部保护区
//   - 用于越界检测、双重释放检测、标签统计等
//   - 头部紧跟用户数据，尾部有保护区
//
typedef struct lmalloc_hdr_s
{
    size_t size;                          // 分配大小
    uint32_t magic;                       // magic标记，检测越界/双重释放
    const char* tag;                      // 分配标签
    uint8_t is_aligned;                   // 是否lmemalign调整过的对齐指针
    uint8_t head_guard[LMAGIC_HEAD_SIZE]; // 头部保护区
} lmalloc_hdr_t;

// 全局初始化标志和互斥锁
static bool gl_malloc_init = false;
static malloc_mutex_t init_mutex = MALLOC_MUTEX_INITIALIZER;

// 判断分配器是否已初始化
static inline bool malloc_initialized(void) { return (gl_malloc_init == true); }

// 分配器全局初始化流程
static inline bool malloc_init(void)
{
    malloc_mutex_lock(TSDN_NULL, &init_mutex);
    if (gl_malloc_init)
    {
        malloc_mutex_unlock(TSDN_NULL, &init_mutex);
        return true;
    }
    // 1. 初始化全局静态 size class 分配策略
    sz_boot();
    // 2. 初始化全局 base（全局元数据分配器）
    global_base_init();
    // 3. 初始化全局 arena 列表
    arena_boot();
    // 4. 初始化 tcache 相关全局结构
    tcache_boot();
    // 5. 初始化主线程 TSD
    tsd_t* tsd = tsd_malloc_boot();
    (void)tsd;
    // 6. 标记初始化完成
    gl_malloc_init = true;
    malloc_mutex_unlock(TSDN_NULL, &init_mutex);
    return true;
}

// 分配上下文结构体
typedef struct alloc_ctx_s alloc_ctx_t;
struct alloc_ctx_s
{
    void** ret;
    size_t size;
    size_t usize;
    size_t alignments;
    bool zero;
    unsigned tcache_ind;
    unsigned arena_ind;
};

// 通过 tcache 实现快速分配
static inline bool malloc_fastpath(size_t size, void** ret)
{
    tsd_t* tsd = tsd_get(false);
    if (unlikely(tsd == NULL))
    {
        return false;
    }
    // 计算 size class index（用sz接口）
    size_t ind = sz_size2index(size);
    if (ind == SIZE_MAX || ind >= SC_NBINS)
        return false;
    *ret = tcache_alloc(tsd, size, ind);
    return *ret != NULL;
}

// 无法通过 tcache 分配，使用默认分配
static inline void* malloc_default(size_t size)
{
    tsd_t* tsd = tsd_get(true);
    if (unlikely(tsd == NULL))
    {
        return NULL;
    }
    size_t ind = sz_size2index(size);
    if (ind == SIZE_MAX)
        return NULL;
    size_t usize = sz_index2size(ind);
    if (usize > (1 << 20))
    {
        return pac_alloc(tsd, size, 16);
    }
    arena_t* arena = arena_choose(tsd);
    if (ind < SC_NBINS)
    {
        return arena_malloc_small(arena, ind);
    }
    // 超过bin范围的大块走extent红黑树路径
    return arena_malloc_large(arena, size);
}

// 统计打印接口（只声明）
void lmalloc_stats_print(void);