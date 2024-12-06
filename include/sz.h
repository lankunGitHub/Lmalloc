#pragma once

#include "sc.h"
#include "sc_ext.h"

// Size class策略模式
typedef enum
{
    SZ_POLICY_STATIC,
    SZ_POLICY_DYNAMIC,
    SZ_POLICY_ADAPTIVE
} sz_policy_mode_t;
extern sz_policy_mode_t g_sz_policy_mode;

// 策略接口抽象

typedef struct sz_policy_s sz_policy_t;
typedef size_t (*sz_size2index_fn)(void* data, size_t size);
typedef size_t (*sz_index2size_fn)(void* data, size_t index);
typedef size_t (*sz_align_fn)(void* data, size_t size);
typedef void (*sz_adapt_fn)(void* data);

struct sz_policy_s
{
    const char* name;
    void* data; // 指向 sc_data_t 或 sc_ext_data_t
    sz_size2index_fn size2index;
    sz_index2size_fn index2size;
    sz_align_fn align;
    sz_adapt_fn adapt;
};

// 设置当前 size class 策略
void sz_set_policy(sz_policy_t* policy);

// 统一查找/对齐接口，主分配路径和所有模块都用这三个接口
size_t sz_size2index(size_t size);  // 查找size对应的index
size_t sz_index2size(size_t index); // 查找index对应的size
size_t sz_align(size_t size);       // 查找size对应的对齐量

// 统一 size class 初始化入口
void sz_boot();