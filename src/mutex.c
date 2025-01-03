/*
 * mutex.c - 分配器互斥锁实现（pthread mutex + witness 锁顺序校验）
 */

#include "mutex.h"

// 初始化互斥锁，返回 true 表示失败
bool malloc_mutex_init(malloc_mutex_t* mutex,
                       const char* name,
                       witness_rank_t rank,
                       malloc_mutex_lock_order_t lock_order)
{
    (void)lock_order;
    atomic_store_b(&mutex->locked, false, ATOMIC_RELAXED);
    if (pthread_mutex_init(&mutex->lock, NULL) != 0)
    {
        return true;
    }
    witness_init(&mutex->witness, name, rank, NULL, NULL);
    return false;
}

void malloc_mutex_lock_slow(malloc_mutex_t* mutex)
{
    MALLOC_MUTEX_LOCK(mutex);
    atomic_store_b(&mutex->locked, true, ATOMIC_RELAXED);
}

void malloc_mutex_destroy(malloc_mutex_t* mutex)
{
    pthread_mutex_destroy(&mutex->lock);
}

bool malloc_mutex_boot(void)
{
    return false;
}
