/*
 * lmalloc_profile.h - 分配追踪与堆分析API
 *
 * 实时追踪分配/释放事件，支持堆快照、泄漏检测、热点分析，
 * 低开销采样，支持多线程并发，便于导出详细分配日志。
 *
 * 典型用法：
 *   lmalloc_profile_export("profile.json");
 *   // 可用 jeprof/heaptrack/自定义脚本分析 profile.json
 *
 * 线程安全：所有接口均为多线程安全。
 */
#ifndef LMALLOC_PROFILE_H
#define LMALLOC_PROFILE_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct lmalloc_profile_entry_s {
    void* ptr;
    size_t size;
    uint32_t is_alloc; // 1=alloc, 0=free
    uint32_t thread_id;
    time_t timestamp;
    const char* tag; // 可选标签
    void* callstack[8]; // 可选，采样调用栈
    int callstack_depth;
} lmalloc_profile_entry_t;

// 记录一条分配/释放事件（内部接口，供 lmalloc/lfree 调用）
void lmalloc_profile_log_event(const lmalloc_profile_entry_t* e);
// 设置采样率
void lmalloc_profile_set_sample_rate(int rate);

// 获取当前事件总数
size_t lmalloc_profile_events_count(void);
// 拷贝最近的N条事件到buf，返回实际拷贝数
size_t lmalloc_profile_events_copy(lmalloc_profile_entry_t* buf, size_t max);
// 导出全部事件到文件（文本/JSON/二进制）
void lmalloc_profile_export(const char* filename);

#ifdef __cplusplus
}
#endif
#endif // LMALLOC_PROFILE_H 