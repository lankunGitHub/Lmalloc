#pragma once
#include "base_struct.h"
#include "mutex.h"
#include <stddef.h>
#include <time.h>

typedef struct bin_s bin_t;
// 一个 arena 管理一组 bin（小块分配）和大块分配的元数据，支持多线程并发
typedef struct extent_node_s extent_node_t;
struct extent_node_s
{
    void* addr;
    size_t size;
    extent_node_t* next;
    time_t free_time;
    // 红黑树节点
    extent_node_t* parent;
    extent_node_t* left;
    extent_node_t* right;
    int color; // 0=黑, 1=红
    // 双向链表
    extent_node_t* prev;
};

struct arena_s
{
    malloc_mutex_t mutex;                   // 互斥锁，保护 arena 内部结构
    bin_t* bins;                            // bin 列表，按大小类分组
    unsigned n_bins;                        // bin 数量
    unsigned arena_id;                      // arena 唯一编号
    unsigned n_threads;                     // 绑定到该 arena 的线程数
    struct extent_node_s* extent_free_list; // 兼容老接口
    struct extent_node_s* extent_tree_root; // extent 红黑树根节点
    size_t alloc_count;
    size_t free_count;
    size_t current_bytes;
    size_t peak_bytes;
    // 详细统计信息
    size_t slab_count;    // 当前活跃slab数量
    size_t extent_count;  // 当前extent数量
    base_t* base;         // 每个arena独立的base分配器
    size_t slab_bytes;    // slab总字节数
    size_t extent_bytes;  // extent总字节数
    size_t decay_count;   // decay回收次数
    size_t migrate_count; // slab迁移次数
    size_t compact_count; // slab合并次数
    size_t split_count;   // slab拆分次数
    // jemalloc相关元数据
    time_t last_decay_time;   // 上次decay时间戳
    time_t last_profile_time; // 上次profile导出时间
    void* heap_profile;       // 指向heap profile数据结构
    void* debug_info;         // 调试/可视化辅助信息
};

typedef struct arena_s arena_t;
extern unsigned n_arenas;
extern struct arena_s* arenas[];