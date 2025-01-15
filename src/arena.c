#define _GNU_SOURCE
/*
 * arena.c - 内存分配器核心Arena管理实现
 *
 * 主要职责：
 *   - 管理多线程/多核环境下的arena实例，实现分配隔离与并发扩展
 *   - 支持NUMA感知分配、per-core/per-node arena选择
 *   - 管理小块分配(bin/slab)、大块分配(extent)、空闲合并、红黑树管理
 *   - 支持extent decay（定期回收未用大块）、后台回收线程
 *   - 统计NUMA分配/回收/大页等多维度指标
 *   - 提供调试、可视化、导出等辅助接口
 *
 * 关键数据结构：
 *   - arena_t: 代表一个分配域，包含bin数组、extent空闲链表/红黑树、统计等
 *   - extent_node_t: 管理大块内存的元数据，支持链表和红黑树组织
 *
 * 主要算法：
 *   - 小块分配采用bin+slab，支持批量迁移/合并/拆分
 *   - 大块分配采用best-fit红黑树，支持空闲合并、插入/删除fixup
 *   - NUMA感知分配，优先本地node/core
 *   - extent decay机制，后台线程定期回收长时间未用大块
 *
 * 并发/NUMA/调优：
 *   - 多arena并发，线程绑定arena，减少锁竞争
 *   - NUMA分配统计，支持大页、节点亲和
 *   - 支持后台回收线程、自动调优参数
 *
 * 调试与可视化：
 *   - extent树导出dot格式，支持Graphviz可视化
 *   - 详细统计打印、heap dump、NUMA统计
 *
 * 典型调用链：
 *   lmalloc -> arena_choose/arena_malloc_small -> bin/slab/extent
 *
 * 设计trade-off：
 *   - 牺牲部分碎片率换取并发扩展性
 *   - 红黑树管理大块，兼顾分配效率与合并效果
 *   - NUMA/大页支持可选，兼容通用平台
 */
#include "arena.h"
#include "bin.h"
#include "sc.h"
#include "sz.h"
#include <assert.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef HAVE_NUMA
#include <numa.h>
#endif
#include "arena_struct.h"
#include "base.h"
#include "tsd.h"
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EXTENT_DECAY_MAX_FREE 8  // decay时允许的最大空闲extent数
#define EXTENT_DECAY_INTERVAL 10 // decay回收的最小间隔（秒）

// 全局arena列表及并发管理
#define MAX_ARENAS 64
arena_t* arenas[MAX_ARENAS]; // 所有arena实例
unsigned n_arenas = 0;       // 当前arena数量
struct numa_stats_s g_numa_stats = {0};

/**
 * @brief 初始化全局arena列表（per-core支持）
 *
 * 典型调用链：lmalloc_init -> arena_boot
 *
 * 线程安全：假定只在初始化阶段调用
 *
 * 实现要点：
 *   - 检测CPU核数，按核分配arena
 *   - 支持最大MAX_ARENAS限制
 *   - 每个arena独立分配bin数组
 */
void arena_boot(void)
{
    if (n_arenas == 0)
    {
        int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpu > MAX_ARENAS)
            ncpu = MAX_ARENAS;
        for (int i = 0; i < ncpu; ++i)
        {
            arenas[i] = arena_new(i);
        }
        n_arenas = ncpu;
        printf("[arena_boot] initialized %u arenas (per-core)\n", n_arenas);
    }
}

/**
 * @brief 选择当前线程使用的arena（优先本地core/node）
 *
 * @param tsd 线程私有数据结构
 * @return 绑定的arena指针
 *
 * 线程安全：每个线程独立选择/绑定arena
 *
 * NUMA优化：优先选择本地NUMA node的arena
 *
 * 设计trade-off：
 *   - 绑定arena减少锁竞争，提升并发扩展性
 *   - 若core/node超出范围，随机分配
 */
arena_t* arena_choose(tsd_t* tsd)
{
    if (tsd->arena)
        return tsd->arena;
    int core = sched_getcpu();
#ifdef HAVE_NUMA
    int node = numa_node_of_cpu(core);
    if (node >= 0 && node < (int)n_arenas)
        core = node;
#endif
    if (core < 0 || core >= (int)n_arenas)
        core = rand() % n_arenas;
    tsd->arena = arenas[core];
    tsd->arena_id = core;
    atomic_fetch_add(&arenas[core]->n_threads, 1);
    return tsd->arena;
}

