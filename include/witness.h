#pragma once
/*用于线程的锁管理*/
#include <assert.h>
#include <stdlib.h>

#include "ql.h"
#include "ts.h"

typedef enum witness_rank_e witness_rank_t;
enum witness_rank_e
{
    /*越往下锁等级越高*/
    WITNESS_RANK_OMIT,                      // 忽略此锁的检查
    WITNESS_RANK_MIN,                       // 最低的锁优先级
    WITNESS_RANK_INIT = WITNESS_RANK_MIN,   // 初始化阶段锁
    WITNESS_RANK_CTL,                       // 控制器相关的锁
    WITNESS_RANK_TCACHES,                   // 线程缓存的锁
    WITNESS_RANK_ARENAS,                    // 内存池相关的锁
    WITNESS_RANK_BACKGROUND_THREAD_GLOBAL,  // 后台线程的全局锁
    WITNESS_RANK_PROF_DUMP,                 // 性能分析转储锁
    WITNESS_RANK_PROF_BT2GCTX,              // 性能分析的堆栈转储锁
    WITNESS_RANK_PROF_TDATAS,               // 性能分析的线程数据锁
    WITNESS_RANK_PROF_TDATA,                // 性能分析的单个线程数据锁
    WITNESS_RANK_PROF_LOG,                  // 性能分析的日志锁
    WITNESS_RANK_PROF_GCTX,                 // 性能分析的全局上下文锁
    WITNESS_RANK_PROF_RECENT_DUMP,          // 性能分析的最近转储锁
    WITNESS_RANK_BACKGROUND_THREAD,         // 后台线程锁
    WITNESS_RANK_CORE,                      // 核心锁
    WITNESS_RANK_DECAY = WITNESS_RANK_CORE, // 渐变核心锁
    WITNESS_RANK_TCACHE_QL,                 // 线程缓存的队列锁
    WITNESS_RANK_SEC_SHARD,                 // 安全分片锁
    WITNESS_RANK_EXTENT_GROW,               // 扩展增长锁
    WITNESS_RANK_HPA_SHARD_GROW = WITNESS_RANK_EXTENT_GROW, // 高性能分配增长锁
    WITNESS_RANK_SAN_BUMP_ALLOC = WITNESS_RANK_EXTENT_GROW, // 随机分配增长锁
    WITNESS_RANK_EXTENTS,                                   // 扩展相关的锁
    WITNESS_RANK_HPA_SHARD = WITNESS_RANK_EXTENTS,          // 高性能分配分片锁
    WITNESS_RANK_HPA_CENTRAL_GROW,                       // 高性能分配中央增长锁
    WITNESS_RANK_HPA_CENTRAL,                            // 高性能分配中央锁
    WITNESS_RANK_EDATA_CACHE,                            // 数据缓存锁
    WITNESS_RANK_RTREE,                                  // R树相关锁
    WITNESS_RANK_BASE,                                   // 基本锁
    WITNESS_RANK_ARENA_LARGE,                            // 大型内存池锁
    WITNESS_RANK_HOOK,                                   // 钩子锁
    WITNESS_RANK_BIN,                                    // 内存池分配锁
    WITNESS_RANK_LEAF = 0x1000,                          // 树叶级别的锁
    WITNESS_RANK_BATCHER = WITNESS_RANK_LEAF,            // 批处理器锁
    WITNESS_RANK_ARENA_STATS = WITNESS_RANK_LEAF,        // 内存池统计锁
    WITNESS_RANK_COUNTER_ACCUM = WITNESS_RANK_LEAF,      // 计数器积累锁
    WITNESS_RANK_DSS = WITNESS_RANK_LEAF,                // 数据集锁
    WITNESS_RANK_PROF_ACTIVE = WITNESS_RANK_LEAF,        // 活跃性能分析锁
    WITNESS_RANK_PROF_DUMP_FILENAME = WITNESS_RANK_LEAF, // 性能分析转储文件名锁
    WITNESS_RANK_PROF_GDUMP = WITNESS_RANK_LEAF,         // 性能分析的全局转储锁
    WITNESS_RANK_PROF_NEXT_THR_UID =
        WITNESS_RANK_LEAF, // 性能分析的下一个线程UID锁
    WITNESS_RANK_PROF_RECENT_ALLOC = WITNESS_RANK_LEAF, // 性能分析的最近分配锁
    WITNESS_RANK_PROF_STATS = WITNESS_RANK_LEAF,        // 性能分析统计锁
    WITNESS_RANK_PROF_THREAD_ACTIVE_INIT =
        WITNESS_RANK_LEAF, // 性能分析线程活动初始化锁
};

