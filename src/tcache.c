#include "tcache.h"
#include "arena.h"
#include "base.h"
#include "bin.h"
#include "sz.h"
#include "tsd.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __GNUC__
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif
#include "cache_bin_struct.h"
#include "tcache_struct.h"
#include <stdatomic.h>
#include <time.h>

#define TCACHE_BIN_MIN            8
#define TCACHE_BIN_MAX            128
#define TCACHE_BIN_DEFAULT        32
#define TCACHE_BIN_ADAPT_INTERVAL 2 // 秒
#define TCACHE_BIN_GC_INTERVAL    5 // 秒
#define TCACHE_HIT_THRESHOLD      0.8
#define TCACHE_MISS_THRESHOLD     0.2

__thread tcache_t* thread_tcache = NULL;
// tcache bin 容量全局上限，auto_tune 可动态调整
int tcache_bin_max_global = TCACHE_BIN_MAX;
static pthread_key_t tcache_key;
static bool tcache_key_created = false;

static void tcache_thread_cleanup(void* arg)
{
    (void)arg;
    if (thread_tcache)
    {
        tcache_flush_all_bins(thread_tcache);
        printf("[tcache] thread tcache flushed on exit\n");
    }
}

void tcache_boot(void)
{
    if (!thread_tcache)
    {
        thread_tcache = (tcache_t*)base_calloc(1, sizeof(tcache_t));
        thread_tcache->max_capacity = TCACHE_BIN_MAX;
        thread_tcache->min_capacity = TCACHE_BIN_MIN;
        for (unsigned i = 0; i < TCACHE_NBINS_MAX; ++i)
        {
            cache_bin_t* bin = &thread_tcache->bins[i];
            bin->stack = (void**)base_alloc(
                base_global(), TCACHE_BIN_MAX * sizeof(void*), 64);
            bin->capacity = TCACHE_BIN_DEFAULT;
            bin->max_capacity = TCACHE_BIN_MAX;
            bin->min_capacity = TCACHE_BIN_MIN;
            bin->head = 0;
            bin->obj_timestamps =
                (uint64_t*)base_calloc(TCACHE_BIN_MAX, sizeof(uint64_t));
            bin->hit_count = bin->miss_count = bin->gc_count = 0;
            bin->full = 0;
            bin->empty = 1;
            bin->low_water = 0;
        }
        if (!tcache_key_created)
        {
            pthread_key_create(&tcache_key, tcache_thread_cleanup);
            tcache_key_created = true;
        }
        pthread_setspecific(tcache_key, thread_tcache);
        printf("[tcache_boot] per-thread tcache initialized\n");
    }
}

tcache_t* tsd_tcachep_get(tsd_t* tsd)
{
    (void)tsd;
    tcache_boot();
    return thread_tcache;
}

// 获取当前时间戳（秒）
static inline uint64_t tcache_now() { return (uint64_t)time(NULL); }

// 自适应调整 cache_bin 容量
static void cache_bin_adapt(cache_bin_t* bin)
{
    static uint64_t last_adapt = 0;
    uint64_t now = tcache_now();
    if (now - last_adapt < TCACHE_BIN_ADAPT_INTERVAL)
        return;
    double hit_rate =
        (double)bin->hit_count / (bin->hit_count + bin->miss_count + 1e-9);
    if (hit_rate > TCACHE_HIT_THRESHOLD && bin->capacity < TCACHE_BIN_MAX)
    {
        bin->capacity *= 2;
        if (bin->capacity > TCACHE_BIN_MAX)
            bin->capacity = TCACHE_BIN_MAX;
    }
    else if (hit_rate < TCACHE_MISS_THRESHOLD && bin->capacity > TCACHE_BIN_MIN)
    {
        bin->capacity /= 2;
        if (bin->capacity < TCACHE_BIN_MIN)
            bin->capacity = TCACHE_BIN_MIN;
        if (bin->head > bin->capacity)
            bin->head = bin->capacity;
    }
    bin->hit_count = bin->miss_count = 0;
    last_adapt = now;
}

