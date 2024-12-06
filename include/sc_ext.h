#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "sc.h"

// 动态/自适应size class条目
typedef struct sc_ext_s {
    size_t size;
    size_t align;
    bool slab;
    int pgs;
    size_t slab_regions; // 新增，兼容旧用法
    _Atomic size_t alloc_count;
    _Atomic size_t free_count;
    _Atomic size_t current_bytes;
    _Atomic size_t peak_bytes;
    bool is_hot;
    bool is_dynamic;
} sc_ext_t;

// 动态/自适应size class全局分布
#define SC_EXT_DATA_MAX 512

typedef struct sc_ext_data_s {
    sc_ext_t entries[SC_EXT_DATA_MAX]; // 直接内嵌静态数组
    size_t used; // 有效元素计数
} sc_ext_data_t;

// 策略接口
typedef struct sc_ext_policy_s sc_ext_policy_t;
typedef void (*sc_ext_policy_init_fn)(sc_ext_data_t* data, const sc_data_t* base);
typedef size_t (*sc_ext_policy_find_fn)(const sc_ext_data_t* data, size_t size);
typedef void (*sc_ext_policy_adapt_fn)(sc_ext_data_t* data);

typedef struct sc_ext_policy_s {
    const char* name;
    sc_ext_policy_init_fn init;
    sc_ext_policy_find_fn find;
    sc_ext_policy_adapt_fn adapt;
} sc_ext_policy_t;

// 只保留动态/自适应策略
extern sc_ext_policy_t sc_ext_dynamic_policy;
extern sc_ext_policy_t sc_ext_adaptive_policy;

extern sc_ext_data_t g_sc_ext_data;
extern sc_ext_policy_t* g_sc_ext_policy;

void sc_ext_data_init(sc_ext_data_t* data, const sc_data_t* base, sc_ext_policy_t* policy);
size_t sc_ext_data_find(const sc_ext_data_t* data, size_t size);
size_t sc_ext_data_index2size(const sc_ext_data_t* data, size_t index);
size_t sc_ext_data_align(const sc_ext_data_t* data, size_t size);
void sc_ext_data_adapt(sc_ext_data_t* data); 