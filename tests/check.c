/*
 * check.c - Lmalloc 正确性测试
 *
 * 覆盖：基本分配/释放、多大小类、calloc清零、realloc内容保持、
 * memalign对齐、压力测试、统计接口冒烟测试。
 * 任何断言失败直接 abort，返回码 0 表示全部通过。
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory/lmalloc.h"
#include "arena.h"
#include "bin.h"
#include "lmalloc_stats.h"
#include "tcache.h"
#include "tsd.h"

static int g_pass = 0;

#define CHECK(cond, name)                                                      \
    do                                                                         \
    {                                                                          \
        if (cond)                                                              \
        {                                                                      \
            g_pass++;                                                          \
            printf("[check] PASS: %s\n", name);                                \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            printf("[check] FAIL: %s\n", name);                                \
            abort();                                                           \
        }                                                                      \
    } while (0)

// 基本分配/释放 + 填充校验
static void test_basic(void)
{
    for (size_t sz = 1; sz <= 1024; sz = sz * 2 + 1)
    {
        for (int round = 0; round < 8; ++round)
        {
            void* p = lmalloc(sz);
            assert(p != NULL);
            memset(p, 0x5A, sz);
            for (size_t i = 0; i < sz; ++i)
            {
                assert(((unsigned char*)p)[i] == 0x5A);
            }
            lfree(p);
        }
    }
    lfree(NULL); // 空指针释放必须安全
    CHECK(true, "基本分配/释放与填充校验");
}

// calloc 必须清零
static void test_calloc(void)
{
    void* p = lcalloc(1, 4096);
    assert(p != NULL);
    for (int i = 0; i < 4096; ++i)
    {
        assert(((unsigned char*)p)[i] == 0);
    }
    lfree(p);
    CHECK(true, "calloc 零初始化");
}

// realloc 内容保持
static void test_realloc(void)
{
    void* p = lmalloc(64);
    assert(p != NULL);
    memset(p, 0xAB, 64);
    p = lrealloc(p, 4096);
    assert(p != NULL);
    for (int i = 0; i < 64; ++i)
    {
        assert(((unsigned char*)p)[i] == 0xAB);
    }
    memset(p, 0xCD, 4096);
    lfree(p);

    // 缩小不搬移
    void* q = lmalloc(512);
    memset(q, 0x11, 512);
    void* q2 = lrealloc(q, 128);
    assert(q2 == q);
    lfree(q2);

    // realloc(NULL, n) == lmalloc(n)
    void* r = lrealloc(NULL, 256);
    assert(r != NULL);
    lfree(r);
    CHECK(true, "realloc 扩容内容保持/缩小原地返回");
}

// memalign 对齐检查
static void test_memalign(void)
{
    for (size_t align = 16; align <= 4096; align *= 2)
    {
        for (int i = 0; i < 16; ++i)
        {
            void* p = lmemalign(align, 128 + i * 8);
            assert(p != NULL);
            assert(((uintptr_t)p % align) == 0);
            memset(p, 0x33, 128 + i * 8);
            lfree(p);
        }
    }
    CHECK(true, "memalign 对齐");
}

// 多大小类交错分配/释放
static void test_multi_size(void)
{
    enum
    {
        N = 512
    };
    void* ptrs[N];
    size_t sizes[N];
    for (int i = 0; i < N; ++i)
    {
        sizes[i] = 16 + (i % 64) * 24;
        ptrs[i] = lmalloc(sizes[i]);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0xA5, sizes[i]);
    }
    // 交错释放一半再分配
    for (int i = 0; i < N; i += 2)
    {
        lfree(ptrs[i]);
        ptrs[i] = lmalloc(sizes[i] + 100);
        assert(ptrs[i] != NULL);
    }
    for (int i = 0; i < N; ++i)
    {
        lfree(ptrs[i]);
    }
    CHECK(true, "多大小类交错分配/释放");
}

// 中等大小（超过bin范围走extent红黑树）分配/释放
static void test_large(void)
{
    for (size_t sz = 65536; sz <= 524288; sz *= 2)
    {
        void* p = lmalloc(sz);
        assert(p != NULL);
        memset(p, 0x77, sz);
        lfree(p);
    }
    CHECK(true, "中等大小分配（extent路径）");
}

// 压力测试：多轮分配释放
static void test_stress(void)
{
    for (int round = 0; round < 5; ++round)
    {
        void* ptrs[1000];
        for (int i = 0; i < 1000; ++i)
        {
            ptrs[i] = lmalloc(32 + (i % 128));
            assert(ptrs[i] != NULL);
        }
        for (int i = 0; i < 1000; ++i)
        {
            lfree(ptrs[i]);
        }
    }
    CHECK(true, "压力测试 5轮x1000次分配");
}

// 统计接口冒烟测试：调用不应崩溃且能打印
static void test_stats(void)
{
    lmalloc_enable_stats(true);
    void* p = lmalloc(256);
    assert(p != NULL);
    memset(p, 1, 256);

    lmalloc_stats_print();
    arena_stats_print_all();

    bin_stats_t bs;
    bin_stats_get(&arenas[0]->bins[0], &bs);
    printf("[check] bin0 stats: slabs=%zu used=%zu/%zu\n",
           bs.slab_count,
           bs.nused,
           bs.nregions);

    tcache_t* tcache = tsd_tcachep_get(tsd_fetch());
    tcache_stats_print(tcache);

    lfree(p);
    tcache_flush_all_bins(tcache);
    CHECK(true, "统计接口（arena/bin/tcache）");
}

int main(void)
{
    printf("========== lmalloc check start ==========\n");
    test_basic();
    test_calloc();
    test_realloc();
    test_memalign();
    test_multi_size();
    test_large();
    test_stress();
    test_stats();
    printf("========== all %d checks passed ==========\n", g_pass);
    return 0;
}
