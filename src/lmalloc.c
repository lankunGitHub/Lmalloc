/*
 * lmalloc.c - 高性能多线程内存分配器主实现文件
 *
 * 主要职责：
 *   - 实现主分配/释放/重分配/对齐分配等API
 *   - 支持多线程、NUMA、标签、泄漏检测、分配追踪、统计等高级特性
 *   - 提供分配钩子、调试、兼容标准接口等扩展
 *
 * 关键数据结构：
 *   - lmalloc_hdr_t: 分配块头部元数据，含大小、magic、标签、保护区等
 *   - lmalloc_hooks_t: 分配/释放/统计钩子结构
 *
 * 主要算法：
 *   - 小块分配走tcache/arena/bin/slab，大块分配走pac/extent
 *   - 分配/释放时采样调用栈，记录profile事件，支持泄漏检测
 *   - 标签分配、分组统计、分配追踪、NUMA亲和等
 *
 * 并发/NUMA/调优：
 *   - 多arena并发，线程私有数据隔离
 *   - NUMA感知分配，支持大页、节点亲和
 *   - 支持分配钩子、自动调优、统计导出
 *
 * 调试与安全：
 *   - magic字节、头尾保护区、越界检测、双重释放检测
 *   - 详细日志、profile、泄漏检测、标签统计
 *
 * 典型调用链：
 *   lmalloc/lfree/lcalloc/lrealloc/lmemalign -> tcache/arena/bin/pac等
 *
 * 设计trade-off：
 *   - 牺牲部分内存碎片和分配速度，换取并发扩展性和可观测性
 *   - 兼容标准接口，便于集成和迁移
 */
#include "arena.h"
#include "lmalloc_inline.h"
#include "lmalloc_leak.h"
#include "lmalloc_profile.h"
#include "lmalloc_stats.h"
#include "lmalloc_tag.h"
#include "pac.h"
#include "tcache.h"
#include "tsd.h"
#include <execinfo.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern bool g_leak_enabled;
extern bool g_stats_enabled;
extern bool g_profile_enabled;
extern bool g_tag_enabled;
extern bool g_trace_enabled;

#ifdef LMALLOC_DEBUG
#define LMALLOC_LOG(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
#define LMALLOC_LOG(fmt, ...)
#endif

#define LMALLOC_CALLSTACK_DEPTH 8
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

// 分配钩子结构体
// 支持用户自定义分配/释放/统计逻辑，便于集成调试
//
typedef void* (*lmalloc_hook_alloc_t)(size_t size, const char* tag);
typedef void (*lmalloc_hook_free_t)(void* ptr);
typedef void (*lmalloc_hook_stats_t)(void);

typedef struct lmalloc_hooks_s
{
    lmalloc_hook_alloc_t alloc;
    lmalloc_hook_free_t free;
    lmalloc_hook_stats_t stats;
} lmalloc_hooks_t;

static lmalloc_hooks_t g_lmalloc_hooks = {0};

// lmemalign返回的对齐指针前24字节处保存识别token
#define LMAGIC_ALIGNED 0x0BADF00Du

// 恢复用户指针对应的真实块头部（兼容lmemalign返回的对齐指针）
lmalloc_hdr_t* hdr_of(void* ptr)
{
    if (*(uint32_t*)((char*)ptr - 24) == LMAGIC_ALIGNED)
    {
        // 对齐指针前16字节处保存了原始用户指针
        void* user = *((void**)((char*)ptr - 16));
        return (lmalloc_hdr_t*)user - 1;
    }
    return (lmalloc_hdr_t*)ptr - 1;
}

/**
 * @brief 设置自定义分配钩子
 * @param hook 分配函数指针
 */
void lmalloc_set_alloc_hook(lmalloc_hook_alloc_t hook)
{
    g_lmalloc_hooks.alloc = hook;
}
/**
 * @brief 设置自定义释放钩子
 * @param hook 释放函数指针
 */
void lmalloc_set_free_hook(lmalloc_hook_free_t hook)
{
    g_lmalloc_hooks.free = hook;
}
/**
 * @brief 设置自定义统计钩子
 * @param hook 统计函数指针
 */
