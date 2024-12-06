#pragma once
#include <stddef.h>
#include <stdbool.h>

#include "ts.h"

// 必须先定义所有依赖的常量
#ifndef POINTER_SIZE
#define POINTER_SIZE 8      // 64位
#endif
#define SC_LG2_TINY_MIN 3   // 必须 <= LG2_QUANTUM
#ifndef LG2_PAGE
#define LG2_PAGE 12         // 4KB页
#endif

// 检查SC_LG2_TINY_MIN合法性
#if SC_LG2_TINY_MIN > LG2_QUANTUM
#error "SC_LG2_TINY_MIN must be less than or equal to LG2_QUANTUM"
#endif

// 这是jemalloc中对于大小类的定义
/*
 * Size class computations:
 *
 * These are a little tricky; we'll first start by describing how things
 * generally work, and then describe some of the details.
 *
 * Ignore the first few size classes for a moment. We can then split all the
 * remaining size classes into groups. The size classes in a group are spaced
 * such that they cover allocation request sizes in a power-of-2 range. The
 * power of two is called the base of the group, and the size classes in it
 * satisfy allocations in the half-open range (base, base * 2]. There are
 * SC_NGROUP size classes in each group, equally spaced in the range, so that
 * each one covers allocations for base / SC_NGROUP possible allocation sizes.
 * We call that value (base / SC_NGROUP) the delta of the group. Each size class
 * is delta larger than the one before it (including the initial size class in a
 * group, which is delta larger than base, the largest size class in the
 * previous group).
 * To make the math all work out nicely, we require that SC_NGROUP is a power of
 * two, and define it in terms of SC_LG_NGROUP. We'll often talk in terms of
 * lg_base and lg_delta. For each of these groups then, we have that
 * lg_delta == lg_base - SC_LG_NGROUP.
 * The size classes in a group with a given lg_base and lg_delta (which, recall,
 * can be computed from lg_base for these groups) are therefore:
 *   base + 1 * delta
 *     which covers allocations in (base, base + 1 * delta]
 *   base + 2 * delta
 *     which covers allocations in (base + 1 * delta, base + 2 * delta].
 *   base + 3 * delta
 *     which covers allocations in (base + 2 * delta, base + 3 * delta].
 *   ...
 *   base + SC_NGROUP * delta ( == 2 * base)
 *     which covers allocations in (base + (SC_NGROUP - 1) * delta, 2 * base].
 * (Note that currently SC_NGROUP is always 4, so the "..." is empty in
 * practice.)
 * Note that the last size class in the group is the next power of two (after
 * base), so that we've set up the induction correctly for the next group's
 * selection of delta.
 *
 * Now, let's start considering the first few size classes. Two extra constants
 * come into play here: LG_QUANTUM and SC_LG_TINY_MIN. LG_QUANTUM ensures
 * correct platform alignment; all objects of size (1 << LG_QUANTUM) or larger
 * are at least (1 << LG_QUANTUM) aligned; this can be used to ensure that we
 * never return improperly aligned memory, by making (1 << LG_QUANTUM) equal the
 * highest required alignment of a platform. For allocation sizes smaller than
 * (1 << LG_QUANTUM) though, we can be more relaxed (since we don't support
 * platforms with types with alignment larger than their size). To allow such
 * allocations (without wasting space unnecessarily), we introduce tiny size
 * classes; one per power of two, up until we hit the quantum size. There are
 * therefore LG_QUANTUM - SC_LG_TINY_MIN such size classes.
 *
 * Next, we have a size class of size (1 << LG_QUANTUM).  This can't be the
 * start of a group in the sense we described above (covering a power of two
 * range) since, if we divided into it to pick a value of delta, we'd get a
 * delta smaller than (1 << LG_QUANTUM) for sizes >= (1 << LG_QUANTUM), which
 * is against the rules.
 *
 * The first base we can divide by SC_NGROUP while still being at least
 * (1 << LG_QUANTUM) is SC_NGROUP * (1 << LG_QUANTUM). We can get there by
 * having SC_NGROUP size classes, spaced (1 << LG_QUANTUM) apart. These size
 * classes are:
 *   1 * (1 << LG_QUANTUM)
 *   2 * (1 << LG_QUANTUM)
 *   3 * (1 << LG_QUANTUM)
 *   ... (although, as above, this "..." is empty in practice)
 *   SC_NGROUP * (1 << LG_QUANTUM).
 *
 * There are SC_NGROUP of these size classes, so we can regard it as a sort of
 * pseudo-group, even though it spans multiple powers of 2, is divided
 * differently, and both starts and ends on a power of 2 (as opposed to just
 * ending). SC_NGROUP is itself a power of two, so the first group after the
 * pseudo-group has the power-of-two base SC_NGROUP * (1 << LG_QUANTUM), for a
 * lg_base of LG_QUANTUM + SC_LG_NGROUP. We can divide this base into SC_NGROUP
 * sizes without violating our LG_QUANTUM requirements, so we can safely set
 * lg_delta = lg_base - SC_LG_GROUP (== LG_QUANTUM).
 *
 * So, in order, the size classes are:
 *
 * Tiny size classes:
 * - Count: LG_QUANTUM - SC_LG_TINY_MIN.
 * - Sizes:
 *     1 << SC_LG_TINY_MIN
 *     1 << (SC_LG_TINY_MIN + 1)
 *     1 << (SC_LG_TINY_MIN + 2)
 *     ...
 *     1 << (LG_QUANTUM - 1)
 *
 * Initial pseudo-group:
 * - Count: SC_NGROUP
 * - Sizes:
 *     1 * (1 << LG_QUANTUM)
 *     2 * (1 << LG_QUANTUM)
 *     3 * (1 << LG_QUANTUM)
 *     ...
 *     SC_NGROUP * (1 << LG_QUANTUM)
 *
 * Regular group 0:
 * - Count: SC_NGROUP
 * - Sizes:
 *   (relative to lg_base of LG_QUANTUM + SC_LG_NGROUP and lg_delta of
 *   lg_base - SC_LG_NGROUP)
 *     (1 << lg_base) + 1 * (1 << lg_delta)
 *     (1 << lg_base) + 2 * (1 << lg_delta)
 *     (1 << lg_base) + 3 * (1 << lg_delta)
 *     ...
 *     (1 << lg_base) + SC_NGROUP * (1 << lg_delta) [ == (1 << (lg_base + 1)) ]
 *
 * Regular group 1:
 * - Count: SC_NGROUP
 * - Sizes:
 *   (relative to lg_base of LG_QUANTUM + SC_LG_NGROUP + 1 and lg_delta of
 *   lg_base - SC_LG_NGROUP)
 *     (1 << lg_base) + 1 * (1 << lg_delta)
 *     (1 << lg_base) + 2 * (1 << lg_delta)
 *     (1 << lg_base) + 3 * (1 << lg_delta)
 *     ...
 *     (1 << lg_base) + SC_NGROUP * (1 << lg_delta) [ == (1 << (lg_base + 1)) ]
 *
 * ...
 *
 * Regular group N:
 * - Count: SC_NGROUP
 * - Sizes:
 *   (relative to lg_base of LG_QUANTUM + SC_LG_NGROUP + N and lg_delta of
 *   lg_base - SC_LG_NGROUP)
 *     (1 << lg_base) + 1 * (1 << lg_delta)
 *     (1 << lg_base) + 2 * (1 << lg_delta)
 *     (1 << lg_base) + 3 * (1 << lg_delta)
 *     ...
 *     (1 << lg_base) + SC_NGROUP * (1 << lg_delta) [ == (1 << (lg_base + 1)) ]
 *
 *
 * Representation of metadata:
 * To make the math easy, we'll mostly work in lg quantities. We record lg_base,
 * lg_delta, and ndelta (i.e. number of deltas above the base) on a
 * per-size-class basis, and maintain the invariant that, across all size
 * classes, size == (1 << lg_base) + ndelta * (1 << lg_delta).
 *
 * For regular groups (i.e. those with lg_base >= LG_QUANTUM + SC_LG_NGROUP),
 * lg_delta is lg_base - SC_LG_NGROUP, and ndelta goes from 1 to SC_NGROUP.
 *
 * For the initial tiny size classes (if any), lg_base is lg(size class size).
 * lg_delta is lg_base for the first size class, and lg_base - 1 for all
 * subsequent ones. ndelta is always 0.
 *
 * For the pseudo-group, if there are no tiny size classes, then we set
 * lg_base == LG_QUANTUM, lg_delta == LG_QUANTUM, and have ndelta range from 0
 * to SC_NGROUP - 1. (Note that delta == base, so base + (SC_NGROUP - 1) * delta
 * is just SC_NGROUP * base, or (1 << (SC_LG_NGROUP + LG_QUANTUM)), so we do
 * indeed get a power of two that way). If there *are* tiny size classes, then
 * the first size class needs to have lg_delta relative to the largest tiny size
 * class. We therefore set lg_base == LG_QUANTUM - 1,
 * lg_delta == LG_QUANTUM - 1, and ndelta == 1, keeping the rest of the
 * pseudo-group the same.
 *
 *
 * Other terminology:
 * "Small" size classes mean those that are allocated out of bins, which is the
 * same as those that are slab allocated.
 * "Large" size classes are those that are not small. The cutoff for counting as
 * large is page size * group size.
 */

