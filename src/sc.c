// src/sc.c
// 内存分配器中的大小类 (Size Class) 管理模块
// 负责管理内存分配器中不同大小的内存块，通过大小类进行分类和分配。
// 大小类是内存分配器中的基本单位，每个大小类对应一个固定的内存块大小。
// 通过预先定义的大小类，可以快速找到适合的内存块，避免频繁的内存分配和释放。
// 大小类的设计需要考虑内存碎片、内存对齐、内存浪费等因素。
// 本模块实现了大小类的初始化、查找和分配等功能。


#include "sc.h"
#include "../include/sc.h"
#include "../include/ts.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t g_sc_size2bin[4096]; // 支持最大 4096 字节的 size class
sc_data_t g_sc_data;
size_t g_sc_bin2size[SC_NBINS];
// 运行时实际bin数量（slab可容纳的大小类个数），启动时由sc_data_init填充
size_t sz_nbins = 0;

// 计算大小类对应的内存块大小
// 参数:
//   lg2_base: 大小类的基准对数
//   lg2_delta: 大小类的增量对数
//   ndelta: 大小类的增量值
// 返回值:
//   内存块大小 (以字节为单位)
size_t size_compute(int lg2_base, int lg2_delta, int ndelta)
{
    return (ZU(1) << lg2_base) + (ZU(ndelta) << lg2_delta);
}

// 找到第一个分配大小满足同时是page和该sc的整倍数，返回page数
// 参数:
//   lg_page: 页面对数
//   lg_base: 大小类的基准对数
//   lg_delta: 大小类的增量对数
//   ndelta: 大小类的增量值
// 返回值:
//   满足条件的page数
static int slab_size(int lg_page, int lg_base, int lg_delta, int ndelta)
{
    size_t page = (ZU(1) << lg_page);
    size_t reg_size = size_compute(lg_base, lg_delta, ndelta);

    size_t try_slab_size = page;
    size_t try_nregs = try_slab_size / reg_size;
    size_t perfect_slab_size = 0;
    bool perfect = false;

    while (!perfect)
    {
        perfect_slab_size = try_slab_size;
        size_t perfect_nregs = try_nregs;
        try_slab_size += page;
        try_nregs = try_slab_size / reg_size;
        if (perfect_slab_size == perfect_nregs * reg_size)
        {
            perfect = true;
        }
    }
    return (int)(perfect_slab_size / page);
}