void lmalloc_set_stats_hook(lmalloc_hook_stats_t hook)
{
    g_lmalloc_hooks.stats = hook;
}

/**
 * @brief 主分配函数，分配size字节内存
 * @param size 分配字节数
 * @return 分配到的用户指针，失败返回NULL
 *
 * 线程安全：多线程安全
 *
 * 算法说明：
 *   - 优先走tcache快速分配，失败则走arena/bin/pac
 *   - 分配头部+用户区+尾部保护区
 *   - 记录profile事件、注册泄漏检测、统计更新
 *   - 支持分配钩子
 *
 * 调试：magic、头尾保护区、越界检测
 */
void* lmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    if (!malloc_initialized())
        malloc_init();
    // 处理调试信号请求的堆dump（信号处理器只置标志，此处安全执行）
    lmalloc_debug_signal_poll();
    if (g_lmalloc_hooks.alloc)
        return g_lmalloc_hooks.alloc(size, NULL);
    size_t total = sizeof(lmalloc_hdr_t) + size + LMAGIC_TAIL_SIZE;
    void* raw = NULL;
    if (!malloc_fastpath(total, &raw))
    {
        raw = malloc_default(total);
    }
    if (unlikely(!raw))
        return NULL;
    lmalloc_hdr_t* hdr = (lmalloc_hdr_t*)raw;
    hdr->size = size;
    hdr->magic = LMAGIC_ALLOC;
    hdr->tag = NULL;
    hdr->is_aligned = 0;
    hdr->pac_base = NULL;
    hdr->pac_size = 0;
    memset(hdr->head_guard, LMAGIC_PAD, LMAGIC_HEAD_SIZE);
    void* user_ptr = (void*)(hdr + 1);
    memset(user_ptr, 0, size);
    uint8_t* tail = (uint8_t*)user_ptr + size;
    memset(tail, LMAGIC_TAIL, LMAGIC_TAIL_SIZE);
    LMALLOC_LOG("[lmalloc] alloc %p size %zu\n", user_ptr, size);
    // 分配追踪
    if (g_profile_enabled)
    {
        void* callstack[LMALLOC_CALLSTACK_DEPTH] = {0};
        int cs_depth = backtrace(callstack, LMALLOC_CALLSTACK_DEPTH);
        lmalloc_profile_entry_t e = {user_ptr,
                                     size,
                                     1,
                                     (uint32_t)pthread_self(),
                                     time(NULL),
                                     NULL,
                                     {0},
                                     0};
        memcpy(e.callstack, callstack, sizeof(void*) * cs_depth);
        e.callstack_depth = cs_depth;
        lmalloc_profile_log_event(&e);
    }
    // 泄漏检测注册
    if (g_leak_enabled)
    {
        leak_register(user_ptr, size, NULL, (uint32_t)pthread_self());
    }
    // 信息统计
    if (g_stats_enabled)
    {
        atomic_fetch_add(&g_lmalloc_stats.alloc_count, 1);
        atomic_fetch_add(&g_lmalloc_stats.current_bytes, size);
        size_t cur = atomic_load(&g_lmalloc_stats.current_bytes);
        size_t peak = atomic_load(&g_lmalloc_stats.peak_bytes);
        if (cur > peak)
            atomic_store(&g_lmalloc_stats.peak_bytes, cur);
    }
    return user_ptr;
}

/**
 * @brief 带标签分配，便于分组统计/归因/热点分析
 * @param size 分配字节数
 * @param tag 标签字符串
 * @return 分配到的用户指针，失败返回NULL
 *
 * 线程安全：多线程安全
 *
 * 算法说明：
 *   - 与lmalloc类似，额外记录tag
 *   - 标签统计、profile、泄漏检测均支持tag
 */