/**
 * @brief 初始化单个arena实例
 *
 * @param arena_id 唯一编号
 * @return 新分配的arena指针
 *
 * 线程安全：假定只在初始化阶段调用
 *
 * 实现要点：
 *   - 分配bin数组，初始化每个bin
 *   - 统计字段清零
 */
arena_t* arena_new(unsigned arena_id)
{
    // 用全局base分配arena_t结构体
    arena_t* arena = (arena_t*)base_alloc(base_global(), sizeof(arena_t), QUANTUM);
    if (!arena)
        return NULL;
    memset(arena, 0, sizeof(arena_t));
    // 每个arena有自己的base
    arena->base = base_new(arena_id, NULL, -1);
    if (!arena->base)
    {
        return NULL;
    }
    arena->n_bins = SC_NBINS;
    arena->bins = (bin_t*)base_alloc(arena->base, SC_NBINS * sizeof(bin_t), QUANTUM);
    if (arena->bins)
        memset(arena->bins, 0, SC_NBINS * sizeof(bin_t));
    for (unsigned i = 0; i < arena->n_bins; ++i)
    {
        bin_t* bin = &arena->bins[i];
        bin->arena = arena;
        // 每个bin按自身size class切分region
        bin_init(bin, sz_index2size(i));
    }
    arena->arena_id = arena_id;
    arena->n_threads = 0;
    return arena;
}

/**
 * @brief 小块分配：根据ind选择bin，分配region
 *
 * @param arena 当前arena
 * @param ind bin索引
 * @return 分配到的region指针
 *
 * 线程安全：需外部保证
 *
 * 典型调用链：lmalloc -> arena_choose -> arena_malloc_small
 */
void* arena_malloc_small(arena_t* arena, szind_t ind)
{
    if (!arena || ind >= arena->n_bins)
        return NULL;
    bin_t* bin = &arena->bins[ind];
    void* p = bin_malloc_region(bin);
    if (p)
    {
        arena->alloc_count++;
        bin->alloc_count++;
        bin->current_bytes += bin->slabcur ? bin->slabcur->nregions : 0;
        if (bin->current_bytes > bin->peak_bytes)
            bin->peak_bytes = bin->current_bytes;
        arena->current_bytes++;
        if (arena->current_bytes > arena->peak_bytes)
            arena->peak_bytes = arena->current_bytes;
    }
    return p;
}

/**
 * @brief 小块回收：根据ind选择bin，回收region
 *
 * @param arena 当前arena
 * @param ptr region指针
 * @param ind bin索引
 *
 * 线程安全：需外部保证
 */
void arena_dalloc_small(arena_t* arena, void* ptr, szind_t ind)
{
    if (!arena || ind >= arena->n_bins)
        return;
    bin_t* bin = &arena->bins[ind];
    bin_dalloc_region(bin, ptr);
    arena->free_count++;
    bin->free_count++;
    if (bin->current_bytes > 0)
        bin->current_bytes--;
    if (arena->current_bytes > 0)
        arena->current_bytes--;
}

/**
 * @brief extent_free_list按地址有序插入并合并相邻空闲块
 *
 * @param arena 当前arena
 * @param addr 空闲块地址
 * @param size 空闲块大小
 *
 * 线程安全：需外部加锁
 *
 * 算法说明：
 *   - 按地址有序插入，若与前/后块相邻则合并
 *   - 合并后释放多余节点
 */
void arena_insert_extent_merge(arena_t* arena, void* addr, size_t size)
{
    extent_node_t** prev = &arena->extent_free_list;
    extent_node_t* node = arena->extent_free_list;
    uintptr_t new_addr = (uintptr_t)addr;
    uintptr_t new_end = new_addr + size;
    // 查找插入点
    while (node && (uintptr_t)node->addr < new_addr)
    {
        prev = &node->next;
        node = node->next;
    }
    // 检查与前一个合并
    if (*prev && (uintptr_t)(*prev)->addr + (*prev)->size == new_addr)
    {
        (*prev)->size += size;
        // 检查与下一个合并
        if (node && new_end == (uintptr_t)node->addr)
        {
            (*prev)->size += node->size;
            (*prev)->next = node->next;
            // extent_node_t等元数据回收时，直接插入extent_free_list或空闲树，不做free。
        }
        return;
    }
    // 检查与下一个合并
    if (node && new_end == (uintptr_t)node->addr)
    {
        node->addr = addr;
        node->size += size;
        // extent_node_t等元数据回收时，直接插入extent_free_list或空闲树，不做free。
        return;
    }
    // 插入新节点
    extent_node_t* new_node = (extent_node_t*)base_alloc(
        arena->base, sizeof(extent_node_t), QUANTUM);
    new_node->addr = addr;
    new_node->size = size;
    new_node->next = node;
    new_node->free_time = time(NULL);
    *prev = new_node;
}

