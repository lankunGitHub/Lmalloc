#pragma once

#include "ts.h"

typedef struct cache_bin_s cache_bin_t;
struct cache_bin_s
{
    void** stack;
    int head;
    uint16_t full;
    uint16_t empty;
    uint16_t low_water;
    // LRU支持
    uint64_t* obj_timestamps; // 每个对象的最近访问时间戳
    uint16_t capacity;        // 当前bin容量
    uint32_t hit_count;       // 命中次数
    uint32_t miss_count;      // 未命中次数
    uint32_t gc_count;        // GC次数
    uint32_t adapt_epoch;     // 上次自适应调整周期
    // 可扩展：LRU链表、跨线程handoff等
    uint16_t max_capacity;
    uint16_t min_capacity;
};

typedef cache_bin_t cache_bin_t_impl;