// LRU GC：淘汰最久未用对象
static void cache_bin_gc_lru(cache_bin_t* bin, arena_t* arena, bin_t* ab)
{
    (void)arena;
    int nflush = bin->capacity / 2;
    for (int i = 0; i < nflush && bin->head > 0; ++i)
    {
        // 找到最久未用对象
        int lru_idx = 0;
        uint64_t lru_time = bin->obj_timestamps[0];
        for (int j = 1; j < bin->head; ++j)
        {
            if (bin->obj_timestamps[j] < lru_time)
            {
                lru_time = bin->obj_timestamps[j];
                lru_idx = j;
            }
        }
        bin_dalloc_region(ab, bin->head ? bin->stack[lru_idx] : NULL);
        // 移除该对象
        for (int j = lru_idx; j < bin->head - 1; ++j)
        {
            bin->stack[j] = bin->stack[j + 1];
            bin->obj_timestamps[j] = bin->obj_timestamps[j + 1];
        }
        bin->head--;
        bin->gc_count++;
    }
}

// lock-free tcache push，维护LRU时间戳和统计
static inline int tcache_bin_push(cache_bin_t* bin, void* ptr)
{
    if (bin->head >= bin->capacity)
        return 0; // full
    bin->stack[bin->head] = ptr;
    bin->obj_timestamps[bin->head] = tcache_now();
    bin->head++;
    bin->full = (bin->head == bin->capacity);
    bin->empty = 0;
    return 1;
}

// lock-free tcache pop，维护LRU时间戳和统计
static inline void* tcache_bin_pop(cache_bin_t* bin)
{
    if (bin->head <= 0)
    {
        bin->empty = 1;
        return NULL;
    }
    bin->head--;
    void* ret = bin->stack[bin->head];
    bin->obj_timestamps[bin->head] = 0;
    bin->full = 0;
    if (bin->head == 0)
        bin->empty = 1;
    return ret;
}

// tcache 分配：自适应调整和 LRU 时间戳
void* tcache_alloc(tsd_t* tsd, size_t size, szind_t ind)
{
    (void)size;
    if (unlikely(ind >= SC_NBINS || ind >= TCACHE_NBINS_MAX))
        return NULL;
    tcache_t* tcache = tsd_tcachep_get(tsd);
    cache_bin_t* bin = &tcache->bins[ind];
    cache_bin_adapt(bin);
    void* ret = tcache_bin_pop(bin);
    if (likely(ret))
    {
        bin->hit_count++;
        bin->obj_timestamps[bin->head] = tcache_now();
        tcache->alloc_count++;
        return ret;
    }
    else
    {
        bin->miss_count++;
    }
    // cache_bin 空，批量 refill
    arena_t* arena = arena_choose(tsd);
    bin_t* ab = &arena->bins[ind];
    int batch = bin->capacity / 2;
    if (batch < 1)
        batch = 1;
    void** batch_arr = (void**)base_alloc(
        base_global(), batch * sizeof(void*), QUANTUM);
    int got = 0;
    for (int i = 0; i < batch; ++i)
    {
        void* p = bin_malloc_region(ab);
        if (unlikely(!p))
            break;
        batch_arr[got++] = p;
    }
    for (int i = 0; i < got; ++i)
    {
        tcache_bin_push(bin, batch_arr[i]);
    }
    // batch_arr 由 base 分配，base 不单独回收，无需 free
    if (got == batch && bin->capacity < TCACHE_BIN_MAX)
    {
        bin->capacity *= 2;
        if (bin->capacity > TCACHE_BIN_MAX)
            bin->capacity = TCACHE_BIN_MAX;
    }
    ret = tcache_bin_pop(bin);
    if (unlikely(!ret))
        return NULL;
    bin->hit_count++;
    bin->obj_timestamps[bin->head] = tcache_now();
    tcache->alloc_count++;
    return ret;
}

// tcache 回收：自适应调整和 LRU 时间戳
void tcache_dalloc(tsd_t* tsd, void* ptr, size_t size, szind_t ind)
{
    // 超过bin范围的大块直接回收到arena extent红黑树
    if (unlikely(ind >= SC_NBINS))
    {
        arena_t* arena = arena_choose(tsd);
        arena_dalloc_large(arena, ptr, size);
        return;
    }
    tcache_t* tcache = tsd_tcachep_get(tsd);
    cache_bin_t* bin = &tcache->bins[ind];
    cache_bin_adapt(bin);
    if (likely(tcache_bin_push(bin, ptr)))
    {
        bin->hit_count++;
        tcache->free_count++;
        return;
    }
    else
    {
        bin->miss_count++;
    }
    // cache_bin 满，LRU GC
    arena_t* arena = arena_choose(tsd);
    bin_t* ab = &arena->bins[ind];
    cache_bin_gc_lru(bin, arena, ab);
    tcache->gc_count += bin->capacity / 2;
    tcache_bin_push(bin, ptr);
}