// 红黑树辅助宏和函数
#define EXTENT_RB_BLACK 0
#define EXTENT_RB_RED   1

/**
 * @brief 红黑树左旋操作
 *
 * @param root 红黑树根节点指针
 * @param x 需要左旋的节点
 *
 * 算法说明：
 *   - 经典红黑树左旋，维护父子关系
 *   - 用于插入/删除fixup
 */
static void extent_left_rotate(extent_node_t** root, extent_node_t* x)
{
    extent_node_t* y = x->right;
    x->right = y->left;
    if (y->left)
        y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent)
        *root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->left = x;
    x->parent = y;
}

/**
 * @brief 红黑树右旋操作
 *
 * @param root 红黑树根节点指针
 * @param y 需要右旋的节点
 */
static void extent_right_rotate(extent_node_t** root, extent_node_t* y)
{
    extent_node_t* x = y->left;
    y->left = x->right;
    if (x->right)
        x->right->parent = y;
    x->parent = y->parent;
    if (!y->parent)
        *root = x;
    else if (y == y->parent->left)
        y->parent->left = x;
    else
        y->parent->right = x;
    x->right = y;
    y->parent = x;
}

/**
 * @brief 红黑树插入修正（fixup）
 *
 * @param root 红黑树根节点指针
 * @param z 新插入节点
 *
 * 算法说明：
 *   - 经典红黑树插入fixup，保持红黑树性质
 */
static void extent_rbtree_insert_fixup(extent_node_t** root, extent_node_t* z)
{
    while (z->parent && z->parent->color == EXTENT_RB_RED)
    {
        if (z->parent == z->parent->parent->left)
        {
            extent_node_t* y = z->parent->parent->right;
            if (y && y->color == EXTENT_RB_RED)
            {
                z->parent->color = EXTENT_RB_BLACK;
                y->color = EXTENT_RB_BLACK;
                z->parent->parent->color = EXTENT_RB_RED;
                z = z->parent->parent;
            }
            else
            {
                if (z == z->parent->right)
                {
                    z = z->parent;
                    extent_left_rotate(root, z);
                }
                z->parent->color = EXTENT_RB_BLACK;
                z->parent->parent->color = EXTENT_RB_RED;
                extent_right_rotate(root, z->parent->parent);
            }
        }
        else
        {
            extent_node_t* y = z->parent->parent->left;
            if (y && y->color == EXTENT_RB_RED)
            {
                z->parent->color = EXTENT_RB_BLACK;
                y->color = EXTENT_RB_BLACK;
                z->parent->parent->color = EXTENT_RB_RED;
                z = z->parent->parent;
            }
            else
            {
                if (z == z->parent->left)
                {
                    z = z->parent;
                    extent_right_rotate(root, z);
                }
                z->parent->color = EXTENT_RB_BLACK;
                z->parent->parent->color = EXTENT_RB_RED;
                extent_left_rotate(root, z->parent->parent);
            }
        }
    }
    (*root)->color = EXTENT_RB_BLACK;
}

/**
 * @brief extent红黑树插入（按地址有序）
 *
 * @param root 红黑树根节点指针
 * @param node 新插入节点
 *
 * 算法说明：
 *   - 按size/地址有序插入
 *   - 插入后调用fixup保持红黑树性质
 */
static void extent_tree_insert(extent_node_t** root, extent_node_t* node)
{
    extent_node_t* y = NULL;
    extent_node_t* x = *root;
    while (x)
    {
        y = x;
        if (node->size < x->size ||
            (node->size == x->size && node->addr < x->addr))
            x = x->left;
        else
            x = x->right;
    }
    node->parent = y;
    if (!y)
        *root = node;
    else if (node->size < y->size ||
             (node->size == y->size && node->addr < y->addr))
        y->left = node;
    else
        y->right = node;
    node->left = node->right = NULL;
    node->color = EXTENT_RB_RED;
    extent_rbtree_insert_fixup(root, node);
}

