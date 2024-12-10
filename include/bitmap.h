#pragma once

#include "bit_util.h"
#include "sc.h"

typedef unsigned long bitmap_t;

/*设定bitmap的大小，bitmap主要用来统计slab的分配状态和大小类，故其大小不可小于slab最大的region和大小类数量*/
#define BITMAP_SIZE     LONG_SIZE
#define LG2_BITMAP_SIZE LG2_LONG_SIZE

#if SC_LG2_SLAB_MAXREGS > LG_CEIL(SC_NSIZES)
#define LG2_BITMAP_MAXBITS SC_LG2_SLAB_MAXREGS
#else
#define LG2_BITMAP_MAXBITS LG_CEIL(SC_NSIZE)
#endif

#define BITMAP_MAXBITS (ZU(1) << LG2_BITMAP_MAXBITS)

/*对于储存多个位，我们需要一个bitmap组，如果需要储存的位比较大，此时我们通过树结构管理bitmap组*/

#define LG2_BITMAP_GROUP_NBITS  (LG2_BITMAP_SIZE + 3)
#define BITMAP_GROUP_NBITS      (1U << LG2_BITMAP_GROUP_NBITS)
#define BITMAP_GROUP_NBITS_MASK (BITMAP_GROUP_NBITS - 1)

/*下面具体介绍如何树形结构管理bitmap，将bitmap分成多个等级，高一级的bitmap管理低一级的bitmap
 * 从而形成了层级结构，第一层是最低级的bitmap，直接用来储存，随后是第二层，用来管理第一层的bitmap
 * 现在再使用一个level数组记录每个层级的偏移量，对于nbits,第一层分配 nbits /
 * BITMAP_GROUP_NBITS (向上取整)个bitmap,
 * 即是level1的偏移量，现在对于第二层level的偏移量level2 = level1 + level1 /
 * BITMAP_GROUP_NBITS)个bitmap,
 * 即是level2的偏移量，此时对于level1和level2之间，即使第二层的bitmap 以此类推
 * 对于这样的管理结构，在我们对于bitmap的一些操作，如查找改变首个有效位等都有很好的优化
 * 当管理的位比较少时，使用这样的方法并不是好的选择，会导致更多的内存占用以及操作消耗
 * 但是在管理巨大的bitmap时将会有很好的性能提升
 */
#if LG2_BITMAP_MAXBITS - LG2_BITMAP_SIZE > 3
#define BITMAP_USE_TREE
#endif

/*对于给定的存储位数，需要多少个bitmap (+
 * BITMAP_GROUP_NBITS_MASK是为了保证向上取整)*/
#define BITMAP_BITS2GROUPS(nbits)                                              \
    (((nbits) + BITMAP_GROUP_NBITS_MASK) >> LG2_BITMAP_GROUP_NBITS)

/*
 * 对于多级的bitmap组,可以理解为用bitmap(level n)组来记录bitmap(level
 * n-1)组
 */
#define BITMAP_GROUPS_L0(nbits) BITMAP_BITS2GROUPS(nbits)
#define BITMAP_GROUPS_L1(nbits) BITMAP_BITS2GROUPS(BITMAP_BITS2GROUPS(nbits))
#define BITMAP_GROUPS_L2(nbits)                                                \
    BITMAP_BITS2GROUPS(BITMAP_BITS2GROUPS(BITMAP_BITS2GROUPS((nbits))))
#define BITMAP_GROUPS_L3(nbits)                                                \
    BITMAP_BITS2GROUPS(                                                        \
        BITMAP_BITS2GROUPS(BITMAP_BITS2GROUPS(BITMAP_BITS2GROUPS((nbits)))))
#define BITMAP_GROUPS_L4(nbits)                                                \
    BITMAP_BITS2GROUPS(BITMAP_BITS2GROUPS(                                     \
        BITMAP_BITS2GROUPS(BITMAP_BITS2GROUPS(BITMAP_BITS2GROUPS((nbits))))))

