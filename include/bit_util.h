#pragma once

#include "ts.h"

#if defined(__GNUC__) || defined(__clang__)

#define INTERNAL_FFSLL __builtin_ffsll
#define INTERNAL_FFSL  __builtin_ffsl
#define INTERNAL_FFS   __builtin_ffs
#define util_assume    assert

#define INTERNAL_CLZLL __builtin_clzll
#define INTERNAL_CLZL  __builtin_clzl
#define INTERNAL_CLZ   __builtin_clz

#define INTERNAL_POPCOUNT   __builtin_popcount
#define INTERNAL_POPCOUNTL  __builtin_popcountl
#define INTERNAL_POPCOUNTLL __builtin_popcountll

#endif

#if !defined(INTERNAL_FFSLL) || !defined(INTERNAL_FFSL) ||                     \
    !defined(INTERNAL_FFS)
#error INTERNAL_FFS{,L,LL} should have been defined
#endif

#if !defined(INTERNAL_CLZLL) || !defined(INTERNAL_CLZL) ||                     \
    !defined(INTERNAL_CLZ)
#error INTERNAL_CLZ{,L,LL} should have been defined
#endif

#if !defined(INTERNAL_POPCOUNT) || !defined(INTERNAL_POPCOUNTL) ||             \
    !defined(INTERNAL_POPCOUNTLL)
#error INTERNAL_POPCOUNT{,L,LL} should have been defined
#endif

// 判断最低有效位
static inline unsigned ffs_llu(unsigned long long x)
{
    util_assume(x != 0);
    return INTERNAL_FFSLL(x) - 1;
}

static inline unsigned ffs_lu(unsigned long x)
{
    util_assume(x != 0);
    return INTERNAL_FFSL(x) - 1;
}

static inline unsigned ffs_u(unsigned x)
{
    util_assume(x != 0);
    return INTERNAL_FFS(x) - 1;
}

static inline unsigned ffs_zu(size_t x)
{
#if POINTER_SIZE == LONG_LONG_SIZE
    return ffs_u(x);
#elif POINTER_SIZE == LONG_SIZE
    return ffs_lu(x);
#elif POINTER_SIZE == INT_SIZE
    return ffs_llu(x);
#else
#error No implementation for size_t ffs()
#endif
}

static inline unsigned ffs_u64(uint64_t x)
{
#if LONG_SIZE == 8
    return ffs_lu(x);
#elif LONG_LONG_size == 8
    return ffs_llu(x);
#else
#error No implementation for 64-bit ffs()
#endif
}

static inline unsigned ffs_u32(uint32_t x)
{
#if INT_SIZE == 4
    return ffs_u(x);
#else
#error No implementation for 32-bit ffs()
#endif
}

// 判断最高有效位
static inline unsigned fls_lu(unsigned long x)
{
    util_assume(x != 0);
    return (8 * sizeof(x) - 1) ^ __builtin_clzl(x);
}

static inline unsigned fls_u(unsigned x)
{
    util_assume(x != 0);
    return (8 * sizeof(x) - 1) ^ __builtin_clz(x);
}

static inline unsigned fls_llu(unsigned long long x)
{
    util_assume(x != 0);
    return (8 * sizeof(x) - 1) ^ __builtin_clzll(x);
}

static inline unsigned fls_zu(size_t x)
{
#if POINTER_SIZE == LONG_LONG_SIZE
    return fls_u(x);
#elif POINTER_SIZE == LONG_SIZE
    return fls_lu(x);
#elif POINTER_SIZE == INT_SIZE
    return fls_llu(x);
#else
#error No implementation for size_t fls()
#endif
}

static inline unsigned fls_u64(uint64_t x)
{
#if LONG_SIZE == 8
    return fls_lu(x);
#elif LONG_LONG_SIZE == 8
    return fls_llu(x);
#else
#error No implementation for 64-bit fls()
#endif
}

static inline unsigned fls_u32(uint32_t x)
{
#if INT_SIZE == 4
    return fls_u(x);
#else
#error No implementation for 32-bit fls()
#endif
}

// 计算二进制位1的个数
static inline unsigned popcount_u(unsigned bitmap)
{
    return INTERNAL_POPCOUNT(bitmap);
}

static inline unsigned popcount_lu(unsigned long bitmap)
{
    return INTERNAL_POPCOUNTL(bitmap);
}

static inline unsigned popcount_llu(unsigned long long bitmap)
{
    return INTERNAL_POPCOUNTLL(bitmap);
}

// 删除第一个未被设置的位
static inline size_t cfs_lu(unsigned long* bitmap)
{
    util_assume(*bitmap != 0);
    size_t bit = ffs_lu(*bitmap);
    *bitmap ^= ZU(1) << bit;
    return bit;
}

// 大于或等于x的最小2的幂
static inline uint64_t pow2_ceil_u64(uint64_t x)
{
    if (unlikely(x <= 1))
    {
        return x;
    }
    size_t msb_on_index = fls_u64(x - 1);
    /*
     * Range-check; it's on the callers to ensure that the result of this
     * call won't overflow.
     */
    assert(msb_on_index < 63);
    return 1ULL << (msb_on_index + 1);
}

static inline uint32_t pow2_ceil_u32(uint32_t x)
{
    if (unlikely(x <= 1))
    {
        return x;
    }
    size_t msb_on_index = fls_u32(x - 1);
    /* As above. */
    assert(msb_on_index < 31);
    return 1U << (msb_on_index + 1);
}

static inline size_t pow2_ceil_zu(size_t x)
{
#if (POINTER_SIZE == 8)
    return pow2_ceil_u64(x);
#else
    return pow2_ceil_u32(x);
#endif
}

// 计算 x 的以 2 为底的对数的向下取整值（即最高有效位的位置）
static inline unsigned lg_floor(size_t x)
{
    util_assume(x != 0);
#if (POINTER_SIZE == 8)
    return fls_u64(x);
#else
    return fls_u32(x);
#endif
}

// 计算 x 的以 2 为底的对数的向上取整值
static inline unsigned lg_ceil(size_t x)
{
    return lg_floor(x) + ((x & (x - 1)) == 0 ? 0 : 1);
}

#define LG_FLOOR_1(x) 0
#define LG_FLOOR_2(x) (x < (1ULL << 1) ? LG_FLOOR_1(x) : 1 + LG_FLOOR_1(x >> 1))
#define LG_FLOOR_4(x) (x < (1ULL << 2) ? LG_FLOOR_2(x) : 2 + LG_FLOOR_2(x >> 2))
#define LG_FLOOR_8(x) (x < (1ULL << 4) ? LG_FLOOR_4(x) : 4 + LG_FLOOR_4(x >> 4))
#define LG_FLOOR_16(x)                                                         \
    (x < (1ULL << 8) ? LG_FLOOR_8(x) : 8 + LG_FLOOR_8(x >> 8))
#define LG_FLOOR_32(x)                                                         \
    (x < (1ULL << 16) ? LG_FLOOR_16(x) : 16 + LG_FLOOR_16(x >> 16))
#define LG_FLOOR_64(x)                                                         \
    (x < (1ULL << 32) ? LG_FLOOR_32(x) : 32 + LG_FLOOR_32(x >> 32))

#if LG2_POINTER_SIZE == 2
#define LG_FLOOR(x) LG_FLOOR_32((x))
#else
#define LG_FLOOR(x) LG_FLOOR_64((x))
#endif

#define LG_CEIL(x) (LG_FLOOR(x) + (((x) & ((x) - 1)) == 0 ? 0 : 1))