/**
 * @brief 红黑树best-fit查找
 * @param root 根节点
 * @param size 需求大小
 * @return 最小满足size的节点
 *
 * 算法说明：
 *   - 类似lower_bound，优先左子树
 */
static extent_node_t* extent_tree_best_fit(extent_node_t* root, size_t size)
{
    extent_node_t* res = NULL;
    while (root)
    {
        if (root->size >= size)
        {
            res = root;
            root = root->left;
        }
        else
        {
            root = root->right;
        }
    }
    return res;
}

/**
 * @brief 红黑树删除fixup
 * @param root 根节点指针
 * @param x 替换节点
 * @param x_parent 替换节点父节点
 *
 * 算法说明：
 *   - 经典红黑树删除fixup，保持红黑树性质
 */
static void extent_rbtree_delete_fixup(extent_node_t** root,
                                       extent_node_t* x,
                                       extent_node_t* x_parent)
{
    // 这里只实现最基本的修正流程，实际可参考CLRS红黑树删除fixup算法
    while (x != *root && (!x || x->color == EXTENT_RB_BLACK))
    {
        if (x == x_parent->left)
        {
            extent_node_t* w = x_parent->right;
            if (w && w->color == EXTENT_RB_RED)
            {
                w->color = EXTENT_RB_BLACK;
                x_parent->color = EXTENT_RB_RED;
                extent_left_rotate(root, x_parent);
                w = x_parent->right;
            }
            if ((!w->left || w->left->color == EXTENT_RB_BLACK) &&
                (!w->right || w->right->color == EXTENT_RB_BLACK))
            {
                if (w)
                    w->color = EXTENT_RB_RED;
                x = x_parent;
                x_parent = x->parent;
            }
            else
            {
                if (!w->right || w->right->color == EXTENT_RB_BLACK)
                {
                    if (w->left)
                        w->left->color = EXTENT_RB_BLACK;
                    w->color = EXTENT_RB_RED;
                    extent_right_rotate(root, w);
                    w = x_parent->right;
                }
                if (w)
                    w->color = x_parent->color;
                x_parent->color = EXTENT_RB_BLACK;
                if (w && w->right)
                    w->right->color = EXTENT_RB_BLACK;
                extent_left_rotate(root, x_parent);
                x = *root;
            }
        }
        else
        {
            extent_node_t* w = x_parent->left;
            if (w && w->color == EXTENT_RB_RED)
            {
                w->color = EXTENT_RB_BLACK;
                x_parent->color = EXTENT_RB_RED;
                extent_right_rotate(root, x_parent);
                w = x_parent->left;
            }
            if ((!w->right || w->right->color == EXTENT_RB_BLACK) &&
                (!w->left || w->left->color == EXTENT_RB_BLACK))
            {
                if (w)
                    w->color = EXTENT_RB_RED;
                x = x_parent;
                x_parent = x->parent;
            }
            else
            {
                if (!w->left || w->left->color == EXTENT_RB_BLACK)
                {
                    if (w->right)
                        w->right->color = EXTENT_RB_BLACK;
                    w->color = EXTENT_RB_RED;
                    extent_left_rotate(root, w);
                    w = x_parent->left;
                }
                if (w)
                    w->color = x_parent->color;
                x_parent->color = EXTENT_RB_BLACK;
                if (w && w->left)
                    w->left->color = EXTENT_RB_BLACK;
                extent_right_rotate(root, x_parent);
                x = *root;
            }
        }
    }
    if (x)
        x->color = EXTENT_RB_BLACK;
}

/**
 * @brief extent红黑树删除实现（带详细注释，完全对标CLRS）
 *
 * @param root 根节点指针
 * @param z 待删除节点
 *
 * 算法说明：
 *   - 1. z最多只有一个非空子节点，直接用子节点替换z
 *   - 2. z有两个非空子节点，找z的后继y（右子树最左节点），用y替换z
 *   - 3. 删除后如有黑色节点丢失，需调用fixup修正红黑树性质
 */