// 初始化大小类数据
// 参数:
//   data: 大小类数据结构体指针
// 功能:
//   1. 遍历所有可能的大小类，确定每个大小类的属性 (slab, pgs, lg2_delta_lookup)
//   2. 将大小类按lg2_base分组，并标记伪组和常规组
//   3. 计算每个大小类的lg2_delta_lookup值
//   4. 更新nlbins计数
static void size_classes(sc_data_t* data)
{
    int nlbins = 0;
    int index = 0;

    int lg2_quantum = data->lg2_quantum;
    int max_lookup = data->lookup_maxclass;
    int ntiny = data->ntiny;

    int ndelta = 0;
    int lg2_base = data->lg2_tiny_minclass;
    int lg2_delta = lg2_base;

    size_t size = 0;

    /*tiny 组*/
    while (lg2_base < lg2_quantum)
    {
        sc_t* sc = &(data->entries[index]);
        sc->index = index++;
        sc->lg2_base = lg2_base;
        sc->lg2_delta = lg2_delta;
        sc->ndelta = ndelta;
        size = size_compute(lg2_base, lg2_delta, ndelta);
        sc->size = size;
        sc->align = (ZU(1) << lg2_base);
        sc->psz = (size % (ZU(1) << LG2_PAGE) == 0);
        if (size < (ZU(1) << (LG2_PAGE + SC_LG2_NGROUP)))
        {
            sc->slab = true;
            sc->pgs = slab_size(LG2_PAGE, lg2_base, lg2_delta, ndelta);
        }
        else
        {
            sc->slab = false;
            sc->pgs = 0;
        }
        if (size <= (ZU(max_lookup)))
        {
            sc->lg2_delta_lookup = lg2_delta;
        }
        else
        {
            sc->lg2_delta_lookup = 0;
        }
        // bin数量按slab可容纳的大小类统计（而非lookup表范围）
        // 注意：sc->index = index++ 后index已自增，nlbins取index即为已生成条目数
        if (sc->slab)
            nlbins = index;
        lg2_delta = lg2_base++;
    }

    assert(lg2_quantum == lg2_base);

    /*伪组*/
    if (ntiny != 0)
    {
        sc_t* sc = &(data->entries[index]);
        sc->index = index++;
        sc->lg2_base = --lg2_base;
        sc->lg2_delta = lg2_delta;
        sc->ndelta = ++ndelta;
        size = size_compute(lg2_base, lg2_delta, ndelta);
        sc->size = size;
        sc->align = (ZU(1) << lg2_base);
        if (size < (ZU(1) << (LG2_PAGE + SC_LG2_NGROUP)))
        {
            sc->slab = true;
            sc->pgs = slab_size(LG2_PAGE, lg2_base, lg2_delta, ndelta);
        }
        else
        {
            sc->slab = false;
            sc->pgs = 0;
        }
        if (size <= (ZU(max_lookup)))
        {
            sc->lg2_delta_lookup = lg2_delta;
        }
        else
        {
            sc->lg2_delta_lookup = 0;
        }
        ++lg2_base;
        ++lg2_delta;
    }
    while (ndelta < (int)SC_NGROUP)
    {
        sc_t* sc = &(data->entries[index]);
        sc->index = index++;
        sc->lg2_base = lg2_base;
        // lg2_delta必须赋值：曾缺失导致sc_ext按组参数重算size时
        // 得到17/18/19等错误大小类（真实为32/48/64）
        sc->lg2_delta = lg2_delta;
        sc->ndelta = ndelta;
        size = size_compute(lg2_base, lg2_delta, ndelta);
        ++ndelta;
        sc->size = size;
        sc->align = (ZU(1) << lg2_base);
        sc->psz = (size % (ZU(1) << LG2_PAGE) == 0);
        if (size < (ZU(1) << (LG2_PAGE + SC_LG2_NGROUP)))
        {
            sc->slab = true;
            sc->pgs = slab_size(LG2_PAGE, lg2_base, lg2_delta, ndelta);
        }
        else
        {
            sc->slab = false;
            sc->pgs = 0;
        }
        if (size <= (ZU(max_lookup)))
        {
            sc->lg2_delta_lookup = lg2_delta;
        }
        else
        {
            sc->lg2_delta_lookup = 0;
        }
    }

    /*常规组*/
    lg2_base = lg2_base + SC_NGROUP;
    int ptr_bits = POINTER_SIZE * 8;
    while (lg2_base < ptr_bits - 1)
    {
        ndelta = 1;
        int ndelta_limit;
        if (lg2_base == ptr_bits - 2)
        {
            ndelta_limit = SC_NGROUP - 1;
        }
        else
        {
            ndelta_limit = SC_NGROUP;
        }
        while (ndelta <= ndelta_limit)
        {
            sc_t* sc = &(data->entries[index]);
            sc->index = index++;
            sc->lg2_base = lg2_base;
            sc->lg2_delta = lg2_delta;
            sc->ndelta = ndelta;
            size = size_compute(lg2_base, lg2_delta, ndelta);
            ++ndelta;
            sc->size = size;
            sc->align = (ZU(1) << lg2_base);
            if (size < (ZU(1) << (LG2_PAGE + SC_LG2_NGROUP)))
            {
                sc->slab = true;
                sc->pgs = slab_size(LG2_PAGE, lg2_base, lg2_delta, ndelta);
            }
            else
            {
                sc->slab = false;
                sc->pgs = 0;
            }
            if (size <= (ZU(max_lookup)))
            {
                sc->lg2_delta_lookup = lg2_delta;
            }
            else
            {
                sc->lg2_delta_lookup = 0;
            }
            // bin数量按slab可容纳的大小类统计（而非lookup表范围）
            // 注意：sc->index = index++ 后index已自增，nlbins取index即为已生成条目数
            if (sc->slab)
                nlbins = index;
        }
        ++lg2_base;
        ++lg2_delta;
    }

    data->nlbins = nlbins;
}

// 初始化大小类数据结构
// 参数:
//   data: 大小类数据结构体指针
// 功能:
//   1. 设置大小类数据结构的基本参数 (lg2_quantum, ntiny, nsizes, npsizes,
//   nbins,
//      lg2_tiny_maxclass, lg2_tiny_minclass, lookup_maxclass, small_maxclass,
//      large_minclass, large_maxclass)
//   2. 调用size_classes函数初始化大小类
//   3. 标记数据结构已初始化
void sc_data_init(sc_data_t* data)
{
    data->lg2_quantum = LG2_QUANTUM;
    data->ntiny = SC_NTINY;
    data->nsizes = SC_NSIZES;
    data->npsizes = SC_NPSIZES;
    data->lg2_tiny_maxclass = SC_LG2_TINY_MAX;
    data->lg2_tiny_minclass = SC_LG2_TINY_MIN;
    data->lookup_maxclass = SC_LOOKUP_MAXCLASS;
    data->small_maxclass = SC_SMALL_MAXCLASS;
    data->large_minclass = SC_LARGE_MINCLASS;
    data->large_maxclass = SC_LARGE_MAXCLASS;

    size_classes(data);

    // bin数量由表内slab可容纳的大小类决定，而非编译期宏：
    // 大小类超过slab容量的类必须走extent大块路径
    data->nbins = data->nlbins;
    sz_nbins = (size_t)data->nlbins;

    data->initialized = true;
}

// 二分查找size对应的index
size_t sc_data_find(const sc_data_t* data, size_t size)
{
    size_t l = 0, r = SC_NSIZES;
    while (l < r)
    {
        size_t m = l + (r - l) / 2;
        if (data->entries[m].size < size)
            l = m + 1;
        else
            r = m;
    }
    return (l < SC_NSIZES) ? l : SC_NSIZES - 1;
}

// 查找index对应的size
size_t sc_data_index2size(const sc_data_t* data, size_t index)
{
    if (index < SC_NSIZES)
        return data->entries[index].size;
    return 0;
}

// 查找size对应的对齐量
size_t sc_data_align(const sc_data_t* data, size_t size)
{
    size_t idx = sc_data_find(data, size);
    return data->entries[idx].align;
}