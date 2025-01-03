/*
 * tsd.c - 线程私有数据（TSD）初始化与生命周期管理
 *
 * 典型调用链：malloc_init -> tsd_malloc_boot -> tsd_data_init
 */

#include "tsd.h"
#include "tcache.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>

__thread tsd_t tsd_tls = TSD_INITIALIZER;

pthread_key_t tsd_tsd;
bool tsd_booted = false;

bool tsd_boot(void)
{
    if (!tsd_booted)
    {
        if (pthread_key_create(&tsd_tsd, NULL) != 0)
        {
            return false;
        }
        tsd_booted = true;
    }
    return true;
}

void tsd_state_set(tsd_t* tsd, uint8_t new_state)
{
    atomic_store_explicit(&tsd->state, new_state, memory_order_relaxed);
}

// 初始化线程私有数据字段（arena绑定、tcache等）
void tsd_data_init(tsd_t* tsd)
{
    tsd->tcache_enabled = true;
    tsd->arena = NULL;
    tsd->arena_id = 0;
    tsd->thread_allocated = 0;
    // 惰性初始化线程 tcache
    tsd_tcachep_get(tsd);
}

// 主线程 boot：初始化全局 tsd 并标记 nominal 状态
tsd_t* tsd_malloc_boot(void)
{
    if (!tsd_boot())
    {
        return NULL;
    }
    tsd_t* tsd = tsd_get(true);
    tsd_state_set(tsd, tsd_state_nominal);
    tsd_data_init(tsd);
    return tsd;
}