#define BITMAP_GROUPS_1_LEVEL(nbits) BITMAP_GROUPS_L0(nbits)
#define BITMAP_GROUPS_2_LEVEL(nbits)                                           \
    (BITMAP_GROUPS_1_LEVEL(nbits) + BITMAP_GROUPS_L1(nbits))
#define BITMAP_GROUPS_3_LEVEL(nbits)                                           \
    (BITMAP_GROUPS_2_LEVEL(nbits) + BITMAP_GROUPS_L2(nbits))
#define BITMAP_GROUPS_4_LEVEL(nbits)                                           \
    (BITMAP_GROUPS_3_LEVEL(nbits) + BITMAP_GROUPS_L3(nbits))
#define BITMAP_GROUPS_5_LEVEL(nbits)                                           \
    (BITMAP_GROUPS_4_LEVEL(nbits) + BITMAP_GROUPS_L4(nbits))

#ifdef BITMAP_USE_TREE

/*根据位图大小确定bitmap组的级数，决定我们使用几级bitmap组作为基本组*/
#if LG2_BITMAP_MAXBITS <= LG2_BITMAP_GROUP_NBITS
#define BITMAP_GROUPS(nbits) BITMAP_GROUPS_1_LEVEL(nbits)
#define BITMAP_GROUPS_MAX    BITMAP_GROUPS_1_LEVEL(BITMAP_MAXBITS)
#elif LG2_BITMAP_MAXBITS <= LG2_BITMAP_GROUP_NBITS * 2
#define BITMAP_GROUPS(nbits) BITMAP_GROUPS_2_LEVEL(nbits)
#define BITMAP_GROUPS_MAX    BITMAP_GROUPS_2_LEVEL(BITMAP_MAXBITS)
#elif LG2_BITMAP_MAXBITS <= LG2_BITMAP_GROUP_NBITS * 3
#define BITMAP_GROUPS(nbits) BITMAP_GROUPS_3_LEVEL(nbits)
#define BITMAP_GROUPS_MAX    BITMAP_GROUPS_3_LEVEL(BITMAP_MAXBITS)
#elif LG2_BITMAP_MAXBITS <= LG2_BITMAP_GROUP_NBITS * 4
#define BITMAP_GROUPS(nbits) BITMAP_GROUPS_4_LEVEL(nbits)
#define BITMAP_GROUPS_MAX    BITMAP_GROUPS_4_LEVEL(BITMAP_MAXBITS)
#elif LG2_BITMAP_MAXBITS <= LG2_BITMAP_GROUP_NBITS * 5
#define BITMAP_GROUPS(nbits) BITMAP_GROUPS_5_LEVEL(nbits)
#define BITMAP_GROUPS_MAX    BITMAP_GROUPS_5_LEVEL(BITMAP_MAXBITS)
#else
#error "Unsupported bitmap size"
#endif

#define BITMAP_LEVEL_MAX 5

#define BITMAP_INFO_INITIALIZER(nbits)                                         \
    {                                                                          \
        /* 存储的位数 */                                                       \
        nbits;                                                                 \
        /* 需要的级数，如果前一级和后一级需要的数量相同则结束 */               \
        (BITMAP_GROUPS_L0(nbits) > BITMAP_GROUPS_L1(nbits)) +                  \
            (BITMAP_GROUPS_L1(nbits) > BITMAP_GROUPS_L2(nbits)) +              \
            (BITMAP_GROUPS_L2(nbits) > BITMAP_GROUPS_L3(nbits)) +              \
            (BITMAP_GROUPS_L3(nbits) > BITMAP_GROUPS_L4(nbits)) + 1;           \
        /* 不同 */                                                             \
        {                                                                      \
            {0}, {BITMAP_GROUPS_L0(nbits)},                                    \
                {BITMAP_GROUPS_L1(nbits) + BITMAP_GROUPS_L0(nbits)},           \
                {BITMAP_GROUPS_L2(nbits) + BITMAP_GROUPS_L1(nbits) +           \
                 BITMAP_GROUPS_L0(nbits)},                                     \
                {BITMAP_GROUPS_L3(nbits) + BITMAP_GROUPS_L2(nbits) +           \
                 BITMAP_GROUPS_L1(nbits) + BITMAP_GROUPS_L0(nbits)},           \
            {                                                                  \
                BITMAP_GROUPS_L4(nbits) + BITMAP_GROUPS_L3(nbits) +            \
                    BITMAP_GROUPS_L2(nbits) + BITMAP_GROUPS_L1(nbits) +        \
                    BITMAP_GROUPS_L0(nbits)                                    \
            }                                                                  \
        }                                                                      \
    }