void* lmalloc_tagged(size_t size, const char* tag)
{
    if (size == 0)
        return NULL;
    if (!malloc_initialized())
        malloc_init();
    if (g_lmalloc_hooks.alloc)
        return g_lmalloc_hooks.alloc(size, tag);
    // 尾部保护区与lmalloc一致（LMAGIC_TAIL_SIZE字节）：
    // 曾只预留sizeof(uint32_t)且未初始化head_guard，导致lfree按16字节
    // 校验误报溢出，且大小类错配（同块按两个大小类分配/释放会写坏相邻块）
    size_t total = size + sizeof(lmalloc_hdr_t) + LMAGIC_TAIL_SIZE;
    void* raw = NULL;
    if (!malloc_fastpath(total, &raw))
        raw = malloc_default(total);
    if (!raw)
        return NULL;
    lmalloc_hdr_t* hdr = (lmalloc_hdr_t*)raw;
    hdr->size = size;
    hdr->magic = LMAGIC_ALLOC;
    hdr->tag = tag;
    hdr->is_aligned = 0;
    hdr->pac_base = NULL;
    hdr->pac_size = 0;
    memset(hdr->head_guard, LMAGIC_PAD, LMAGIC_HEAD_SIZE);
    void* user_ptr = (void*)(hdr + 1);
    memset(user_ptr, 0xAA, size);
    uint8_t* tail = (uint8_t*)user_ptr + size;
    memset(tail, LMAGIC_TAIL, LMAGIC_TAIL_SIZE);
    LMALLOC_LOG("[lmalloc] alloc(tag) %p size %zu tag=%s\n",
                user_ptr,
                size,
                tag ? tag : "");
    // 分配追踪
    void* callstack[LMALLOC_CALLSTACK_DEPTH] = {0};
    int cs_depth = backtrace(callstack, LMALLOC_CALLSTACK_DEPTH);
    lmalloc_profile_entry_t e = {
        user_ptr, size, 1, (uint32_t)pthread_self(), time(NULL), tag, {0}, 0};
    memcpy(e.callstack, callstack, sizeof(void*) * cs_depth);
    e.callstack_depth = cs_depth;
    if (g_profile_enabled)
        lmalloc_profile_log_event(&e);
    // 标签统计：主路径记账（alloc_count/current_bytes/peak_bytes）
    lmalloc_tag_account_alloc(tag, size);
    // 泄漏检测注册
    if (g_leak_enabled)
    {
        leak_register(user_ptr, size, tag, (uint32_t)pthread_self());
    }
    if (g_stats_enabled)
    {
        atomic_fetch_add(&g_lmalloc_stats.alloc_count, 1);
        atomic_fetch_add(&g_lmalloc_stats.current_bytes, size);
        size_t cur = atomic_load(&g_lmalloc_stats.current_bytes);
        size_t peak = atomic_load(&g_lmalloc_stats.peak_bytes);
        if (cur > peak)
            atomic_store(&g_lmalloc_stats.peak_bytes, cur);
    }
    return user_ptr;
}

/**
 * @brief 释放由lmalloc/lcalloc/lrealloc/lmemalign分配的内存
 * @param ptr 用户指针
 *
 * 线程安全：多线程安全
 *
 * 算法说明：
 *   - 检查magic、头尾保护区，防止越界/双重释放
 *   - 记录profile事件、注销泄漏检测、标签统计
 *   - 支持释放钩子
 */