void extent_tree_delete(extent_node_t** root, extent_node_t* z)
{
    /*
     * 红黑树删除分三种情况：
     * 1. z最多只有一个非空子节点，直接用子节点替换z。
     * 2. z有两个非空子节点，找到z的后继y（右子树最左节点），用y替换z。
     * 3. 删除后如有黑色节点丢失，需调用fixup修正红黑树性质。
     */
    extent_node_t* y = z;
    extent_node_t* x = NULL;
    extent_node_t* x_parent = NULL;
    int y_original_color = y->color;
    if (z->left == NULL)
    {
        x = z->right;
        x_parent = z->parent;
        if (z->parent)
        {
            if (z == z->parent->left)
                z->parent->left = z->right;
            else
                z->parent->right = z->right;
        }
        else
        {
            *root = z->right;
        }
        if (z->right)
            z->right->parent = z->parent;
    }
    else if (z->right == NULL)
    {
        x = z->left;
        x_parent = z->parent;
        if (z->parent)
        {
            if (z == z->parent->left)
                z->parent->left = z->left;
            else
                z->parent->right = z->left;
        }
        else
        {
            *root = z->left;
        }
        if (z->left)
            z->left->parent = z->parent;
    }
    else
    {
        // 找到z的后继y
        y = z->right;
        while (y->left)
            y = y->left;
        y_original_color = y->color;
        x = y->right;
        if (y->parent == z)
        {
            if (x)
                x->parent = y;
            x_parent = y;
        }
        else
        {
            if (y->parent)
            {
                if (y == y->parent->left)
                    y->parent->left = x;
                else
                    y->parent->right = x;
            }
            if (x)
                x->parent = y->parent;
            y->right = z->right;
            if (y->right)
                y->right->parent = y;
            x_parent = y->parent;
        }
        if (z->parent)
        {
            if (z == z->parent->left)
                z->parent->left = y;
            else
                z->parent->right = y;
        }
        else
        {
            *root = y;
        }
        y->parent = z->parent;
        y->left = z->left;
        if (y->left)
            y->left->parent = y;
        y->color = z->color;
    }
    // 删除后修正红黑树性质
    if (y_original_color == EXTENT_RB_BLACK)
    {
        extent_rbtree_delete_fixup(root, x, x_parent);
    }
    // extent_node_t等元数据回收时，直接插入extent_free_list或空闲树，不做free。
}

/**
 * @brief extent分配主流程：best-fit策略
 *
 * @param arena 当前arena
 * @param size 需求大小
 * @return 分配到的内存地址
 *
 * 算法说明：
 *   - 优先从红黑树找best-fit块
 *   - 若无可用块，回退malloc
 *   - 支持分割或整块分配
 */
static void* arena_alloc_extent_tree(arena_t* arena, size_t size)
{
    extent_node_t* node = extent_tree_best_fit(arena->extent_tree_root, size);
    if (!node)
        return base_alloc(arena->base, size, QUANTUM);
    // 分割或整块分配
    void* addr = node->addr;
    if (node->size == size)
    {
        extent_tree_delete(&arena->extent_tree_root, node);
        return addr;
    }
    else
    {
        node->addr = (char*)node->addr + size;
        node->size -= size;
        return addr;
    }
}

/**
 * @brief extent回收主流程：插入红黑树并合并相邻块
 *
 * @param arena 当前arena
 * @param addr 回收地址
 * @param size 回收大小
 *
 * 算法说明：
 *   - 查找左右邻居，合并后插入红黑树
 */
static void arena_free_extent_tree(arena_t* arena, void* addr, size_t size)
{
    uintptr_t a = (uintptr_t)addr;
    uintptr_t b = a + size;
    extent_node_t* left = NULL;
    extent_node_t* right = NULL;
    // 查找左邻、右邻
    extent_node_t* node = arena->extent_tree_root;
    while (node)
    {
        if ((uintptr_t)node->addr + node->size == a)
            left = node;
        if ((uintptr_t)node->addr == b)
            right = node;
        if (b <= (uintptr_t)node->addr)
            node = node->left;
        else if (a >= (uintptr_t)node->addr + node->size)
            node = node->right;
        else
            break;
    }
    // 合并左邻
    if (left)
    {
        extent_tree_delete(&arena->extent_tree_root, left);
        addr = left->addr;
        size += left->size;
        // extent_node_t等元数据回收时，直接插入extent_free_list或空闲树，不做free。
    }
    // 合并右邻
    if (right)
    {
        extent_tree_delete(&arena->extent_tree_root, right);
        size += right->size;
        // extent_node_t等元数据回收时，直接插入extent_free_list或空闲树，不做free。
    }
    // 插入新节点
    extent_node_t* new_node = (extent_node_t*)base_alloc(
        arena->base, sizeof(extent_node_t), QUANTUM);
    new_node->addr = addr;
    new_node->size = size;
    new_node->parent = new_node->left = new_node->right = new_node->prev = NULL;
    new_node->color = EXTENT_RB_RED;
    new_node->free_time = time(NULL);
    extent_tree_insert(&arena->extent_tree_root, new_node);
}

