#pragma once

#include "tsd_struct.h"

#include <pthread.h>

#define TSDN_NULL ((tsdn_t*)0)
static inline tsdn_t* tsd_tsdn(tsd_t* tsd) { return (tsdn_t*)tsd; }

static inline bool tsdn_null(const tsdn_t* tsdn) { return tsdn == NULL; }

static inline tsd_t* tsdn_tsd(tsdn_t* tsdn)
{
    assert(!tsdn_null(tsdn));

    return &tsdn->tsd;
}

extern pthread_key_t tsd_tsd;
extern bool tsd_booted;

#ifdef USE_TSD_TLS
extern __thread tsd_t __attribute__((tls_model("initial-exec"))) tsd_tls;

static inline tsd_t* tsd_get(bool init) { (void)init; return &tsd_tls; }

static inline void tsd_set(tsd_t* val)
{
    assert(tsd_booted);
    if (likely(&tsd_tls != val))
    {
        tsd_tls = (*val);
    }
    if (pthread_setspecific(tsd_tsd, (void*)(&tsd_tls)) != 0)
    {
        abort();
    }
}
#else

typedef struct
{
    bool initialized;
    tsd_t val;
} tsd_wrapper_t;

typedef struct tsd_init_block_s tsd_init_block_t;
struct tsd_init_block_s
{
    ql_elm(tsd_init_block_t) link;
    pthread_t thread;
    void* data;
};

tsd_wrapper_t tsd_boot_wrapper;
tsd_init_block_t tsd_init_head;

static inline void tsd_cleanup_wrapper(void* arg)
{
    tsd_wrapper_t* wrapper = (tsd_wrapper_t*)arg;

    if (wrapper->initialized)
    {
        wrapper->initialized = false;
        tsd_cleanup(&wrapper->val);
        if (wrapper->initialized)
        {
            if (pthread_setspecific(tsd_tsd, (void*)wrapper) != 0)
            {
                abort();
            }
            return;
        }
    }
    malloc_tsd_dalloc(wrapper);
}

static inline void tsd_wrapper_set(tsd_wrapper_t* wrapper)
{
    if (unlikely(!tsd_booted))
    {
        return;
    }
    if (pthread_setspecific(tsd_tsd, (void*)wrapper) != 0)
    {
        abort();
    }
}

static inline tsd_wrapper_t* tsd_wrapper_get(bool init)
{
    tsd_wrapper_t* wrapper;

    if (unlikely(!tsd_booted))
    {
        return &tsd_boot_wrapper;
    }

    wrapper = (tsd_wrapper_t*)pthread_getspecific(tsd_tsd);

    if (init && unlikely(wrapper == NULL))
    {
        tsd_init_block_t block;
        wrapper =
            (tsd_wrapper_t*)tsd_init_check_recursion(&tsd_init_head, &block);
        /*进行递归检查的原因是在线程初始化过程中，可能其他线程访问本线程数据，导致本线程多次进行初始化*/
        if (wrapper)
        {
            return wrapper;
        }
        wrapper = (tsd_wrapper_t*)malloc_tsd_malloc(sizeof(tsd_wrapper_t));
        block.data = (void*)wrapper;
        if (wrapper == NULL)
        {
            abort();
        }
        else
        {
            wrapper->initialized = false;
            tsd_t initializer = TSD_INITIALIZER;
            wrapper->val = initializer;
        }
        tsd_wrapper_set(wrapper);
        tsd_init_finish(&tsd_init_head, &block);
    }
    return wrapper;
}

static inline tsd_t* tsd_get(bool init)
{
    tsd_wrapper_t* wrapper;

    assert(tsd_booted);
    wrapper = tsd_wrapper_get(init);

    return &wrapper->val;
}

static inline void tsd_set(tsd_t* val)
{
    tsd_wrapper_t* wrapper;

    assert(tsd_booted);
    wrapper = tsd_wrapper_get(true);
    if (likely(&wrapper->val != val))
    {
        wrapper->val = *(val);
    }
    wrapper->initialized = true;
}

#endif

// tsd 状态常量
#define tsd_state_uninitialized 0
#define tsd_state_nominal 1
#define tsd_state_nominal_slow 2
#define tsd_state_nominal_recompute 3
#define tsd_state_reincarnated 4
#define tsd_state_minimal_initialized 5

// tsd_t 聚合初始化器（字段顺序见 tsd_struct.h）
#define TSD_INITIALIZER                                                       \
    {                                                                         \
        false,                                /* tcache_enabled */            \
        {0},                                  /* rtree_ctx */                 \
        0,                                    /* state */                     \
        0,                                    /* thread_allocated */          \
        { {{0}}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, /* tcache */                  \
        NULL,                                 /* arena */                     \
        0                                     /* arena_id */                  \
    }

// 获取当前线程 tsd（不初始化）
static inline tsd_t* tsd_fetch(void) { return tsd_get(false); }

void tsd_state_set(tsd_t* tsd, uint8_t new_state);
bool tsd_boot(void);
void tsd_data_init(tsd_t* tsd);
tsd_t* tsd_malloc_boot();