void lfree(void* ptr)
{
    if (unlikely(!ptr))
        return;
    if (g_lmalloc_hooks.free)
    {
        g_lmalloc_hooks.free(ptr);
        return;
    }
    lmalloc_hdr_t* hdr = hdr_of(ptr);
    size_t size = hdr->size;
    if (unlikely(hdr->magic != LMAGIC_ALLOC))
    {
        fprintf(stderr, "[lmalloc] Double free or corruption detected!\n");
        return;
    }
    // 检查头部和尾部magic
    for (int i = 0; i < LMAGIC_HEAD_SIZE; ++i)
    {
        if (hdr->head_guard[i] != LMAGIC_PAD)
        {
            fprintf(stderr, "[lmalloc] Head overflow detected!\n");
            break;
        }
    }
    uint8_t* user_ptr = (uint8_t*)(hdr + 1);
    // 检查尾部magic字节，防止越界写
    for (int i = 0; i < LMAGIC_TAIL_SIZE; ++i)
    {
        if (user_ptr[hdr->size + i] != (uint8_t)LMAGIC_TAIL)
        {
            fprintf(stderr, "[lmalloc] Tail overflow detected!\n");
            break;
        }
    }
    hdr->magic = LMAGIC_FREED;
    memset(user_ptr, LMAGIC_FREEFILL, hdr->size);
    LMALLOC_LOG("[lmalloc] free %p size %zu\n", ptr, size);
    // 分配追踪
    void* callstack[LMALLOC_CALLSTACK_DEPTH] = {0};
    int cs_depth = backtrace(callstack, LMALLOC_CALLSTACK_DEPTH);
    lmalloc_profile_entry_t e = {
        ptr, size, 0, (uint32_t)pthread_self(), time(NULL), hdr->tag, {0}, 0};
    memcpy(e.callstack, callstack, sizeof(void*) * cs_depth);
    e.callstack_depth = cs_depth;
    if (g_profile_enabled)
        lmalloc_profile_log_event(&e);
    // 标签统计：主路径记账（free_count/current_bytes回退）
    lmalloc_tag_account_free(hdr->tag, size);
    // 泄漏检测注销
    if (g_leak_enabled)
    {
        // 用头部对应的用户指针注销：lmemalign返回的对齐指针与注册时
        // 的内层指针不同，用传入指针会永远匹配不上（表项残留）
        leak_unregister((void*)(hdr + 1));
    }
    if (g_stats_enabled)
    {
        atomic_fetch_add(&g_lmalloc_stats.free_count, 1);
        atomic_fetch_sub(&g_lmalloc_stats.current_bytes, size);
    }
    tsd_t* tsd = tsd_get(false);
    // pac大块路径：按hdr记录的映射基址整段munmap，与大小类无关
    // （lmemalign高对齐小块的大小类可能落在bin范围内，不能按大小类路由）
    if (hdr->pac_base)
    {
        pac_dalloc(tsd, hdr->pac_base, hdr->pac_size);
        return;
    }
    // 按整块大小（头部+用户区+尾部保护区）计算size class
    size_t total = sizeof(lmalloc_hdr_t) + size + LMAGIC_TAIL_SIZE;
    szind_t ind = sz_size2index(total);
    void* raw = (void*)hdr;
    if (sz_index2size(ind) > (1 << 20))
    {
        // 大块走mmap，直接munmap回收
        pac_dalloc(tsd, raw, total);
        return;
    }
    tcache_dalloc(tsd, raw, total, ind);
}

/**
 * @brief 分配nmemb个size字节的零初始化内存，类似calloc
 * @param nmemb 元素个数
 * @param size 单个元素字节数
 * @return 分配到的用户指针，失败返回NULL
 *
 * 线程安全：多线程安全
 */
void* lcalloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void* p = lmalloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

/**
 * @brief 调整已分配内存大小，类似realloc
 * @param ptr 原指针
 * @param size 新字节数
 * @return 新分配的用户指针，失败返回NULL
 *
 * 线程安全：多线程安全
 *
 * 算法说明：
 *   - 若ptr为NULL，等价于lmalloc
 *   - 若size<=原大小，直接返回原指针
 *   - 否则分配新块，拷贝数据，释放原块
 */
void* lrealloc(void* ptr, size_t size)
{
    if (!ptr)
        return lmalloc(size);
    lmalloc_hdr_t* hdr = hdr_of(ptr);
    size_t old_size = hdr->size;
    if (size <= old_size)
        return ptr;
    void* newp = lmalloc(size);
    if (newp)
    {
        memcpy(newp, ptr, old_size);
        memset(
            (char*)newp + old_size, 0xCC, size - old_size); // 新增部分填充0xCC
        lfree(ptr);
    }
    return newp;
}

/**
 * @brief 按alignment字节对齐分配size字节内存，类似posix_memalign
 * @param alignment 对齐字节数（2的幂）
 * @param size 分配字节数
 * @return 分配到的对齐指针，失败返回NULL
 *
 * 线程安全：多线程安全
 *
 * 算法说明：
 *   - 大块或高对齐走pac_alloc
 *   - 否则分配多余空间，手动对齐
 */