/**
 * @brief extent decay机制：定期回收长时间未用的extent
 *
 * @param arena 当前arena
 *
 * 算法说明：
 *   - 超过阈值和时间的空闲块直接munmap回收
 *   - 典型后台线程定期调用
 */
void arena_extent_decay(arena_t* arena)
{
    extent_node_t** prev = &arena->extent_free_list;
    extent_node_t* node = arena->extent_free_list;
    time_t now = time(NULL);
    int count = 0;
    while (node)
    {
        count++;
        if (count > EXTENT_DECAY_MAX_FREE &&
            now - node->free_time > EXTENT_DECAY_INTERVAL)
        {
            munmap(node->addr, node->size);
            *prev = node->next;
            node = node->next;
            // extent_node_t等元数据回收时，直接插入extent_free_list或空闲树，不做free。
            continue;
        }
        prev = &node->next;
        node = node->next;
    }
}

/**
 * @brief NUMA感知slab/extent分配，支持node参数和大页
 *
 * @param arena 当前arena
 * @param size 分配大小
 * @param node NUMA节点
 * @param use_hugepage 是否使用大页
 * @return 分配到的内存指针
 *
 * 算法说明：
 *   - 优先本地NUMA节点，支持大页
 *   - 回退普通malloc
 *   - 统计NUMA分配
 */
void* arena_alloc_extent_numa_ex(arena_t* arena,
                                 size_t size,
                                 int node,
                                 int use_hugepage)
{
#ifdef HAVE_NUMA
    if (node < 0)
    {
        int cpu = sched_getcpu();
        node = numa_node_of_cpu(cpu);
    }
    if (node >= 0 && numa_available() != -1)
    {
        int flags = 0;
        void* mem = NULL;
        if (use_hugepage)
        {
#ifdef MAP_HUGETLB
            flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;
            mem = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
            if (mem != MAP_FAILED)
            {
                g_numa_stats.hugepage_count[node]++;
                g_numa_stats.hugepage_bytes[node] += size;
            }
            else
            {
                mem = NULL; // 回退普通页
            }
#endif
        }
        if (!mem)
        {
            mem = numa_alloc_onnode(size, node);
        }
        if (mem)
        {
            g_numa_stats.alloc_count[node]++;
            g_numa_stats.alloc_bytes[node] += size;
            return mem;
        }
    }
#endif
    // Fallback: 普通分配
    (void)node;
    (void)use_hugepage;
    void* mem = base_alloc(arena->base, size, QUANTUM);
    g_numa_stats.alloc_count[0]++;
    g_numa_stats.alloc_bytes[0] += size;
    return mem;
}

/**
 * @brief extent/slab回收时统计NUMA
 *
 * @param addr 回收地址
 * @param size 回收大小
 * @param node NUMA节点
 * @param is_hugepage 是否大页
 */
void arena_free_extent_numa(void* addr, size_t size, int node, int is_hugepage)
{
    (void)addr;
    if (node < 0)
        node = 0;
    g_numa_stats.free_count[node]++;
    g_numa_stats.free_bytes[node] += size;
    if (is_hugepage)
    {
        g_numa_stats.hugepage_count[node]++;
        g_numa_stats.hugepage_bytes[node] += size;
    }
}

/**
 * @brief NUMA感知slab/extent分配（简化接口，不指定NUMA节点）
 * @param arena 当前arena
 * @param size 分配大小
 * @return 分配到的内存指针
 */
void* arena_alloc_extent_numa(arena_t* arena, size_t size)
{
    return arena_alloc_extent_numa_ex(arena, size, -1, 0);
}

/**
 * @brief NUMA统计打印接口
 * 打印每个NUMA节点的分配统计信息
 */
