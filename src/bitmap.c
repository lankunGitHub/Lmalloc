/*
 * bitmap.c - 位图管理实现
 *
 * 主要职责：
 *   - 高效管理内存分配器中的region分配状态
 *   - 支持多级位图（树型/扁平两种实现），适应不同规模
 *   - 提供初始化、分配、回收、大小计算等接口
 *
 * 关键数据结构：
 *   - bitmap_t: 实际位图存储
 *   - bitmap_info_t: 位图元信息，支持多级结构
 *
 * 主要算法：
 *   - 多级位图支持大规模region管理，快速定位空闲位
 *   - 扁平位图适合小规模，节省空间
 *   - 支持按需填充/清空/部分位处理
 *
 * 并发/调优：
 *   - 线程安全由上层保证
 *   - 支持不同填充策略，适应不同分配场景
 *
 * 典型调用链：
 *   slab_new_arena -> bitmap_info_init/bitmap_init
 *   bin_malloc_region/bin_dalloc_region -> bitmap操作
 *
 * 设计trade-off：
 *   - 多级位图提升大规模分配效率，牺牲部分空间
 *   - 扁平位图节省空间，适合小对象
 */
#include <string.h>

#include "bitmap.h"

#ifdef BITMAP_USE_TREE

/**
 * @brief 计算多级位图的总group数
 * @param binfo 位图元信息
 * @return group数量
 *
 * 算法说明：
 *   - 递归计算每级group数量，适合大规模region
 */
static size_t bitmap_info_ngroups(const bitmap_info_t* binfo)
{
    return binfo->levels[binfo->nlevels].offset;
}

/**
 * @brief 初始化多级位图元信息
 * @param binfo 输出元信息
 * @param nbits 位数
 *
 * 算法说明：
 *   - 逐级计算每级group偏移
 *   - 支持多级嵌套，适合大对象
 */
void bitmap_info_init(bitmap_info_t* binfo, size_t nbits)
{
    size_t ngroup = 0;
    int i = 0;

    assert(nbits > 0);
    assert(nbits <= (ZU(1) << LG2_BITMAP_MAXBITS));

    binfo->levels[0].offset = 0;
    ngroup = BITMAP_BITS2GROUPS(nbits);

    for (i = 1; ngroup > 1; i++)
    {
        assert(i <= BITMAP_LEVEL_MAX);
        binfo->levels[i].offset = binfo->levels[i - 1].offset + ngroup;
        ngroup = BITMAP_BITS2GROUPS(ngroup);
    }

    binfo->levels[i].offset = binfo->levels[i - 1].offset + ngroup;
    assert(binfo->levels[i].offset <= BITMAP_GROUPS_MAX);
    binfo->nlevels = i;
    binfo->nbits = nbits;
}

/**
 * @brief 初始化多级位图内容
 * @param bitmap 位图存储
 * @param binfo 元信息
 * @param fill 是否清零
 *
 * 算法说明：
 *   - fill=true时清零，fill=false时全1（空闲）
 *   - 处理多级group的未用位
 */
void bitmap_init(bitmap_t* bitmap, const bitmap_info_t* binfo, bool fill)
{
    if (fill)
    {
        memset(bitmap, 0, bitmap_size(binfo));
        return;
    }

    /*空数组每位初始为1,未使用位用0填充*/
    memset(bitmap, 0xffU, bitmap_size(binfo));

    size_t extra = 0;
    unsigned i = 1;

    /*将未使用的位设置为0*/
    extra = (BITMAP_GROUP_NBITS - (binfo->nbits & BITMAP_GROUP_NBITS_MASK)) &
            BITMAP_GROUP_NBITS_MASK;
    if (extra != 0)
    {
        bitmap[binfo->levels[1].offset - 1] >>= extra;
    }
    for (i = 1; i < binfo->nlevels; i++)
    {
        size_t group_count =
            binfo->levels[i].offset - binfo->levels[i - 1].offset;
        extra = (BITMAP_GROUP_NBITS - (group_count & BITMAP_GROUP_NBITS_MASK)) &
                BITMAP_GROUP_NBITS_MASK;
        if (extra != 0)
        {
            bitmap[binfo->levels[i + 1].offset - 1] >>= extra;
        }
    }
}

#else

/**
 * @brief 初始化扁平位图元信息
 * @param binfo 输出元信息
 * @param nbits 位数
 *
 * 算法说明：
 *   - 只需计算group数量，适合小对象
 */
void bitmap_info_init(bitmap_info_t* binfo, size_t nbits)
{
    assert(nbits > 0);
    assert(nbits <= (ZU(1) << LG2_BITMAP_MAXBITS));

    binfo->ngroups = BITMAP_BITS2GROUPS(nbits);
    binfo->nbits = nbits;
}

/**
 * @brief 计算扁平位图的group数
 * @param binfo 元信息
 * @return group数量
 */
static size_t bitmap_info_ngroups(const bitmap_info_t* binfo)
{
    return binfo->ngroups;
}

/**
 * @brief 初始化扁平位图内容
 * @param bitmap 位图存储
 * @param binfo 元信息
 * @param fill 是否清零
 *
 * 算法说明：
 *   - fill=true时清零，fill=false时全1（空闲）
 *   - 处理最后一个group的未用位
 */
void bitmap_init(bitmap_t* bitmap, const bitmap_info_t* binfo, bool fill)
{
    size_t extra;

    if (fill)
    {
        memset(bitmap, 0, bitmap_size(binfo));
        return;
    }

    memset(bitmap, 0xffU, bitmap_size(binfo));
    extra = (BITMAP_GROUP_NBITS - (binfo->nbits & BITMAP_GROUP_NBITS_MASK)) &
            BITMAP_GROUP_NBITS_MASK;
    if (extra != 0)
    {
        bitmap[binfo->ngroups - 1] >>= extra;
    }
}

#endif

/**
 * @brief 计算位图实际占用字节数
 * @param binfo 元信息
 * @return 字节数
 *
 * 算法说明：
 *   - group数*每group字节数
 */
size_t bitmap_size(const bitmap_info_t* binfo)
{
    return (bitmap_info_ngroups(binfo) << LG2_BITMAP_SIZE);
}