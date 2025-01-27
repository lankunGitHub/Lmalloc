/*
 * lmalloc_profile.c - 分配追踪与堆分析实现
 *
 * 主要职责：
 *   - 实时追踪分配/释放事件，支持堆快照、泄漏检测、热点分析
 *   - 低开销采样，支持多线程并发，便于导出详细分配日志
 *   - 提供事件采样、导出、Top-N分析等接口
 *
 * 关键数据结构：
 *   - lmalloc_profile_entry_t: 记录每次分配/释放事件的元信息
 *   - profile_ring: 环形缓冲区，存储最近的事件
 *
 * 主要算法：
 *   - 环形缓冲区高效存储事件，支持并发写入
 *   - 支持采样率调整、按tag/线程/大小/时间等多维过滤导出
 *   - Top-N热点分析，便于定位内存热点
 *
 * 并发/调优：
 *   - 全局互斥锁保护profile_ring，支持多线程安全
 *   - 采样率可调，兼顾性能与可观测性
 *
 * 典型调用链：
 *   lmalloc/lfree等 -> lmalloc_profile_log_event
 *   lmalloc_profile_export/lmalloc_profile_events_copy等
 *
 * 设计trade-off：
 *   - 牺牲部分事件丢失，换取低开销和高并发
 *   - 支持多种导出/分析方式，便于集成和扩展
 */
#include "lmalloc_profile.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define PROFILE_RING_SIZE 4096

static lmalloc_profile_entry_t profile_ring[PROFILE_RING_SIZE];
static size_t profile_head = 0;
static pthread_mutex_t profile_mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t profile_count = 0;

static int g_profile_sample_rate = 1; // 每N次分配采样一次，默认全采样
void lmalloc_profile_set_sample_rate(int rate) { g_profile_sample_rate = rate > 0 ? rate : 1; }

bool g_profile_enabled = false;
void lmalloc_enable_profile(bool enable) { g_profile_enabled = enable; }

