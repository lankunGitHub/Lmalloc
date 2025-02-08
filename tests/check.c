/*
 * check.c - Lmalloc 正确性测试
 *
 * 覆盖：基本分配/释放、多大小类、calloc清零、realloc内容保持、
 * memalign对齐、压力测试、统计接口冒烟测试。
 * 任何断言失败直接 abort，返回码 0 表示全部通过。
 */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory/lmalloc.h"
#include "arena.h"
#include "bin.h"
#include "lmalloc_stats.h"
#include "sz.h"
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

// memalign 大块/高对齐走 pac 路径，分配应被统计且 lfree 可正常回收
// 回归：曾直接返回裸指针，lfree 报 corruption 且永不 munmap
static void test_memalign_large(void)
{
    lmalloc_enable_stats(true);
    size_t before = (size_t)atomic_load(&g_lmalloc_stats.current_bytes);
    void* p = lmemalign(16, (1 << 20) + 4096);
    assert(p != NULL);
    memset(p, 0x44, (1 << 20) + 4096);
    size_t after_alloc = (size_t)atomic_load(&g_lmalloc_stats.current_bytes);
    assert(after_alloc >= before + (1 << 20));
    lfree(p);
    assert(atomic_load(&g_lmalloc_stats.current_bytes) == before);

    void* q = lmemalign(8192, 256); // alignment > 4096 走 pac
    assert(q != NULL);
    assert(((uintptr_t)q % 8192) == 0);
    memset(q, 0x55, 256);
    lfree(q);
    assert(atomic_load(&g_lmalloc_stats.current_bytes) == before);
    // 回归：pac块的hdr曾被误按大小类缓存进tcache（544类<1MB），
    // flush时撞上无关slab触发corruption
    tcache_t* t = tsd_tcachep_get(tsd_fetch());
    tcache_flush_all_bins(t);
    assert(atomic_load(&g_lmalloc_stats.current_bytes) == before);
    CHECK(true, "memalign 大块/高对齐路径");
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

// 11KB~64KB 历史死区：大小类超过 slab 容量却被路由到 bin，
// 导致分配返回 NULL（回归：bin 数量必须与大小类表一致）
static void test_mid_size(void)
{
    size_t sizes[] = {12000, 13000, 16384, 20000, 30000, 50000, 60000};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
    {
        void* p = lmalloc(sizes[i]);
        assert(p != NULL);
        memset(p, 0x7E, sizes[i]);
        lfree(p);
    }
    CHECK(true, "中等大小分配（11KB~64KB 历史死区）");
}

// 连续大块分配的用户指针必须保持16字节对齐
// 回归：extent红黑树分割未对齐尺寸导致后续分配地址漂移（未对齐指针）
static void test_large_aligned(void)
{
    size_t sizes[] = {12000, 13000, 20000};
    void* ptrs[3];
    for (int i = 0; i < 3; ++i)
    {
        ptrs[i] = lmalloc(sizes[i]);
        assert(ptrs[i] != NULL);
        assert(((uintptr_t)ptrs[i] % 16) == 0);
        memset(ptrs[i], 0x3C, sizes[i]);
    }
    for (int i = 0; i < 3; ++i)
        lfree(ptrs[i]);
    CHECK(true, "连续大块分配对齐");
}

// 大小类表一致性：查找结果不小于请求，且组内首档大小类不缺失
static void test_size_class_table(void)
{
    for (size_t sz = 1; sz <= (1 << 16); sz += 7)
    {
        assert(sz_index2size(sz_size2index(sz)) >= sz);
    }
    // 修复前 4096 被归到 4608（组内首档大小类缺失，内部碎片大）
    assert(sz_index2size(sz_size2index(4096)) == 4352);
    CHECK(true, "大小类表一致性");
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

// 4096 大小类的 bin 每 slab 仅 3 个 region，批量分配会填满多块 slab，
// 回归：full_slabs 满链管理不应死循环（曾因环链表按线性遍历挂死）
static void test_multi_slab(void)
{
    enum
    {
        N = 32
    };
    void* ptrs[N];
    for (int i = 0; i < N; ++i)
    {
        ptrs[i] = lmalloc(4096);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0x5C, 4096);
    }
    for (int i = 0; i < N; ++i)
    {
        lfree(ptrs[i]);
    }
    CHECK(true, "4096 多 slab 满链分配/释放");
}

// 标签分配：尾部保护区大小类一致性 + 主路径统计记账
// 回归：尾部曾只有4字节导致大小类错配（写坏相邻块），
// 且主路径标签统计恒为0/虚高
static void test_tagged(void)
{
    lmalloc_enable_tag(true);
    // 190..283 跨 272/288 大小类边界
    void* ptrs[32];
    for (int i = 0; i < 32; ++i)
    {
        size_t sz = 190 + i * 3;
        ptrs[i] = lmalloc_tagged(sz, "check-tagged");
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0xE4, sz);
    }
    for (int i = 0; i < 32; ++i)
    {
        lfree(ptrs[i]);
    }
    // 主路径记账：分配/释放计数与当前字节数一致
    lmalloc_tag_stat_t st;
    assert(lmalloc_tag_stats("check-tagged", &st));
    assert(atomic_load(&st.alloc_count) == 32);
    assert(atomic_load(&st.free_count) == 32);
    assert(atomic_load(&st.current_bytes) == 0);
    // 后续分配完整性（曾报 double free or corruption）
    for (int i = 0; i < 16; ++i)
    {
        void* p = lmalloc(200);
        assert(p != NULL);
        memset(p, 1, 200);
        lfree(p);
    }
    CHECK(true, "标签分配/释放与统计记账");
}

