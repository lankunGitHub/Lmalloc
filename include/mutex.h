#pragma once

#include <pthread.h>
#include <stdatomic.h>

#include "atomic.h"
#include "witness.h"

// 前向声明，避免 mutex.h <-> tsd_struct.h 循环依赖
typedef struct tsdn_s tsdn_t;

typedef struct witness_tsdn_s witness_tsdn_t;

static bool isthreaded = true;

typedef enum
{
    malloc_mutex_rank_exclusive,
    malloc_mutex_address_ordered
} malloc_mutex_lock_order_t;

#define MALLOC_MUTEX_INITIALIZER \
    {false, PTHREAD_MUTEX_INITIALIZER, WITNESS_INITIALIZER("mutex", WITNESS_RANK_OMIT)}

#define MALLOC_MUTEX_LOCK(m)    pthread_mutex_lock(&(m)->lock)
#define MALLOC_MUTEX_UNLOCK(m)  pthread_mutex_unlock(&(m)->lock)
#define MALLOC_MUTEX_TRYLOCK(m) (pthread_mutex_trylock(&(m)->lock) != 0)

#define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_DEFAULT

typedef struct malloc_mutex_s {
    atomic_bool locked;
    pthread_mutex_t lock;
    witness_t witness;
} malloc_mutex_t;

bool malloc_mutex_init(malloc_mutex_t* mutex,
                       const char* name,
                       witness_rank_t rank,
                       malloc_mutex_lock_order_t lock_order);
void malloc_mutex_prefork(tsdn_t* tsdn, malloc_mutex_t* mutex);
void malloc_mutex_postfork_parent(tsdn_t* tsdn, malloc_mutex_t* mutex);
void malloc_mutex_postfork_child(tsdn_t* tsdn, malloc_mutex_t* mutex);
bool malloc_mutex_boot(void);
void malloc_mutex_destroy(malloc_mutex_t* mutex);

void malloc_mutex_lock_slow(malloc_mutex_t* mutex);

static inline void malloc_mutex_lock_final(malloc_mutex_t* mutex)
{
    MALLOC_MUTEX_LOCK(mutex);
    atomic_store_b(&mutex->locked, true, ATOMIC_RELAXED);
}

static inline bool malloc_mutex_trylock_final(malloc_mutex_t* mutex)
{
    bool failed = MALLOC_MUTEX_TRYLOCK(mutex);
    if (!failed)
    {
        atomic_store_b(&mutex->locked, true, ATOMIC_RELAXED);
    }

    return failed;
}

static inline bool malloc_mutex_is_locked(malloc_mutex_t* mutex)
{
    /* Used for sanity checking only. */
    return atomic_load_b(&mutex->locked, ATOMIC_RELAXED);
}

static inline void mutex_owner_stats_update(void* tsdn, void* mutex) {
    (void)tsdn; (void)mutex;
}

static inline bool malloc_mutex_trylock(tsdn_t* tsdn, malloc_mutex_t* mutex)
{
    witness_assert_not_owner(tsdn_witness_tsdp_get(tsdn), &mutex->witness);
    if (isthreaded)
    {
        if (malloc_mutex_trylock_final(mutex))
        {
            return true;
        }
        assert(malloc_mutex_is_locked(mutex));
        mutex_owner_stats_update(tsdn, mutex);
    }
    witness_lock(tsdn_witness_tsdp_get(tsdn), &mutex->witness);

    return false;
}

static inline void malloc_mutex_lock(tsdn_t* tsdn, malloc_mutex_t* mutex)
{
    witness_assert_not_owner(tsdn_witness_tsdp_get(tsdn), &mutex->witness);
    if (isthreaded)
    {
        if (malloc_mutex_trylock_final(mutex))
        {
            malloc_mutex_lock_slow(mutex);
        }
        assert(malloc_mutex_is_locked(mutex));
        mutex_owner_stats_update(tsdn, mutex);
    }
    witness_lock(tsdn_witness_tsdp_get(tsdn), &mutex->witness);
}

static inline void malloc_mutex_unlock(tsdn_t* tsdn, malloc_mutex_t* mutex)
{
    witness_unlock(tsdn_witness_tsdp_get(tsdn), &mutex->witness);
    if (isthreaded)
    {
        assert(malloc_mutex_is_locked(mutex));
        atomic_store_b(&mutex->locked, false, ATOMIC_RELAXED);
        MALLOC_MUTEX_UNLOCK(mutex);
    }
}

static inline void malloc_mutex_assert_owner(tsdn_t* tsdn,
                                             malloc_mutex_t* mutex)
{
    witness_assert_owner(tsdn_witness_tsdp_get(tsdn), &mutex->witness);
    if (isthreaded)
    {
        assert(malloc_mutex_is_locked(mutex));
    }
}

static inline void malloc_mutex_assert_not_owner(tsdn_t* tsdn,
                                                 malloc_mutex_t* mutex)
{
    witness_assert_not_owner(tsdn_witness_tsdp_get(tsdn), &mutex->witness);
}