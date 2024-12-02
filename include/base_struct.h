#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "mutex.h"

// 后端类型
typedef enum {
    BASE_BACKEND_MMAP = 0,
    BASE_BACKEND_HUGEPAGE,
    BASE_BACKEND_DAX,
    BASE_BACKEND_CUSTOM
} base_backend_kind_t;

// 后端抽象接口
struct base_backend_s {
    base_backend_kind_t kind;
    void* (*alloc)(size_t size, size_t alignment, int numa_node, void* arg);
    void  (*dealloc)(void* ptr, size_t size, void* arg);
    void* arg; // 用户自定义参数
};
typedef struct base_backend_s base_backend_t;

// THP 策略
typedef enum {
    BASE_THP_DISABLED = 0,
    BASE_THP_AUTO     = 1,
    BASE_THP_ALWAYS   = 2
} base_thp_mode_t;

// block 头部结构
typedef struct base_block_s {
    size_t size;                  // block 总大小
    struct base_block_s* next;    // 下一个 block
    void*  unused_start;          // 未用空间起始
    size_t unused_size;           // 未用空间大小
    // 可选：THP 标记、NUMA 节点等
} base_block_t;

// 统计信息
typedef struct base_stats_s {
    size_t allocated;
    size_t resident;
    size_t mapped;
    size_t n_thp;
    size_t n_alloc_calls;
    size_t n_free_calls;
    size_t edata_allocated;
    size_t rtree_allocated;
} base_stats_t;

// base allocator 主结构体
typedef struct base_s {
    malloc_mutex_t mutex;
    base_backend_t* backend;
    int numa_node; // 亲和 NUMA 节点，-1 表示不指定
    base_thp_mode_t thp_mode;
    bool auto_thp_switched;
    size_t n_thp;
    size_t pind_last;
    size_t extent_sn_next;
    base_block_t* blocks;
    // 可选：heap/edata_avail/ehooks/trace hook/元数据池等
    void* heap;
    void* edata_avail;
    void* ehooks;
    base_stats_t stats;
    void* trace_hook;
} base_t; 