// 大块释放后地址应可复用
// 回归：extent红黑树分割后键序失效导致best-fit漏配，空闲块被永久搁置
static void test_extent_reuse(void)
{
    void* p = lmalloc(50000);
    assert(p != NULL);
    memset(p, 0x2B, 50000);
    lfree(p);
    void* q = lmalloc(40000);
    assert(q != NULL);
    assert(q == p); // 合并回extent后原地址复用
    memset(q, 0x2C, 40000);
    lfree(q);
    CHECK(true, "大块extent释放后复用");
}

// handoff 接口的 binind 校验：越界不得野写崩溃
// 回归：曾直接索引tcache->bins[binind]导致野写SIGSEGV
static void test_handoff_validate(void)
{
    tcache_handoff((void*)0x1000, 64, 100000); // 越界binind
    tcache_handoff((void*)0x2000, 64, -5);     // 负值binind
    tcache_t* t = tsd_tcachep_get(tsd_fetch());
    tcache_flush_all_bins(t); // flush不应因handoff队列崩溃
    CHECK(true, "handoff binind 越界校验");
}

// lmemalign 的泄漏检测注册/注销一致性
// 回归：注销用对齐指针、注册用内层指针，永不匹配导致表项残留
static void test_leak_memalign(void)
{
    lmalloc_enable_leak(true);
    void* p = lmemalign(16, 128);
    assert(p != NULL);
    memset(p, 0x9A, 128);
    lfree(p);
    lmalloc_leak_info_t buf[8];
    size_t n = lmalloc_leak_scan(buf, 8, 0);
    assert(n == 0);
    lmalloc_enable_leak(false);
    CHECK(true, "memalign 泄漏注册/注销一致");
}

// profile 开关：仅开启 profile 即应记录事件
// 回归：事件记录曾被trace开关门控，只开profile时事件数恒为0
static void test_profile_enable(void)
{
    lmalloc_enable_profile(true);
    lmalloc_enable_trace(false);
    void* p = lmalloc(64);
    assert(p != NULL);
    size_t n = lmalloc_profile_events_count();
    assert(n >= 1);
    lfree(p);
    lmalloc_enable_profile(false);
    CHECK(true, "profile 开关独立生效");
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

// 全 bin 统计遍历：满/未满 slab 并存时遍历不应死循环
// 回归：slabcur/full_slabs 曾被当作线性链表遍历（环链表导致挂死）
static void test_stats_walk_all_bins(void)
{
    // 让 arena 0 的多个 bin 出现使用过的 slab（含多 slab 满链的 bin）
    void* ptrs[40];
    for (int i = 0; i < 40; ++i)
    {
        size_t sz = 8 + (i % 8) * 512; // 覆盖多个大小类
        ptrs[i] = lmalloc(sz);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0x6B, sz);
    }
    for (unsigned b = 0; b < arenas[0]->n_bins; ++b)
    {
        bin_stats_t bs;
        bin_stats_get(&arenas[0]->bins[b], &bs);
    }
    arena_stats_print_all();
    for (int i = 0; i < 40; ++i)
    {
        lfree(ptrs[i]);
    }
    CHECK(true, "全 bin 统计遍历（满/未满 slab 并存）");
}

int main(void)
{
    printf("========== lmalloc check start ==========\n");
    test_basic();
    test_calloc();
    test_realloc();
    test_memalign();
    test_memalign_large();
    test_multi_size();
    test_large();
    test_mid_size();
    test_large_aligned();
    test_size_class_table();
    test_multi_slab();
    test_stress();
    test_tagged();
    test_extent_reuse();
    test_handoff_validate();
    test_leak_memalign();
    test_profile_enable();
    test_stats();
    test_stats_walk_all_bins();
    printf("========== all %d checks passed ==========\n", g_pass);
    return 0;
}
