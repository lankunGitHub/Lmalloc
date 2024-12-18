#pragma once

#include "base.h"
#include "mutex.h"
#include "ts.h"

#ifndef LG2_ADDR
#define LG2_ADDR 48
#endif

/*
虚拟地址 (48-bit)：
+-----------------+----------------+---------------+-------------+
| PML4 (9-bit)   | PDPT (9-bit)   | PD (9-bit)   | PT (9-bit)  |  Offset
(12-bit)
+-----------------+----------------+---------------+-------------+
*/

/*高无效位：地址总位数 - 有效地址位数（x86-64上为 64-48=16）*/
#define HBIT (1U << ((1U << (LG2_POINTER_SIZE + 3)) - LG2_ADDR))
/*低无效位*/
#define LBIT LG2_PAGE
/*有效位*/
#define UBIT (LG2_ADDR - LG2_PAGE)

#if UBIT <= 10
#define RTREEHEIGHT 1
#elif UBIT <= 36
#define RTREEHEIGHT 2
#elif UBIT <= 52
#define RTREEHEIGHT 3
#else
#error Unsupported address size
#endif

typedef struct rtree_node_s rtree_node_t;
struct rtree_node_s
{
    atomic_p_t child;
};

typedef struct rtree_leaf_s rtree_leaf_t;
struct rtree_leaf_s
{
    atomic_p_t bits;
};

typedef struct rtree_level_s rtree_level_t;
struct rtree_level_s
{
    unsigned bits;  /*该层使用的位数*/
    unsigned cbits; /*该层使用位数在整个有效位中的位置*/
};

typedef struct rtree_s rtree_t;
struct rtree_s
{
    base_t* base;
    malloc_mutex_t mutex;

#if RTREEHEIGHT == 1
    rtree_leaf_t root[1U << (UBIT / RTREEHEIGHT)];
#elif RTREEHEIGHT > 1
    rtree_node_t root[1U << (UBIT / RTREEHEIGHT)];
#endif
};

static const rtree_level_t levels[] = {
#if RTREEHEIGHT == 1
    {UBIT, HBIT + UBIT}
#elif RTREEHEIGHT == 2
    {UBIT / 2, HBIT + UBIT / 2}, {UBIT + UBIT % 2, HBIT + UBIT}
#elif RTREEHEIGHT == 3
    {UBIT / 3, HBIT + UBIT / 3},
    {UBIT / 3 + UBIT % 3 / 2, HBIT + UBIT / 3 * 2 + UBIT % 3 / 2},
    {UBIT / 3 + UBIT % 3 - UBIT % 3 / 2, HBIT + UBIT} /*需要考虑除不尽的情况*/
#endif
};

bool rtree_new(rtree_t* rtree, base_t* base, bool zero);