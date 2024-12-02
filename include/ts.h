// 各变量类型大小
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __GNUC__
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   !!(x)
#define unlikely(x) !!(x)
#endif

#pragma once

#if __SIZEOF_POINTER__ == 8
#ifndef POINTER_SIZE
#define POINTER_SIZE   8
#endif
#define CHAR_SIZE      1
#define INT_SIZE       4
#define LONG_SIZE      8
#define LONG_LONG_SIZE 8
#define FLOAT_SIZE     4
#define DOUBLE_SIZE    8

#define LG2_POINTER_SIZE     3
#define LG2_CHAR_SIZE        0
#define LG2_INT_SIZE         2
#define LG2_LONG_SIZE        3
#define LG2_LONG_LONG_SIZE   3
#define LG2_FLOAT_SIZE       2
#define LG2_DOUBLE_SIZE      3
#define LG2_LONG_DOUBLE_SIZE 3

#elif __SIZEOF_POINTER__ == 4
#ifndef POINTER_SIZE
#define POINTER_SIZE   4
#endif
#define CHAR_SIZE      1
#define INT_SIZE       4
#define LONG_SIZE      4
#define LONG_LONG_SIZE 4
#define FLOAT_SIZE     4
#define DOUBLE_SIZE    4

#define LG2_POINTER_SIZE     2
#define LG2_CHAR_SIZE        0
#define LG2_INT_SIZE         2
#define LG2_LONG_SIZE        2
#define LG2_LONG_LONG_SIZE   2
#define LG2_FLOAT_SIZE       2
#define LG2_DOUBLE_SIZE      2
#define LG2_LONG_DOUBLE_SIZE 2

#else
#error Unsupported pointer size
#endif

#ifndef LG2_PAGE
#define LG2_PAGE        12 // Linux 默认页面大小为 4KB
#endif
#ifndef PAGE
#define PAGE            (size_t)(1U << LG2_PAGE)
#endif
#ifndef PAGE_MASK
#define PAGE_MASK       ((size_t)(PAGE - 1))
#endif
#ifndef PAGE_CEILING
#define PAGE_CEILING(s) (((s) + PAGE_MASK) & ~PAGE_MASK)
#endif

#ifndef LG2_QUANTUM
#define LG2_QUANTUM 4 // 最小对齐量的log2
#endif
#ifndef QUANTUM
#define QUANTUM     (size_t)(1U << LG2_QUANTUM)
#endif

/*
计算机的缓存系统通常按 cache line（缓存行） 进行数据传输，一个 CACHELINE 通常是
64 字节（具体大小依赖于 CPU 架构）。

如果结构体的地址不是 CACHELINE 对齐的，多个结构体
可能会共享同一个缓存行，导致 伪共享（false sharing），从而降低性能。
*/
#define CACHELINE 64

#define ZU(z) ((size_t)z)
#define ZD(z) ((ssize_t)z)
#define QU(q) ((uint64_t)q)
#define QD(q) ((int64_t)q)

// 计算addr向下对齐alignment的地址
#define ALIGNMENT_ADDR2BASE(a, alignment)                                      \
    ((void*)(((char*)(a)) -                                                    \
             (((uintptr_t)(a)) - ((uintptr_t)(a) & ((~(alignment)) + 1)))))

/* 计算与向下对齐地址的偏移量 */
#define ALIGNMENT_ADDR2OFFSET(a, alignment)                                    \
    ((size_t)((uintptr_t)(a) & (alignment - 1)))

/* 计算地址向下对齐到页 */
#ifndef PAGE_ADDR2BASE
#define PAGE_ADDR2BASE(a) ALIGNMENT_ADDR2BASE(a, PAGE)
#endif

/* 计算向上对齐alignment的地址 */
#define ALIGNMENT_CEILING(s, alignment)                                        \
    (((s) + (alignment - 1)) & ((~(alignment)) + 1))

#define USE_TSD_TLS