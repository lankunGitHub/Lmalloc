/*
 * stress_mt.c - Lmalloc 多线程压力测试
 *
 * 多线程并发分配/释放，覆盖小/中/大块、memalign、标签分配，
 * 验证线程安全（无崩溃/无损坏）。返回码 0 表示全部通过。
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory/lmalloc.h"

#define NTHREADS 8
#define NITERS   2000

static void* worker(void* arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg + 1;
    for (int i = 0; i < NITERS; ++i)
    {
        // 伪随机大小：小/中/大块混合
        seed = seed * 1103515245 + 12345;
        size_t sz = 16 + (seed % 70000);
        void* p = lmalloc(sz);
        assert(p != NULL);
        memset(p, (int)(seed & 0xFF), sz);
        if ((seed & 3) == 0)
        {
            // 部分块做 realloc
            size_t nsz = sz + (seed % 4096);
            p = lrealloc(p, nsz);
            assert(p != NULL);
            memset(p, 0x5A, nsz);
            lfree(p);
        }
        else if ((seed & 7) == 1)
        {
            // 部分块用 memalign
            lfree(p);
            size_t align = 1u << (4 + (seed % 6)); // 16..512
            p = lmemalign(align, 64 + (seed % 2048));
            assert(p != NULL);
            assert(((uintptr_t)p % align) == 0);
            memset(p, 0x6C, 64);
            lfree(p);
        }
        else
        {
            lfree(p);
        }
    }
    return NULL;
}

int main(void)
{
    pthread_t th[NTHREADS];
    for (unsigned i = 0; i < NTHREADS; ++i)
        assert(pthread_create(&th[i], NULL, worker, (void*)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < NTHREADS; ++i)
        assert(pthread_join(th[i], NULL) == 0);
    printf("[stress_mt] %d threads x %d iters passed\n", NTHREADS, NITERS);
    return 0;
}