#define SC_LG2_NGROUP   2 // 每组拥有的大小类数量的log2
#define SC_LG2_TINY_MIN 3 // 最小的大小类的log2

#if SC_LG2_TINY_MIN > LG2_QUANTUM
#error "SC_LG2_TINY_MIN must be less than or equal to LG2_QUANTUM"
#endif

#define SC_NGROUP       (1ULL << SC_LG2_NGROUP)         // 每组拥有的大小类数量
#define SC_NPSEUDO      SC_NGROUP                       // 伪组的数量
#define SC_NTINY        (LG2_QUANTUM - SC_LG2_TINY_MIN) // 最小大小类组有几个大小类
#define SC_LG2_TINY_MAX (LG2_QUANTUM - 1) // 最小大小类组中最大的大小类
#define SC_LG2_FIRST_REGULAR_GROUP                                             \
    (LG2_QUANTUM + SC_LG2_NGROUP) // 第一个常规大小类组开始的大小类大小的log2

#define SC_LG2_MAX_SC                                                          \
    (POINTER_SIZE * 8 -                                                        \
     1) // 最大大小类的log2(不包含，因为实际最大分配要小于这个值)
#define SC_LG2_MAX_GROUP                                                       \
    (SC_LG2_MAX_SC - 1) // 最大大小类组开始的大小类大小的log2

