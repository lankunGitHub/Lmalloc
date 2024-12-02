#pragma once
#include <stdint.h>
#include <stdbool.h>
#if defined(__cplusplus)
    #include <atomic>
    // C++下直接用std::atomic<T>，不再定义ATOMIC_VAR等宏
#else
    #include <stdatomic.h>
    // C下直接用_Atomic T，不再定义ATOMIC_VAR等宏
    typedef _Atomic uint8_t atomic_u8_t;
    typedef _Atomic void* atomic_p_t;
    static inline uint8_t atomic_load_u8(const atomic_u8_t* a, int mo) { return atomic_load_explicit(a, mo); }
    static inline void atomic_store_b(_Atomic bool* a, bool val, int mo) { atomic_store_explicit(a, val, mo); }
    static inline bool atomic_load_b(const _Atomic bool* a, int mo) { return atomic_load_explicit(a, mo); }
#endif
#ifndef ATOMIC_RELAXED
#define ATOMIC_RELAXED memory_order_relaxed
#endif
#ifndef ATOMIC_ACQUIRE
#define ATOMIC_ACQUIRE memory_order_acquire
#endif
#ifndef ATOMIC_RELEASE
#define ATOMIC_RELEASE memory_order_release
#endif
#ifndef ATOMIC_ACQ_REL
#define ATOMIC_ACQ_REL memory_order_acq_rel
#endif
#ifndef ATOMIC_SEQ_CST
#define ATOMIC_SEQ_CST memory_order_seq_cst
#endif
#ifndef ATOMIC_VAR
#if defined(__cplusplus)
    #define ATOMIC_VAR(T) std::atomic<T>
#else
    #define ATOMIC_VAR(T) _Atomic T
#endif
#endif