#define WITNESS_INITIALIZER(name, rank) {name, rank, NULL, NULL, {NULL, NULL}}

typedef struct witness_s witness_t;
typedef int witness_comp_t(const witness_t*, void*, const witness_t*, void*);

struct witness_s
{
    const char* name;
    witness_rank_t rank;
    witness_comp_t* comp;
    void* opaque;

    ql_elm(witness_t) link;
};

typedef ql_head(witness_t) witness_list_t;

// 一个线程中管理的锁
typedef struct witness_tsd_s witness_tsd_t;
struct witness_tsd_s
{
    witness_list_t witnesses;
    bool forking;
};

#define WITNESS_TSD_INITIALIZER {ql_head_initializer(witnesses), false}
#define WITNESS_TSDN_NULL       ((witness_tsdn_t*)0)

typedef struct witness_tsdn_s {
    witness_tsd_t witness_tsd;
} witness_tsdn_t;

static inline witness_tsdn_t* tsdn_witness_tsdp_get(void* tsdn) {
    (void)tsdn;
    return NULL;
}

static inline witness_tsdn_t* witness_tsd_tsdn(witness_tsd_t* witness_tsd)
{
    return (witness_tsdn_t*)witness_tsd;
}

static inline bool witness_tsdn_null(witness_tsdn_t* witness_tsdn)
{
    return witness_tsdn == NULL;
}

static inline witness_tsd_t* witness_tsdn_tsd(witness_tsdn_t* witness_tsdn)
{
    assert(!witness_tsdn_null(witness_tsdn));
    return &witness_tsdn->witness_tsd;
}

void witness_init(witness_t* witness,
                  const char* name,
                  witness_rank_t rank,
                  witness_comp_t* comp,
                  void* opaque);

void witnesses_cleanup(witness_tsd_t* witness_tsd);
void witness_prefork(witness_tsd_t* witness_tsd);
void witness_postfork_parent(witness_tsd_t* witness_tsd);
void witness_postfork_child(witness_tsd_t* witness_tsd);

static inline bool witness_owner(witness_tsd_t* witness_tsd,
                                 const witness_t* witness)
{
    witness_list_t* witnesses;
    witness_t* w;

    witnesses = &witness_tsd->witnesses;
    ql_foreach(w, witnesses, link)
    {
        if (w == witness)
        {
            return true;
        }
    }

    return false;
}

static inline void witness_assert_owner(witness_tsdn_t* witness_tsdn,
                                        const witness_t* witness)
{
    witness_tsd_t* witness_tsd;

    if (witness_tsdn_null(witness_tsdn))
    {
        return;
    }
    witness_tsd = witness_tsdn_tsd(witness_tsdn);
    if (witness->rank == WITNESS_RANK_OMIT)
    {
        return;
    }

    if (witness_owner(witness_tsd, witness))
    {
        return;
    }
    abort();
}

static inline void witness_assert_not_owner(witness_tsdn_t* witness_tsdn,
                                            const witness_t* witness)
{
    witness_tsd_t* witness_tsd;
    witness_list_t* witnesses;
    witness_t* w;

    if (witness_tsdn_null(witness_tsdn))
    {
        return;
    }
    witness_tsd = witness_tsdn_tsd(witness_tsdn);
    if (witness->rank == WITNESS_RANK_OMIT)
    {
        return;
    }

    witnesses = &witness_tsd->witnesses;
    ql_foreach(w, witnesses, link)
    {
        if (w == witness)
        {
            abort();
        }
    }
}