#define SC_NREGULAR                                                            \
    (SC_NGROUP * (SC_LG2_MAX_GROUP - SC_LG2_FIRST_REGULAR_GROUP + 1) -         \
     1) // 常规大小类组的个数

/*所有大小类数量*/
#define SC_NSIZES (SC_NTINY + SC_NPSEUDO + SC_NREGULAR)

// 将大小类按页面大小分配的数量
#define SC_NPSIZES                                                             \
    (SC_NGROUP + (SC_LG2_MAX_GROUP - (LG2_PAGE + SC_LG2_NGROUP)) * SC_NGROUP + \
     SC_NGROUP - 1)

// 对于 size < page size * group 的，我们会使用bin(size会映射为某个bin
// index,如果该index小于SC_NBINS即可使用bin)
#define SC_NBINS                                                               \
    (SC_NTINY + SC_NPSEUDO +                                                   \
     SC_NGROUP * (LG2_PAGE + SC_LG2_NGROUP - SC_LG2_FIRST_REGULAR_GROUP) - 1)

/*根据page index查找对应的size class*/
extern size_t sz_pind2sz_tab[SC_NPSIZES + 1];
/*根据index来查找对应的size class*/
extern size_t sz_index2size_tab[SC_NSIZES];
/*
 *size2index_tab查找表使用uint8_t来编码每个bin索引，所以我们
 *不能支持超过256个小型类。
 */
