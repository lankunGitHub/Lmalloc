/*
 * lmalloc_leak.h - 内存泄漏检测与报告API
 *
 * 自动追踪所有分配对象，检测未释放内存，支持详细报告导出（文本/JSON），
 * 支持调用栈采集、分配时间筛选、标签归因。
 * 线程安全，适合高并发场景。
 *
 * 典型用法：
 *   void* p = lmalloc(128);
 *   // ...
 *   lmalloc_leak_report_print(0);
 *   lmalloc_leak_report_export("leak_report.json", 0);
 */
#ifndef LMALLOC_LEAK_H
#define LMALLOC_LEAK_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct lmalloc_leak_info_s {
    void* ptr;
    size_t size;
    time_t alloc_time;
    const char* tag;
    uint32_t thread_id;
    void* callstack[8];
    int callstack_depth;
} lmalloc_leak_info_t;

// 分配/释放时注册/注销活跃对象（内部接口，供 lmalloc/lfree 调用）
void leak_register(void* ptr, size_t size, const char* tag, uint32_t tid);
void leak_unregister(void* ptr);

// 扫描所有活跃分配，返回可疑泄漏对象数
size_t lmalloc_leak_scan(lmalloc_leak_info_t* buf, size_t max, time_t min_age_sec);
// 打印泄漏报告
void lmalloc_leak_report_print(time_t min_age_sec);
// 导出泄漏报告到文件（文本/JSON）
void lmalloc_leak_report_export(const char* filename, time_t min_age_sec);

#ifdef __cplusplus
}
#endif
#endif // LMALLOC_LEAK_H 