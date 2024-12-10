#pragma once

#include "base_struct.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 创建 base allocator，指定后端和 NUMA 节点
base_t* base_new(unsigned ind, base_backend_t* backend, int numa_node);
// 销毁 base allocator
void base_delete(base_t* base);

// 分配内存（带对齐、可指定 NUMA 节点）
void* base_alloc(base_t* base, size_t size, size_t alignment);
// 零初始化分配
void* base_calloc(size_t nmemb, size_t size);
// 调整已分配内存大小（旧块内容拷贝到新块）
void* base_realloc(void* ptr, size_t old_size, size_t new_size);
// 释放内存
void base_free(base_t* base, void* ptr, size_t size);

// 分配元数据 edata
void* base_alloc_edata(base_t* base);
// 分配 rtree
void* base_alloc_rtree(base_t* base, size_t size);
// tcache stack 分配/释放
void* b0_alloc_tcache_stack(size_t size);
void b0_dalloc_tcache_stack(void* tcache_stack);

// 获取统计信息
void base_get_stats(base_t* base, base_stats_t* stats_out);

// 设置/获取 THP 策略
void base_set_thp_mode(base_t* base, base_thp_mode_t mode);
base_thp_mode_t base_get_thp_mode(base_t* base);

// 可选：注册 trace/profile 钩子
void base_set_trace_hook(base_t* base,
                         void (*trace_hook)(const char* event,
                                            void* ptr,
                                            size_t size));

// 可选：全局 base 支持
base_t* base_global(void);
void base_global_init(base_backend_t* backend, int numa_node);
// 全局 base 惰性初始化（backend 为 NULL 时使用默认 mmap 后端）
void global_base_init(void);

// fork 支持
void base_prefork(base_t* base);
void base_postfork_parent(base_t* base);
void base_postfork_child(base_t* base);