#else
#define BITMAP_GROUPS(nbits) BITMAP_BITS2GROUPS(nbits)
#define BITMAP_GROUPS_MAX    BITMAP_BITS2GROUPS(BITMAP_MAXBITS)

#define BITMAP_INFO_INITIALIZER(nbits)                                         \
    {       /* nbits. */                                                       \
     nbits, /* ngroups. */                                                     \
     BITMAP_BITS2GROUPS(nbits)}

#endif

typedef struct bitmap_level_s bitmap_level_t;
struct bitmap_level_s
{
    /*每层的偏移量,即某个nbit在不同层数需要的bitmap数量*/
    size_t offset;
};

typedef struct bitmap_info_s bitmap_info_t;
struct bitmap_info_s
{
    size_t nbits;

#ifdef BITMAP_USE_TREE

    unsigned nlevels;
    bitmap_level_t levels[BITMAP_GROUPS_MAX + 1];

#else
    size_t ngroups;
#endif
};

/*获得该bitmap(准确来说是这个bitmap组)可储存的大小*/
size_t bitmap_size(const bitmap_info_t* binfo);
void bitmap_info_init(bitmap_info_t* binfo, size_t nbits);
void bitmap_init(bitmap_t* bitmap, const bitmap_info_t* binfo, bool fill);

static inline bool bitmap_full(bitmap_t* bitmap, const bitmap_info_t* binfo)
{
#ifdef BITMAP_USE_TREE
    size_t rgoff = binfo->levels[binfo->nlevels].offset - 1;
    bitmap_t rg = bitmap[rgoff];
    return (rg == 0);
#else
    size_t i;

    for (i = 0; i < binfo->ngroups; i++)
    {
        if (bitmap[i] != 0)
        {
            return false;
        }
    }
    return true;
#endif
}

static inline bool
bitmap_get(bitmap_t* bitmap, const bitmap_info_t* binfo, size_t bit)
{
    size_t goff;
    bitmap_t g;

    assert(bit < binfo->nbits);
    goff = bit >> LG2_BITMAP_GROUP_NBITS;
    g = bitmap[goff];
    return !(g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK)));
}

static inline void
bitmap_set(bitmap_t* bitmap, const bitmap_info_t* binfo, size_t bit)
{
    size_t goff;
    bitmap_t* gp;
    bitmap_t g;

    assert(bit < binfo->nbits);
    assert(!bitmap_get(bitmap, binfo, bit));
    goff = bit >> LG2_BITMAP_GROUP_NBITS;
    gp = &bitmap[goff];
    g = *gp;
    assert(g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK)));
    g ^= ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK);
    *gp = g;
    assert(bitmap_get(bitmap, binfo, bit));
#ifdef BITMAP_USE_TREE
    if (g == 0)
    {
        unsigned i;
        for (i = 1; i < binfo->nlevels; i++)
        {
            bit = goff;
            goff = bit >> LG2_BITMAP_GROUP_NBITS;
            gp = &bitmap[binfo->levels[i].offset + goff];
            g = *gp;
            assert(g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK)));
            g ^= ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK);
            *gp = g;
            if (g != 0)
            {
                break;
            }
        }
    }
#endif
}