// 过滤导出profile（支持tag/线程/大小/时间区间）
void lmalloc_profile_export_filtered_json(const char* filename, const char* tag, int thread_id, size_t min_size, size_t max_size, time_t start_time, time_t end_time) {
    if (!g_profile_enabled) return;
    FILE* f = fopen(filename, "w");
    if (!f) return;
    fprintf(f, "[\n");
    size_t n = lmalloc_profile_events_count();
    lmalloc_profile_entry_t* buf = malloc(n * sizeof(lmalloc_profile_entry_t));
    n = lmalloc_profile_events_copy(buf, n);
    int first = 1;
    for (size_t i = 0; i < n; ++i) {
        lmalloc_profile_entry_t* e = &buf[i];
        if (tag && e->tag && strcmp(e->tag, tag) != 0) continue;
        if (thread_id >= 0 && (int)e->thread_id != thread_id) continue;
        if (min_size && e->size < min_size) continue;
        if (max_size && e->size > max_size) continue;
        if (start_time && e->timestamp < start_time) continue;
        if (end_time && e->timestamp > end_time) continue;
        if (!first) fprintf(f, ",\n");
        first = 0;
        fprintf(f, "  {\"ptr\":\"%p\",\"size\":%zu,\"is_alloc\":%u,\"thread_id\":%u,\"timestamp\":%ld,\"tag\":\"%s\",\"callstack\":[",
            e->ptr, e->size, e->is_alloc, e->thread_id, (long)e->timestamp, e->tag ? e->tag : "");
        for (int j = 0; j < e->callstack_depth; ++j) {
            fprintf(f, "\"%p\"%s", e->callstack[j], (j+1==e->callstack_depth)?"":", ");
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n]\n");
    free(buf);
    fclose(f);
}

// profile事件采样（在lmalloc/lfree等调用）
static int g_profile_event_count = 0;
void lmalloc_profile_log_event_sampled(lmalloc_profile_entry_t* e) {
    if (!g_profile_enabled) return;
    if ((g_profile_event_count++ % g_profile_sample_rate) == 0) {
        lmalloc_profile_log_event(e);
    }
}

void lmalloc_profile_log_event(const lmalloc_profile_entry_t* e) {
    if (!g_profile_enabled) return;
    pthread_mutex_lock(&profile_mutex);
    profile_ring[profile_head] = *e;
    profile_head = (profile_head + 1) % PROFILE_RING_SIZE;
    if (profile_count < PROFILE_RING_SIZE) ++profile_count;
    pthread_mutex_unlock(&profile_mutex);
}

size_t lmalloc_profile_events_count(void) {
    if (!g_profile_enabled) return 0;
    pthread_mutex_lock(&profile_mutex);
    size_t n = profile_count;
    pthread_mutex_unlock(&profile_mutex);
    return n;
}

size_t lmalloc_profile_events_copy(lmalloc_profile_entry_t* buf, size_t max) {
    if (!g_profile_enabled) return 0;
    pthread_mutex_lock(&profile_mutex);
    size_t n = (profile_count < max) ? profile_count : max;
    size_t start = (profile_head + PROFILE_RING_SIZE - n) % PROFILE_RING_SIZE;
    for (size_t i = 0; i < n; ++i) {
        buf[i] = profile_ring[(start + i) % PROFILE_RING_SIZE];
    }
    pthread_mutex_unlock(&profile_mutex);
    return n;
}

// C API: 导出所有profile事件到用户buffer，返回实际数量
size_t lmalloc_profile_events_copy_to_user(lmalloc_profile_entry_t* buf, size_t max) {
    if (!g_profile_enabled) return 0;
    return lmalloc_profile_events_copy(buf, max);
}

// Top-N分配热点分析（按tag聚合）
typedef struct {
    const char* tag;
    size_t total_size;
    size_t count;
} tag_stat_t;

int lmalloc_profile_topN_tags(tag_stat_t* out, size_t maxN) {
    if (!g_profile_enabled) return 0;
    lmalloc_profile_entry_t* buf = malloc(PROFILE_RING_SIZE * sizeof(lmalloc_profile_entry_t));
    size_t n = lmalloc_profile_events_copy(buf, PROFILE_RING_SIZE);
    size_t n_tags = 0;
    for (size_t i = 0; i < n; ++i) {
        const char* tag = buf[i].tag ? buf[i].tag : "(null)";
        size_t j;
        for (j = 0; j < n_tags; ++j) {
            if (strcmp(out[j].tag, tag) == 0) break;
        }
        if (j == n_tags && n_tags < maxN) {
            out[n_tags].tag = tag;
            out[n_tags].total_size = 0;
            out[n_tags].count = 0;
            n_tags++;
        }
        if (j < maxN) {
            out[j].total_size += buf[i].size;
            out[j].count++;
        }
    }
    free(buf);
    // 按total_size降序排序
    for (size_t i = 0; i + 1 < n_tags; ++i) {
        for (size_t j = i + 1; j < n_tags; ++j) {
            if (out[j].total_size > out[i].total_size) {
                tag_stat_t tmp = out[i]; out[i] = out[j]; out[j] = tmp;
            }
        }
    }
    return n_tags;
}

// Top-N分配热点分析（按线程聚合）
typedef struct {
    int thread_id;
    size_t total_size;
    size_t count;
} thread_stat_t;

int lmalloc_profile_topN_threads(thread_stat_t* out, size_t maxN) {
    if (!g_profile_enabled) return 0;
    lmalloc_profile_entry_t* buf = malloc(PROFILE_RING_SIZE * sizeof(lmalloc_profile_entry_t));
    size_t n = lmalloc_profile_events_copy(buf, PROFILE_RING_SIZE);
    size_t n_threads = 0;
    for (size_t i = 0; i < n; ++i) {
        int tid = buf[i].thread_id;
        size_t j;
        for (j = 0; j < n_threads; ++j) {
            if (out[j].thread_id == tid) break;
        }
        if (j == n_threads && n_threads < maxN) {
            out[n_threads].thread_id = tid;
            out[n_threads].total_size = 0;
            out[n_threads].count = 0;
            n_threads++;
        }
        if (j < maxN) {
            out[j].total_size += buf[i].size;
            out[j].count++;
        }
    }
    free(buf);
    // 按total_size降序排序
    for (size_t i = 0; i + 1 < n_threads; ++i) {
        for (size_t j = i + 1; j < n_threads; ++j) {
            if (out[j].total_size > out[i].total_size) {
                thread_stat_t tmp = out[i]; out[i] = out[j]; out[j] = tmp;
            }
        }
    }
    return n_threads;
}

void lmalloc_profile_export(const char* filename) {
    if (!g_profile_enabled) return;
    pthread_mutex_lock(&profile_mutex);
    FILE* f = fopen(filename, "w");
    if (!f) { pthread_mutex_unlock(&profile_mutex); return; }
    fprintf(f, "[\n");
    for (size_t i = 0; i < profile_count; ++i) {
        lmalloc_profile_entry_t* e = &profile_ring[(profile_head + PROFILE_RING_SIZE - profile_count + i) % PROFILE_RING_SIZE];
        fprintf(f, "  {\"ptr\":%p,\"size\":%zu,\"is_alloc\":%u,\"thread_id\":%u,\"timestamp\":%ld,\"tag\":\"%s\"}%s\n",
            e->ptr, e->size, e->is_alloc, e->thread_id, (long)e->timestamp, e->tag ? e->tag : "", (i+1==profile_count)?"":" ,");
    }
    fprintf(f, "]\n");
    fclose(f);
    pthread_mutex_unlock(&profile_mutex);
}

void lmalloc_profile_export_csv(const char* filename) {
    if (!g_profile_enabled) return;
    FILE* f = fopen(filename, "w");
    if (!f) return;
    fprintf(f, "ptr,size,is_alloc,thread_id,timestamp,tag,callstack\n");
    size_t n = lmalloc_profile_events_count();
    lmalloc_profile_entry_t* buf = malloc(n * sizeof(lmalloc_profile_entry_t));
    n = lmalloc_profile_events_copy(buf, n);
    for (size_t i = 0; i < n; ++i) {
        fprintf(f, "%p,%zu,%u,%u,%ld,%s,\"", buf[i].ptr, buf[i].size, buf[i].is_alloc, buf[i].thread_id, (long)buf[i].timestamp, buf[i].tag ? buf[i].tag : "");
        for (int j = 0; j < buf[i].callstack_depth; ++j) {
            fprintf(f, "%p%s", buf[i].callstack[j], (j+1==buf[i].callstack_depth)?"":";");
        }
        fprintf(f, "\"\n");
    }
    free(buf);
    fclose(f);
}

void lmalloc_profile_export_json(const char* filename) {
    if (!g_profile_enabled) return;
    FILE* f = fopen(filename, "w");
    if (!f) return;
    fprintf(f, "[\n");
    size_t n = lmalloc_profile_events_count();
    lmalloc_profile_entry_t* buf = malloc(n * sizeof(lmalloc_profile_entry_t));
    n = lmalloc_profile_events_copy(buf, n);
    for (size_t i = 0; i < n; ++i) {
        fprintf(f, "  {\"ptr\":\"%p\",\"size\":%zu,\"is_alloc\":%u,\"thread_id\":%u,\"timestamp\":%ld,\"tag\":\"%s\",\"callstack\":[",
            buf[i].ptr, buf[i].size, buf[i].is_alloc, buf[i].thread_id, (long)buf[i].timestamp, buf[i].tag ? buf[i].tag : "");
        for (int j = 0; j < buf[i].callstack_depth; ++j) {
            fprintf(f, "\"%p\"%s", buf[i].callstack[j], (j+1==buf[i].callstack_depth)?"":", ");
        }
        fprintf(f, "]}%s\n", (i+1==n)?"":" ,");
    }
    free(buf);
    fprintf(f, "]\n");
    fclose(f);
} 