// 跨线程回收队列（简化版，实际可用无锁队列）
#define TCACHE_HANDOFF_MAX 64
struct tcache_handoff_entry
{
    void* ptr;
    size_t size;
    int binind;
};

static __thread struct tcache_handoff_entry
    tcache_handoff_queue[TCACHE_HANDOFF_MAX];
static __thread int tcache_handoff_head = 0;

// 跨线程回收接口
void tcache_handoff(void* ptr, size_t size, int binind)
{
    if (tcache_handoff_head < TCACHE_HANDOFF_MAX)
    {
        tcache_handoff_queue[tcache_handoff_head++] =
            (struct tcache_handoff_entry) {ptr, size, binind};
    }
    else
    {
        // 队列满，直接回收到arena
        arena_t* arena = arena_choose(tsd_fetch());
        arena_dalloc_small(arena, ptr, binind);
    }
    tcache_t* tcache = tsd_tcachep_get(tsd_fetch());
    tcache->handoff_count++;
}

// 线程分配前自动处理handoff队列
static void tcache_process_handoff(tcache_t* tcache)
{
    for (int i = 0; i < tcache_handoff_head; ++i)
    {
        int binind = tcache_handoff_queue[i].binind;
        cache_bin_t* bin = &tcache->bins[binind];
        if (bin->head < bin->capacity)
        {
            bin->stack[bin->head++] = tcache_handoff_queue[i].ptr;
        }
        else
        {
            // bin满，直接回收
            arena_t* arena = arena_choose(tsd_fetch());
            arena_dalloc_small(arena, tcache_handoff_queue[i].ptr, binind);
        }
    }
    tcache_handoff_head = 0;
}

// tcache flush批量回收
void tcache_flush_all_bins(tcache_t* tcache)
{
    if (!tcache)
        return;
    for (unsigned i = 0; i < TCACHE_NBINS_MAX; ++i)
    {
        cache_bin_t* bin = &tcache->bins[i];
        while (bin->head > 0)
        {
            void* ptr = bin->stack[--bin->head];
            arena_t* arena = arena_choose(tsd_fetch());
            arena_dalloc_small(arena, ptr, i);
            tcache->free_count++;
        }
    }
    tcache->flush_count++;
    tcache_process_handoff(tcache);
}

// 打印tcache整体统计
void tcache_stats_print(tcache_t* tcache)
{
    if (!tcache)
        return;
    size_t cached_objs = 0, total_cap = 0;
    for (unsigned i = 0; i < TCACHE_NBINS_MAX; ++i)
    {
        cached_objs += tcache->bins[i].head;
        total_cap += tcache->bins[i].capacity;
    }
    printf("[tcache %p] alloc=%zu free=%zu flush=%zu gc=%zu handoff=%zu "
           "cached_objs=%zu capacity=%zu\n",
           (void*)tcache,
           tcache->alloc_count,
           tcache->free_count,
           tcache->flush_count,
           tcache->gc_count,
           tcache->handoff_count,
           cached_objs,
           total_cap);
}

// 导出tcache bin统计
void tcache_bin_stats_export_csv(const char* filename)
{
    FILE* f = fopen(filename, "w");
    if (!f)
        return;
    fprintf(f, "bin,capacity,head,hit_count,miss_count,gc_count\n");
    tcache_t* tcache = thread_tcache;
    for (unsigned i = 0; i < TCACHE_NBINS_MAX; ++i)
    {
        cache_bin_t* bin = &tcache->bins[i];
        fprintf(f,
                "%d,%u,%d,%u,%u,%u\n",
                i,
                bin->capacity,
                bin->head,
                bin->hit_count,
                bin->miss_count,
                bin->gc_count);
    }
    fclose(f);
}