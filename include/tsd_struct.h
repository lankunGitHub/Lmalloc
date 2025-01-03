#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "atomic.h"
#include "ql.h"
#include "tcache_struct.h"
#include "arena.h"
#include "ts.h"
#include "witness.h"
#include "arena_struct.h"

struct arena_s;

typedef struct rtree_ctx_s {
    int dummy; // 占位，实际应为rtree_ctx实现
} rtree_ctx_t;

typedef struct tsd_s tsd_t;
typedef unsigned szind_t;

// 直接手动声明需要的字段，避免复杂宏
struct tsd_s {
    bool tcache_enabled;
    rtree_ctx_t rtree_ctx;
    atomic_u8_t state;
    uint64_t thread_allocated;
    tcache_t tcache;
    struct arena_s* arena;      // 线程绑定的 arena
    unsigned arena_id;   // 线程绑定的 arena id
};
typedef struct tsd_s tsd_t;

/*
 * tsdn_t 的封装结构，目的是避免 tsd_t 和 tsdn_t 之间的隐式转换。
 * 其中，tsdn_t 允许为空（nullable），而 tsd_t 是非空的（non-nullable），
 * 因此需要进行显式转换，以确保安全性。
 */
struct tsdn_s
{
    tsd_t tsd; // 实际存储的 tsd_t 实例
};

typedef struct tsdn_s tsdn_t;

typedef ql_elm(tsd_t) tsd_link_t;

#undef O

static inline uint8_t tsd_state_get(tsd_t* tsd)
{
    return atomic_load_u8(&tsd->state, ATOMIC_RELAXED);
}

#define O(n, t, nt)                                                            \
    static inline t* tsd_##n##p_get_unsafe(tsd_t* tsd)                         \
    {                                                                          \
        return &tsd->TSD_MANGLE(n);                                            \
    }

// TSD_DATA_SLOW
// TSD_DATA_FAST
#undef O

#define O(n, t, nt)                                                            \
    static inline t* tsd_##n##p_get(tsd_t* tsd)                                \
    {                                                                          \
        /*                                                                     \
         * Because the state might change asynchronously if it's               \
         * nominal, we need to make sure that we only read it once.            \
         */                                                                    \
        uint8_t state = tsd_state_get(tsd);                                    \
        assert(state == tsd_state_nominal ||                                   \
               state == tsd_state_nominal_slow ||                              \
               state == tsd_state_nominal_recompute ||                         \
               state == tsd_state_reincarnated ||                              \
               state == tsd_state_minimal_initialized);                        \
        return tsd_##n##p_get_unsafe(tsd);                                     \
    }
// TSD_DATA_SLOW
// TSD_DATA_FAST
#undef O