/* ffu: find first unset >= bit. */
static inline size_t
bitmap_ffu(const bitmap_t* bitmap, const bitmap_info_t* binfo, size_t min_bit)
{
    assert(min_bit < binfo->nbits);

#ifdef BITMAP_USE_TREE
    size_t bit = 0;
    for (unsigned level = binfo->nlevels; level--;)
    {
        size_t lg_bits_per_group = (LG2_BITMAP_GROUP_NBITS * (level + 1));
        bitmap_t group =
            bitmap[binfo->levels[level].offset + (bit >> lg_bits_per_group)];
        unsigned group_nmask =
            (unsigned)(((min_bit > bit) ? (min_bit - bit) : 0) >>
                       (lg_bits_per_group - LG2_BITMAP_GROUP_NBITS));
        assert(group_nmask <= BITMAP_GROUP_NBITS);
        bitmap_t group_mask = ~((1LU << group_nmask) - 1);
        bitmap_t group_masked = group & group_mask;
        if (group_masked == 0LU)
        {
            if (group == 0LU)
            {
                return binfo->nbits;
            }

            size_t sib_base = bit + (ZU(1) << lg_bits_per_group);
            assert(sib_base > min_bit);
            assert(sib_base > bit);
            if (sib_base >= binfo->nbits)
            {
                return binfo->nbits;
            }
            return bitmap_ffu(bitmap, binfo, sib_base);
        }
        bit += ((size_t)ffs_lu(group_masked))
               << (lg_bits_per_group - LG2_BITMAP_GROUP_NBITS);
    }
    assert(bit >= min_bit);
    assert(bit < binfo->nbits);
    return bit;
#else
    size_t i = min_bit >> LG2_BITMAP_GROUP_NBITS;
    bitmap_t g =
        bitmap[i] & ~((1LU << (min_bit & BITMAP_GROUP_NBITS_MASK)) - 1);
    size_t bit;
    while (1)
    {
        if (g != 0)
        {
            bit = ffs_lu(g);
            return (i << LG2_BITMAP_GROUP_NBITS) + bit;
        }
        i++;
        if (i >= binfo->ngroups)
        {
            break;
        }
        g = bitmap[i];
    }
    return binfo->nbits;
#endif
}

/* sfu: set first unset. */
static inline size_t bitmap_sfu(bitmap_t* bitmap, const bitmap_info_t* binfo)
{
    size_t bit;
    bitmap_t g;
    unsigned i;

    assert(!bitmap_full(bitmap, binfo));

#ifdef BITMAP_USE_TREE
    i = binfo->nlevels - 1;
    g = bitmap[binfo->levels[i].offset];
    bit = ffs_lu(g);
    while (i > 0)
    {
        i--;
        g = bitmap[binfo->levels[i].offset + bit];
        bit = (bit << LG2_BITMAP_GROUP_NBITS) + ffs_lu(g);
    }
#else
    i = 0;
    g = bitmap[0];
    while (g == 0)
    {
        i++;
        g = bitmap[i];
    }
    bit = (i << LG2_BITMAP_GROUP_NBITS) + ffs_lu(g);
#endif
    bitmap_set(bitmap, binfo, bit);
    return bit;
}

static inline void
bitmap_unset(bitmap_t* bitmap, const bitmap_info_t* binfo, size_t bit)
{
    size_t goff;
    bitmap_t* gp;
    bitmap_t g;
    bool propagate;

    assert(bit < binfo->nbits);
    assert(bitmap_get(bitmap, binfo, bit));
    goff = bit >> LG2_BITMAP_GROUP_NBITS;
    gp = &bitmap[goff];
    g = *gp;
    propagate = (g == 0);
    assert((g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK))) == 0);
    g ^= ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK);
    *gp = g;
    assert(!bitmap_get(bitmap, binfo, bit));
#ifdef BITMAP_USE_TREE
    if (propagate)
    {
        unsigned i;
        for (i = 1; i < binfo->nlevels; i++)
        {
            bit = goff;
            goff = bit >> LG2_BITMAP_GROUP_NBITS;
            gp = &bitmap[binfo->levels[i].offset + goff];
            g = *gp;
            propagate = (g == 0);
            assert((g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK))) == 0);
            g ^= ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK);
            *gp = g;
            if (!propagate)
            {
                break;
            }
        }
    }
#endif
}