void numa_stats_print(void)
{
#ifdef HAVE_NUMA
    int maxnode = numa_max_node();
    printf("[NUMA] 分配统计：\n");
    for (int i = 0; i <= maxnode && i < 64; ++i)
    {
        printf("  node %d: alloc_count=%zu alloc_bytes=%zu\n",
               i,
               g_numa_stats.alloc_count[i],
               g_numa_stats.alloc_bytes[i]);
    }
#endif
}

/**
 * @brief extent_tree中序遍历打印
 * @param root 根节点
 * 打印每个extent的地址、大小、颜色
 */
void extent_tree_inorder(extent_node_t* root)
{
    if (!root)
        return;
    extent_tree_inorder(root->left);
    printf("[extent] addr=%p size=%zu color=%s\n",
           root->addr,
           root->size,
           root->color == EXTENT_RB_RED ? "RED" : "BLACK");
    extent_tree_inorder(root->right);
}

/**
 * @brief extent_tree统计节点数和总字节数
 * @param root 根节点
 * @param count 节点计数
 * @param total_bytes 总字节数
 */
void extent_tree_stats(extent_node_t* root, size_t* count, size_t* total_bytes)
{
    if (!root)
        return;
    (*count)++;
    (*total_bytes) += root->size;
    extent_tree_stats(root->left, count, total_bytes);
    extent_tree_stats(root->right, count, total_bytes);
}

/**
 * @brief extent_tree导出为dot格式（Graphviz可视化）
 * @param root 根节点
 * @param f 输出文件指针
 */
void extent_tree_export_dot(extent_node_t* root, FILE* f)
{
    if (!root)
        return;
    if (root->left)
    {
        fprintf(f,
                "\t\"%p[%zu]\" -> \"%p[%zu]\" [label=\"L\"]\n",
                root,
                root->size,
                root->left,
                root->left->size);
        extent_tree_export_dot(root->left, f);
    }
    if (root->right)
    {
        fprintf(f,
                "\t\"%p[%zu]\" -> \"%p[%zu]\" [label=\"R\"]\n",
                root,
                root->size,
                root->right,
                root->right->size);
        extent_tree_export_dot(root->right, f);
    }
}

/**
 * @brief 调试接口：打印arena extent树结构和统计，导出dot文件
 * @param arena 当前arena
 *
 * 典型用法：
 *   arena_extent_tree_debug(arena);
 *   dot -Tpng arena_extent_tree.dot -o tree.png
 */
void arena_extent_tree_debug(arena_t* arena)
{
    printf("==== Arena %u Extent Tree ====\n", arena->arena_id);
    extent_tree_inorder(arena->extent_tree_root);
    size_t count = 0, total = 0;
    extent_tree_stats(arena->extent_tree_root, &count, &total);
    printf("节点数: %zu, 总字节数: %zu\n", count, total);
    FILE* f = fopen("arena_extent_tree.dot", "w");
    if (f)
    {
        fprintf(f, "digraph extent_tree {\n");
        extent_tree_export_dot(arena->extent_tree_root, f);
        fprintf(f, "}\n");
        fclose(f);
        printf("已导出为 arena_extent_tree.dot (可用dot -Tpng可视化)\n");
    }
}

// 后台decay回收线程参数与实现
int g_decay_interval_sec = 10; // 回收周期（秒），可被auto_tune调整
int g_decay_threshold = 8;     // 超过多少个空闲extent触发回收
static int g_decay_thread_running = 0;
static pthread_t g_decay_thread;

/**
 * @brief 后台decay回收线程主函数
 *
 * 典型用法：arena_decay_thread_start()
 */
void* arena_decay_thread_func(void* arg)
{
    (void)arg;
    while (g_decay_thread_running)
    {
        sleep(g_decay_interval_sec);
        for (unsigned i = 0; i < n_arenas; ++i)
        {
            arena_extent_decay(arenas[i]);
        }
    }
    return NULL;
}

/**
 * @brief 启动后台decay回收线程
 * @param interval_sec 回收周期
 * @param threshold 空闲extent阈值
 */
void arena_decay_thread_start(int interval_sec, int threshold)
{
    g_decay_interval_sec = interval_sec;
    g_decay_threshold = threshold;
    g_decay_thread_running = 1;
    pthread_create(&g_decay_thread, NULL, arena_decay_thread_func, NULL);
    printf("[decay] 后台回收线程已启动，周期=%ds，阈值=%d\n",
           interval_sec,
           threshold);
}