#if (SC_NBINS > 256)
#error "Too many small size classes"
#endif

/* 查找表中最大的尺寸类及其二进制日志。 */
#define SC_LG2_MAX_LOOKUP  12
#define SC_LOOKUP_MAXCLASS (1 << SC_LG2_MAX_LOOKUP)

/*对于小内存，即可以使用bin管理的内存最大的组和增量*/
#define SC_SMALL_MAX_GROUP (1 << (LG2_PAGE + SC_LG2_NGROUP - 1))
#define SC_SMALL_MAX_DELTA (1 << (LG2_PAGE - 1))

/*可以通过slab分配的最大的大小类*/
#define SC_SMALL_MAXCLASS                                                      \
    (SC_SMALL_MAX_GROUP + (SC_NGROUP - 1) * SC_SMALL_MAX_DELTA)

/*如果可分配最大内存大于了查找表可查找范围，则会出错*/
#if (SC_SMALL_MAXCLASS < SC_LOOKUP_MAXCLASS)
#error "Lookup table sizes must be small"
#endif

/*不通过slab分配的最小大小*/
#define SC_LARGE_MINCLASS     ((size_t)1ULL << (LG2_PAGE + SC_LG2_NGROUP))
#define SC_LG2_LARGE_MINCLASS (LG2_PAGE + SC_LG2_NGROUP)

/*为了定义大的内存所使用*/
#define SC_MAX_BASE ((size_t)1 << (POINTER_SIZE * 8 - 2))
#define SC_MAX_DELTA                                                           \
    ((size_t)1 << (POINTER_SIZE * 8 - 2 -                                      \
                   SC_LG2_NGROUP)) // lg(delta) = lg(base) - lg(ngroup)

/*支持的最大分配*/
#define SC_LARGE_MAXCLASS (SC_MAX_BASE + (SC_NGROUP - 1) * SC_MAX_DELTA)

/*一个slab所允许拥有的最大region数量(一个slab由多个region组成)*/
#define SC_LG2_SLAB_MAXREGS (LG2_PAGE - SC_LG2_TINY_MIN)
#define SC_SLAB_MAXREGS     (1U << SC_LG2_SLAB_MAXREGS)

// jemalloc风格size class条目
typedef struct sc_s {
    unsigned long index;      // size class索引
    unsigned lg2_base;        // 组的base
    unsigned lg2_delta;       // 组的增量
    unsigned ndelta;          // 相对base增加了几个delta
    bool psz;                 // 是否page大小的整数倍
    bool slab;                // 是否为slab分配
    int pgs;                  // slab分配时的page数
    int lg2_delta_lookup;     // 查找表用
    size_t size;              // 实际大小
    size_t align;             // 对齐要求
} sc_t;

// jemalloc风格size class全局分布及元数据
typedef struct sc_data_s {
    int lg2_quantum;
    unsigned ntiny;
    int nlbins;
    int nbins;
    int nsizes;
    unsigned npsizes;
    int lg2_tiny_maxclass;
    int lg2_tiny_minclass;
    size_t lookup_maxclass;
    size_t small_maxclass;
    size_t large_minclass;
    size_t large_maxclass;
    bool initialized;
    sc_t entries[SC_NSIZES]; // 直接内嵌静态数组
} sc_data_t;

// 全局唯一静态分布（头文件用extern，源文件定义）
extern sc_data_t g_sc_data;

// 初始化静态分布和查找表
void sc_data_init(sc_data_t* data);
// 查找size对应的index
size_t sc_data_find(const sc_data_t* data, size_t size);
// 查找index对应的size
size_t sc_data_index2size(const sc_data_t* data, size_t index);
// 对齐
size_t sc_data_align(const sc_data_t* data, size_t size);