void* lmemalign(size_t alignment, size_t size)
{
    if (size == 0)
        return NULL;
    if (size > (1 << 20) || alignment > 4096)
    {
        // pac大块/高对齐路径：在用户指针前放置标准头部，
        // hdr->pac_base/pac_size记录映射基址和长度，lfree按常规流程回收。
        // 曾直接返回裸指针，导致lfree报corruption且永不munmap。
        if (!malloc_initialized())
            malloc_init();
        size_t total = sizeof(lmalloc_hdr_t) + size + LMAGIC_TAIL_SIZE;
        tsd_t* tsd = tsd_get(true);
        void* raw = pac_alloc(tsd, total + alignment, alignment);
        if (!raw)
            return NULL;
        uintptr_t user_addr =
            ((uintptr_t)raw + sizeof(lmalloc_hdr_t) + alignment - 1) &
            ~(alignment - 1);
        lmalloc_hdr_t* hdr =
            (lmalloc_hdr_t*)(user_addr - sizeof(lmalloc_hdr_t));
        hdr->size = size;
        hdr->magic = LMAGIC_ALLOC;
        hdr->tag = NULL;
        hdr->is_aligned = 0;
        hdr->pac_base = raw;
        hdr->pac_size = total + alignment;
        memset(hdr->head_guard, LMAGIC_PAD, LMAGIC_HEAD_SIZE);
        uint8_t* user = (uint8_t*)user_addr;
        memset(user, 0, size);
        uint8_t* tail = user + size;
        memset(tail, LMAGIC_TAIL, LMAGIC_TAIL_SIZE);
        // 统计与泄漏检测注册（与lmalloc保持一致）
        if (g_leak_enabled)
            leak_register(user, size, NULL, (uint32_t)pthread_self());
        if (g_stats_enabled)
        {
            atomic_fetch_add(&g_lmalloc_stats.alloc_count, 1);
            atomic_fetch_add(&g_lmalloc_stats.current_bytes, size);
            size_t cur = atomic_load(&g_lmalloc_stats.current_bytes);
            size_t peak = atomic_load(&g_lmalloc_stats.peak_bytes);
            if (cur > peak)
                atomic_store(&g_lmalloc_stats.peak_bytes, cur);
        }
        return (void*)user_addr;
    }
    // 多分配 alignment+24 空间用于对齐调整和恢复信息
    void* p = lmalloc(size + alignment + 24);
    if (!p)
        return NULL;
    lmalloc_hdr_t* hdr = (lmalloc_hdr_t*)p - 1;
    hdr->is_aligned = 1;
    // 对齐指针前留出24字节：[-24]识别token，[-16]原始用户指针
    uintptr_t aligned = (((uintptr_t)p + 24) + alignment - 1) & ~(alignment - 1);
    *(uint32_t*)((char*)aligned - 24) = LMAGIC_ALIGNED;
    *((void**)((char*)aligned - 16)) = p;
    return (void*)aligned;
}

/**
 * @brief mallopt兼容接口，部分参数支持
 * @param param 参数
 * @param value 值
 * @return 是否支持
 */
int my_mallopt(int param, int value)
{
    (void)value;
    switch (param)
    {
    case 1: // M_MMAP_THRESHOLD
        return 1;
    case 2: // M_TRIM_THRESHOLD
        return 1;
    case 3: // M_ARENA_MAX
        return 1;
    default:
        return 0;
    }
}

/**
 * @brief 查询分配块实际可用大小，类似malloc_usable_size
 * @param ptr 分配指针
 * @return 实际可用字节数
 */
size_t my_malloc_usable_size(void* ptr)
{
    if (!ptr)
        return 0;
    lmalloc_hdr_t* hdr = (lmalloc_hdr_t*)ptr - 1;
    return hdr->size;
}

/**
 * @brief 打印分配器统计信息，兼容glibc malloc_stats
 */
void my_malloc_stats(void)
{
    printf("[lmalloc stats] alloc_count=%zu free_count=%zu current_bytes=%zu "
           "peak_bytes=%zu\n",
           (size_t)atomic_load(&g_lmalloc_stats.alloc_count),
           (size_t)atomic_load(&g_lmalloc_stats.free_count),
           (size_t)atomic_load(&g_lmalloc_stats.current_bytes),
           (size_t)atomic_load(&g_lmalloc_stats.peak_bytes));
}

#define mallopt            my_mallopt
#define malloc_usable_size my_malloc_usable_size
#define malloc_stats       my_malloc_stats