static inline unsigned witness_depth_to_rank(witness_list_t* witnesses,
                                             witness_rank_t rank_inclusive)
{
    unsigned d = 0;
    witness_t* w = ql_last(witnesses, link);

    if (w != NULL)
    {
        ql_reverse_foreach(w, witnesses, link)
        {
            if (w->rank < rank_inclusive)
            {
                break;
            }
            d++;
        }
    }

    return d;
}

static inline void witness_assert_depth_to_rank(witness_tsdn_t* witness_tsdn,
                                                witness_rank_t rank_inclusive,
                                                unsigned depth)
{
    if (witness_tsdn_null(witness_tsdn))
    {
        return;
    }

    witness_list_t* witnesses = &witness_tsdn_tsd(witness_tsdn)->witnesses;
    unsigned d = witness_depth_to_rank(witnesses, rank_inclusive);

    if (d != depth)
    {
        abort();
    }
}

static inline void witness_assert_depth(witness_tsdn_t* witness_tsdn,
                                        unsigned depth)
{
    witness_assert_depth_to_rank(witness_tsdn, WITNESS_RANK_MIN, depth);
}

static inline void witness_assert_lockless(witness_tsdn_t* witness_tsdn)
{
    witness_assert_depth(witness_tsdn, 0);
}

static inline void
witness_assert_positive_depth_to_rank(witness_tsdn_t* witness_tsdn,
                                      witness_rank_t rank_inclusive)
{
    if (witness_tsdn_null(witness_tsdn))
    {
        return;
    }

    witness_list_t* witnesses = &witness_tsdn_tsd(witness_tsdn)->witnesses;
    unsigned d = witness_depth_to_rank(witnesses, rank_inclusive);

    if (d == 0)
    {
        abort();
    }
}

static inline void witness_lock(witness_tsdn_t* witness_tsdn,
                                witness_t* witness)
{
    witness_tsd_t* witness_tsd;
    witness_list_t* witnesses;
    witness_t* w;

    if (witness_tsdn_null(witness_tsdn))
    {
        return;
    }
    witness_tsd = witness_tsdn_tsd(witness_tsdn);
    if (witness->rank == WITNESS_RANK_OMIT)
    {
        return;
    }

    witness_assert_not_owner(witness_tsdn, witness);

    witnesses = &witness_tsd->witnesses;
    w = ql_last(witnesses, link);
    if (w == NULL)
    {
        /* No other locks; do nothing. */
    }
    else if (witness_tsd->forking && w->rank <= witness->rank)
    {
        /* Forking, and relaxed ranking satisfied. */
    }
    else if (w->rank > witness->rank)
    {
        /* Not forking, rank order reversal. */
        abort();
    }
    else if (w->rank == witness->rank &&
             (w->comp == NULL || w->comp != witness->comp ||
              w->comp(w, w->opaque, witness, witness->opaque) > 0))
    {

        abort();
    }

    assert(ql_empty(witnesses) || qr_prev(ql_first(witnesses), link) != NULL);
    ql_elm_new(witness, link);
    ql_tail_insert(witnesses, witness, link);
}

static inline void witness_unlock(witness_tsdn_t* witness_tsdn,
                                  witness_t* witness)
{
    witness_tsd_t* witness_tsd;
    witness_list_t* witnesses;

    if (witness_tsdn_null(witness_tsdn))
    {
        return;
    }
    witness_tsd = witness_tsdn_tsd(witness_tsdn);
    if (witness->rank == WITNESS_RANK_OMIT)
    {
        return;
    }

    if (witness_owner(witness_tsd, witness))
    {
        witnesses = &witness_tsd->witnesses;
        ql_remove(witnesses, witness, link);
    }
    else
    {
        witness_assert_owner(witness_tsdn, witness);
    }
}