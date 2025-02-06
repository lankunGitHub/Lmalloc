#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <pthread.h>
#include <string.h> // Added for memset
#include <unistd.h>
#include <sched.h>
#ifdef HAVE_NUMA
#include <numa.h>
#endif
#ifdef LMALLOC_PROFILE
void lmalloc_profile_export_now(const char* filename);
#endif

#include "tsd.h"
#include "../include/memory/lmalloc.h"

#define TEST_N 100
#define THREAD_N 8
#define PER_THREAD_N 1000

typedef struct {
    int tid;
} thread_arg_t;

void* thread_func(void* arg) {
    int tid = *(int*)arg;
    void* ptrs[PER_THREAD_N];
    size_t sizes[PER_THREAD_N];
    int cpu = sched_getcpu();
#ifdef HAVE_NUMA
    int node = numa_node_of_cpu(cpu);
#else
    int node = -1;
#endif
    for (int i = 0; i < PER_THREAD_N; ++i) {
        sizes[i] = 32 + (i % 16) * 8 + tid * 4;
        ptrs[i] = lmalloc(sizes[i]);
        if (ptrs[i]) memset(ptrs[i], 0x5A, sizes[i]);
    }
    for (int i = 0; i < PER_THREAD_N; ++i) {
        lfree(ptrs[i]);
    }
    printf("[Thread %d] CPU=%d NUMA=%d 分配/回收完成\n", tid, cpu, node);
    return NULL;
}

void test_multi_size() {
    printf("[Test] 多大小类分配/回收\n");
    void* ptrs[TEST_N];
    size_t sizes[TEST_N];
    for (int i = 0; i < TEST_N; ++i) {
        sizes[i] = 16 + (i % 32) * 16;
        ptrs[i] = lmalloc(sizes[i]);
        if (ptrs[i]) memset(ptrs[i], 0xA5, sizes[i]);
    }
    for (int i = 0; i < TEST_N; ++i) {
        lfree(ptrs[i]);
    }
    printf("[Test] 多大小类分配/回收完成\n");
}

void test_multithread() {
    printf("[Test] 多线程/多核/NUMA 分配/回收\n");
    pthread_t threads[THREAD_N];
    int tids[THREAD_N];
    for (int i = 0; i < THREAD_N; ++i) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, thread_func, &tids[i]);
    }
    for (int i = 0; i < THREAD_N; ++i) {
        pthread_join(threads[i], NULL);
    }
    printf("[Test] 多线程/多核/NUMA 分配/回收完成\n");
}

void test_memalign() {
    printf("[Test] 对齐分配\n");
    for (size_t align = 16; align <= 4096; align *= 2) {
        void* p = lmemalign(align, 128);
        printf("lmemalign(%zu, 128) = %p\n", align, p);
        if (p && ((uintptr_t)p % align != 0)) {
            printf("[Error] 未对齐: %p (align %zu)\n", p, align);
        }
        lfree(p);
    }
    printf("[Test] 对齐分配完成\n");
}

void test_realloc() {
    printf("[Test] realloc\n");
    void* p = lmalloc(64);
    memset(p, 0x11, 64);
    p = lrealloc(p, 128);
    memset(p, 0x22, 128);
    p = lrealloc(p, 32);
    memset(p, 0x33, 32);
    lfree(p);
    printf("[Test] realloc 完成\n");
}

void test_profile_export() {
#ifdef LMALLOC_PROFILE
    printf("[Test] profile 导出\n");
    lmalloc_profile_export_now("lmalloc_profile_test.log");
    printf("[Test] profile 导出完成\n");
#endif
}

void test_stress() {
    printf("[Test] 压力测试\n");
    for (int round = 0; round < 10; ++round) {
        void* ptrs[1000];
        for (int i = 0; i < 1000; ++i) {
            ptrs[i] = lmalloc(32 + (i % 128));
        }
        for (int i = 0; i < 1000; ++i) {
            lfree(ptrs[i]);
        }
    }
    printf("[Test] 压力测试完成\n");
}

int main() {
    printf("[test] lmalloc basic test start\n");
    // 开启所有功能开关
    lmalloc_enable_leak(true);
    lmalloc_enable_stats(true);
    lmalloc_enable_profile(true);
    lmalloc_enable_tag(true);
    lmalloc_enable_trace(true);

    // 基本分配/释放
    void* p1 = lmalloc(64);
    if (!p1) { printf("[test] lmalloc failed!\n"); return 1; }
    memset(p1, 0xAB, 64);
    lfree(p1);

    // 带标签分配
    void* p2 = lmalloc_tag(128, "test-tag");
    if (!p2) { printf("[test] lmalloc_tag failed!\n"); return 1; }
    memset(p2, 0xCD, 128);
    lfree_tag(p2);

    // 统计导出（如有实现）
    // lmalloc_stats_print();
    // profile导出（如有实现）
    // lmalloc_profile_export("profile.log");

    printf("[test] lmalloc basic test passed!\n");
    return 0;
}