/**
 * @brief 停止后台decay回收线程
 */
void arena_decay_thread_stop()
{
    g_decay_thread_running = 0;
    pthread_join(g_decay_thread, NULL);
    printf("[decay] 后台回收线程已停止\n");
}

/**
 * @brief 大块分配：优先从extent红黑树分配，不足则malloc
 * @param arena 当前arena
 * @param size 分配字节数
 * @return 分配到的内存指针
 *
 * 线程安全：加锁保护
 */
void* arena_malloc_large(arena_t* arena, size_t size)
{
    void* ret = NULL;
    malloc_mutex_lock(TSDN_NULL, &arena->mutex);
    ret = arena_alloc_extent_tree(arena, size);
    if (ret)
    {
        arena->alloc_count++;
        arena->current_bytes += size;
        if (arena->current_bytes > arena->peak_bytes)
            arena->peak_bytes = arena->current_bytes;
    }
    malloc_mutex_unlock(TSDN_NULL, &arena->mutex);
    return ret;
}

/**
 * @brief 大块回收：插入extent红黑树并合并
 * @param arena 当前arena
 * @param ptr 回收指针
 * @param size 回收字节数
 *
 * 线程安全：加锁保护
 */
void arena_dalloc_large(arena_t* arena, void* ptr, size_t size)
{
    malloc_mutex_lock(TSDN_NULL, &arena->mutex);
    arena_free_extent_tree(arena, ptr, size);
    arena->free_count++;
    if (arena->current_bytes > size)
        arena->current_bytes -= size;
    else
        arena->current_bytes = 0;
    malloc_mutex_unlock(TSDN_NULL, &arena->mutex);
}

/**
 * @brief 获取arena统计快照（遍历bin/slab实时计算）
 * @param arena 目标arena
 * @param out 输出统计结构体
 */
void arena_stats_get(arena_t* arena, arena_stats_t* out)
{
    memset(out, 0, sizeof(*out));
    if (!arena)
        return;
    out->alloc_count = arena->alloc_count;
    out->free_count = arena->free_count;
    out->current_bytes = arena->current_bytes;
    out->peak_bytes = arena->peak_bytes;
    out->decay_count = arena->decay_count;
    out->migrate_count = arena->migrate_count;
    out->compact_count = arena->compact_count;
    out->split_count = arena->split_count;
    out->n_threads = arena->n_threads;
    // 遍历bin统计slab数量与使用情况
    for (unsigned b = 0; b < arena->n_bins; ++b)
    {
        bin_t* bin = &arena->bins[b];
        slab_t* slabs[3] = {(slab_t*)bin->slabcur,
                            (slab_t*)bin->unfull_slabs,
                            (slab_t*)bin->full_slabs.qlh_first};
        for (int s = 0; s < 3; ++s)
        {
            slab_t* slab = slabs[s];
            while (slab)
            {
                out->slab_count++;
                out->slab_bytes += sizeof(slab_t);
                slab = slab->link.qre_next;
            }
        }
    }
    // 统计空闲extent
    extent_node_t* node = arena->extent_free_list;
    while (node)
    {
        out->extent_count++;
        out->extent_bytes += node->size;
        node = node->next;
    }
}

/**
 * @brief 打印单个arena统计
 * @param arena 目标arena
 */
void arena_stats_print(arena_t* arena)
{
    arena_stats_t s;
    arena_stats_get(arena, &s);
    printf("[arena %u] threads=%zu alloc=%zu free=%zu cur=%zu peak=%zu "
           "slabs=%zu(%zuB) extents=%zu(%zuB) decay=%zu migrate=%zu "
           "compact=%zu split=%zu\n",
           arena->arena_id,
           s.n_threads,
           s.alloc_count,
           s.free_count,
           s.current_bytes,
           s.peak_bytes,
           s.slab_count,
           s.slab_bytes,
           s.extent_count,
           s.extent_bytes,
           s.decay_count,
           s.migrate_count,
           s.compact_count,
           s.split_count);
}

/**
 * @brief 打印所有arena统计
 */
void arena_stats_print_all(void)
{
    for (unsigned i = 0; i < n_arenas; ++i)
    {
        arena_stats_print(arenas[i]);
    }
}