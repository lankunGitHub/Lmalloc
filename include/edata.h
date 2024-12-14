#pragma once

#include "bitmap.h"
#include "ph.h"
#include "ql.h"
#include "sc.h"
#include "sz.h"

typedef struct edata_s edata_t;

ph_structs(edata_avail, edata_t);
ph_structs(edata_heap, edata_t);

// region 元数据压缩存储
// 分配状态 bitmap 已有 slab_data
// 其他调试信息用紧凑数组存储

typedef struct region_aux_meta_s
{
    uint8_t type;         // 分配类型（tcache/bin/large/huge）
    uint32_t alloc_tid;   // 分配线程id（可选）
    uint32_t alloc_time;  // 分配时间戳（可选，秒）
    uint32_t free_tid;    // 回收线程id
    uint32_t free_time;   // 回收时间戳
    uint32_t alloc_count; // 分配次数
    uint32_t free_count;  // 回收次数
} region_aux_meta_t;

struct edata_s
{
    uint64_t e_bits;                       // 存储关于edata的相关信息
    void* addr;                            // slab/large/huge 内存块地址
    size_t e_sn;                           // 序列号
    size_t bsize;                          // 剩余大小
    bitmap_t slab_data[BITMAP_GROUPS_MAX]; // 记录每个region的分配状态
    // region 辅助元数据数组（仅调试/分析时启用，正常可省略）
    region_aux_meta_t* region_aux_arr;
    size_t nregions; // region 数量
    // slab/extent 双向链表指针，便于 slab 迁移/合并/调试
    struct edata_s* prev;
    struct edata_s* next;
};

static inline void* edata_addr_get(edata_t* edata) { return edata->addr; }
static inline size_t edata_bsize_get(edata_t* edata) { return edata->bsize; }
static inline size_t edata_sn_get(edata_t* edata) { return edata->e_sn; }
static inline bool base_edata_is_reused(edata_t* edata)
{
    (void)edata;
    return false;
} // 如有复用标志可补充

void edata_binit(
    edata_t* edata, void* addr, size_t size, size_t sn, bool is_reused);
void edata_heap_new(edata_heap_t* heap);
void edata_avail_new(edata_avail_t* avail);
void edata_avail_insert(edata_avail_t* avail, edata_t* edata);
