/*
 * lmalloc.h - 高性能分配器主API头文件
 *
 * 提供高性能、线程安全的内存分配/释放、统计、标签、泄漏检测、profile等接口。
 * 适合多线程/高并发/大规模内存场景。
 *
 * 典型用法：
 *   void* p = lmalloc(128);
 *   lfree(p);
 *   lmalloc_stats_print();
 *   lmalloc_leak_report_print(0);
 *   lmalloc_tagged(256, "session");
 *   lmalloc_profile_export("profile.json");
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 类型声明（暴露给用户）
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>

// 标签统计结构体
typedef struct lmalloc_tag_stat_s {
    _Atomic size_t alloc_count;   // 分配次数
    _Atomic size_t free_count;    // 释放次数
    _Atomic size_t current_bytes; // 当前分配字节数
    _Atomic size_t peak_bytes;    // 峰值分配字节数
} lmalloc_tag_stat_t;

// 泄漏对象信息结构体
typedef struct lmalloc_leak_info_s {
    void* ptr;            // 泄漏对象指针
    size_t size;          // 对象大小
    time_t alloc_time;    // 分配时间
    const char* tag;      // 分配标签
    uint32_t thread_id;   // 分配线程ID
    void* callstack[8];   // 分配时调用栈
    int callstack_depth;  // 调用栈深度
} lmalloc_leak_info_t;

// 分配事件profile结构体
typedef struct lmalloc_profile_entry_s {
    void* ptr;            // 对象指针
    size_t size;          // 分配/释放大小
    uint32_t is_alloc;    // 1=分配, 0=释放
    uint32_t thread_id;   // 线程ID
    time_t timestamp;     // 时间戳
    const char* tag;      // 标签
    void* callstack[8];   // 调用栈
    int callstack_depth;  // 调用栈深度
} lmalloc_profile_entry_t;

// ================== 主分配/释放API ==================
/**
 * @brief 分配指定字节数的内存，类似malloc。
 * @param size 字节数
 * @return 分配成功返回指针，失败返回NULL
 * @note 线程安全，支持高并发。
 */
void* lmalloc(size_t size);

/**
 * @brief 释放由lmalloc/lcalloc/lrealloc/lmemalign分配的内存。
 * @param ptr 待释放指针
 * @note 线程安全，支持高并发。
 */
void lfree(void* ptr);

/**
 * @brief 分配nmemb个size字节的零初始化内存，类似calloc。
 * @param nmemb 元素个数
 * @param size 单个元素字节数
 * @return 分配成功返回指针，失败返回NULL
 * @note 线程安全。
 */
void* lcalloc(size_t nmemb, size_t size);

/**
 * @brief 调整已分配内存大小，类似realloc。
 * @param ptr 原指针
 * @param size 新字节数
 * @return 成功返回新指针，失败返回NULL
 * @note 线程安全。
 */
void* lrealloc(void* ptr, size_t size);

/**
 * @brief 按alignment字节对齐分配size字节内存，类似posix_memalign。
 * @param alignment 对齐字节数（2的幂）
 * @param size 分配字节数
 * @return 成功返回对齐指针，失败返回NULL
 * @note 线程安全。
 */
void* lmemalign(size_t alignment, size_t size);

/**
 * @brief 分配带标签的内存，便于分组统计/归因/热点分析。
 * @param size 字节数
 * @param tag 标签字符串
 * @return 分配成功返回指针，失败返回NULL
 * @note 线程安全。
 */
void* lmalloc_tag(size_t size, const char* tag);

/**
 * @brief 释放由lmalloc_tagged分配的内存。
 * @param ptr 待释放指针
 * @note 线程安全。
 */
void lfree_tag(void* ptr);

// ================== 统计与监控API ==================
/**
 * @brief 打印分配/释放/内存使用等实时统计信息。
 * @note 线程安全。可用于监控/调试。
 */
void lmalloc_stats_print(void);

/**
 * @brief 打印当前分配器堆的快照。
 * @note 线程安全。
 */
void lmalloc_heap_dump(void);

/**
 * @brief 根据统计自动调优分配器参数（如tcache/slab/decay等）。
 * @note 线程安全。可定期调用。
 */
void lmalloc_auto_tune(void);

/**
 * @brief 导出Prometheus风格metrics到文件，便于监控系统采集。
 * @param filename 输出文件名
 * @note 线程安全。
 */
void lmalloc_metrics_export_prometheus(const char* filename);

// ================== 标签分配与分组统计API ==================
/**
 * @brief 查询指定标签的统计信息。
 * @param tag 标签
 * @param out 输出统计结构体
 * @return 成功返回1，失败返回0
 * @note 线程安全。
 */
int lmalloc_tag_stats(const char* tag, lmalloc_tag_stat_t* out);

/**
 * @brief 打印所有标签的统计信息。
 * @note 线程安全。
 */
void lmalloc_tag_stats_print(void);

/**
 * @brief 导出所有标签统计到文件（JSON/文本）。
 * @param filename 输出文件名
 * @note 线程安全。
 */
void lmalloc_tag_stats_export(const char* filename);

// ================== 泄漏检测与报告API ==================
/**
 * @brief 扫描所有活跃分配，返回可疑泄漏对象数。
 * @param buf 输出泄漏对象数组
 * @param max 最大扫描数
 * @param min_age_sec 最小存活时间（秒）
 * @return 实际发现的泄漏对象数
 * @note 线程安全。
 */
size_t lmalloc_leak_scan(lmalloc_leak_info_t* buf, size_t max, time_t min_age_sec);

/**
 * @brief 打印泄漏报告。
 * @param min_age_sec 最小存活时间（秒）
 * @note 线程安全。
 */
void lmalloc_leak_report_print(time_t min_age_sec);

/**
 * @brief 导出泄漏报告到文件（JSON/文本）。
 * @param filename 输出文件名
 * @param min_age_sec 最小存活时间（秒）
 * @note 线程安全。
 */
void lmalloc_leak_report_export(const char* filename, time_t min_age_sec);

// ================== 分配追踪与堆分析API ==================
/**
 * @brief 获取当前分配/释放事件总数。
 * @return 事件数
 * @note 线程安全。
 */
size_t lmalloc_profile_events_count(void);

/**
 * @brief 拷贝最近的N条分配/释放事件到buf。
 * @param buf 输出事件数组
 * @param max 最大拷贝数
 * @return 实际拷贝数
 * @note 线程安全。
 */
size_t lmalloc_profile_events_copy(lmalloc_profile_entry_t* buf, size_t max);

/**
 * @brief 导出全部分配/释放事件到文件（JSON/文本/二进制）。
 * @param filename 输出文件名
 * @note 线程安全。
 */
void lmalloc_profile_export(const char* filename);

// ================== 兼容标准接口 ==================
/**
 * @brief mallopt兼容接口，部分参数支持。
 */
int my_mallopt(int param, int value);

/**
 * @brief 查询分配块实际可用大小，类似malloc_usable_size。
 * @param ptr 分配指针
 * @return 实际可用字节数
 */
size_t my_malloc_usable_size(void* ptr);

/**
 * @brief 打印分配器统计信息，兼容glibc malloc_stats。
 */
void my_malloc_stats(void);
#define mallopt my_mallopt
#define malloc_usable_size my_malloc_usable_size
#define malloc_stats my_malloc_stats

// 功能开关API
void lmalloc_enable_leak(bool enable);
void lmalloc_enable_stats(bool enable);
void lmalloc_enable_profile(bool enable);
void lmalloc_enable_tag(bool enable);
void lmalloc_enable_trace(bool enable);

#ifdef __cplusplus
}
#endif