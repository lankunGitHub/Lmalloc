/*
 * extent_mmap.c - 基于mmap的物理内存分配与回收实现
 *
 * 主要职责：
 *   - 提供大块物理内存的分配（mmap）与回收（munmap）接口
 *   - 支持对齐、零填充、提交等参数
 *   - 作为分配器底层物理内存获取/释放的统一入口
 *
 * 关键数据结构：
 *   - 无专用结构体，直接操作虚拟内存
 *
 * 主要算法：
 *   - 调用pages_map/pages_unmap实现跨平台mmap/munmap
 *   - 支持对齐、零填充、提交等参数
 *
 * 并发/调优：
 *   - 线程安全由上层保证
 *   - 可扩展支持大页、NUMA等
 *
 * 典型调用链：
 *   base_map/arena_alloc_extent_numa_ex -> extent_alloc_mmap
 *   base_delete/arena_extent_decay -> extent_dalloc_mmap
 *
 * 设计trade-off：
 *   - 直接mmap/munmap，牺牲部分性能换取通用性和简洁性
 */
#include "extent_mmap.h"
#include "page.h"

/**
 * @brief 基于mmap分配大块物理内存
 * @param new_addr 建议分配地址（一般为NULL）
 * @param size 分配字节数
 * @param alignment 对齐字节数（需为PAGE整数倍）
 * @param zero 是否要求零填充
 * @param commit 是否要求提交物理内存
 * @return 分配到的内存地址，失败返回NULL
 *
 * 算法说明：
 *   - 调用pages_map实现跨平台mmap
 *   - 确保对齐、零填充、提交等参数
 *   - 失败时返回NULL
 *
 * 典型用法：
 *   base_map/arena_alloc_extent_numa_ex -> extent_alloc_mmap
 */
void* extent_alloc_mmap(
    void* new_addr, size_t size, size_t alignment, bool* zero, bool* commit)
{
    (void)zero;
    assert(alignment == ALIGNMENT_CEILING(alignment, PAGE));
    void* ret = pages_map(new_addr, size, alignment, commit);
    if (ret == NULL)
    {
        return NULL;
    }
    assert(ret != NULL);
    return ret;
}

/**
 * @brief 基于munmap回收大块物理内存
 * @param addr 回收地址
 * @param size 回收字节数
 * @return 总是返回true
 *
 * 算法说明：
 *   - 调用pages_unmap实现跨平台munmap
 *   - 直接回收物理内存
 *
 * 典型用法：
 *   base_delete/arena_extent_decay -> extent_dalloc_mmap
 */
bool extent_dalloc_mmap(void* addr, size_t size)
{
    pages_unmap(addr, size);
    return true;
}