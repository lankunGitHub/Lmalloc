#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "memory/lmalloc.h"

// 全局trace开关，默认关闭
bool g_trace_enabled = false;
void lmalloc_enable_trace(bool enable) { g_trace_enabled = enable; }

// 追踪事件结构体
typedef struct {
    void* ptr;
    size_t size;
    const char* op; // "alloc"/"free"/"realloc"
    uint32_t thread_id;
    time_t timestamp;
} lmalloc_trace_event_t;

// 简单trace事件记录（可扩展为环形缓冲区/文件导出等）
void lmalloc_trace_log_event(const char* op, void* ptr, size_t size) {
    lmalloc_trace_event_t e = {
        .ptr = ptr,
        .size = size,
        .op = op,
        .thread_id = (uint32_t)pthread_self(),
        .timestamp = time(NULL)
    };
    // 这里只做简单打印，实际可扩展为写文件/缓冲区等
    printf("[lmalloc-trace] %s ptr=%p size=%zu tid=%u time=%ld\n", e.op, e.ptr, e.size, e.thread_id, e.timestamp);
} 