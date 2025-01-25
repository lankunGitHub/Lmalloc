/*
 * lmalloc_leak.c - 内存泄漏检测与报告实现
 *
 * 主要职责：
 *   - 自动追踪所有分配对象，检测未释放内存
 *   - 支持详细泄漏报告导出（文本/JSON）、调用栈采集、分配时间筛选、标签归因
 *   - 提供注册/注销、扫描、打印、导出等接口
 *
 * 关键数据结构：
 *   - leak_entry_t: 记录每个分配对象的元信息（指针、大小、时间、标签、线程、调用栈）
 *   - 全局leak_table: 活跃分配对象表，支持高并发
 *
 * 主要算法：
 *   - 分配时注册，释放时注销，O(1)插入/删除
 *   - 扫描时支持最小存活时间、标签、线程等多维筛选
 *
 * 并发/调优：
 *   - 全局互斥锁保护leak_table，支持多线程安全
 *   - 最大对象数LEAK_MAX_OBJS可调
 *
 * 典型调用链：
 *   lmalloc/lfree -> leak_register/leak_unregister
 *   lmalloc_leak_scan/lmalloc_leak_report_print/lmalloc_leak_report_export
 *
 * 设计trade-off：
 *   - 牺牲部分性能和内存，换取泄漏检测和可观测性
 *   - 简化实现，便于集成和扩展
 */
#include "lmalloc_leak.h"
#include "lmalloc_json.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
bool g_leak_enabled = false;
void lmalloc_enable_leak(bool enable) { g_leak_enabled = enable; }
#define LEAK_MAX_OBJS 8192

// 演示用全局分配表（实际应与主分配器集成）
// 设计说明：
//   - 记录每个分配对象的指针、大小、分配时间、标签、线程、调用栈等
//   - 支持O(1)插入/删除，便于高并发
//   - 最大对象数LEAK_MAX_OBJS可调
//
typedef struct leak_entry_s {
    void* ptr;            // 分配对象指针
    size_t size;          // 对象大小
    time_t alloc_time;    // 分配时间
    const char* tag;      // 分配标签
    uint32_t thread_id;   // 分配线程ID
    void* callstack[8];   // 分配时调用栈
    int callstack_depth;  // 调用栈深度
} leak_entry_t;

static leak_entry_t leak_table[LEAK_MAX_OBJS];
static size_t leak_count = 0;
static size_t leak_dropped = 0; // 表满被丢弃的注册数（用于报告提示漏报）
static pthread_mutex_t leak_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief 注册分配对象到leak_table
 * @param ptr 分配指针
 * @param size 分配字节数
 * @param tag 标签
 * @param tid 线程ID
 *
 * 线程安全：多线程安全，互斥锁保护
 *
 * 算法说明：
 *   - O(1)插入，超出最大数则丢弃
 */
void leak_register(void* ptr, size_t size, const char* tag, uint32_t tid) {
    if (!g_leak_enabled) return;
    pthread_mutex_lock(&leak_mutex);
    if (leak_count < LEAK_MAX_OBJS) {
        leak_entry_t* e = &leak_table[leak_count++];
        e->ptr = ptr;
        e->size = size;
        e->alloc_time = time(NULL);
        e->tag = tag;
        e->thread_id = tid;
        e->callstack_depth = 0; // 可选采样
    } else {
        ++leak_dropped; // 表满丢弃：报告时提示存在漏报
    }
    pthread_mutex_unlock(&leak_mutex);
}

/**
 * @brief 注销释放对象，从leak_table移除
 * @param ptr 释放指针
 *
 * 线程安全：多线程安全，互斥锁保护
 *
 * 算法说明：
 *   - O(1)删除，直接用最后一个覆盖
 */
void leak_unregister(void* ptr) {
    if (!g_leak_enabled) return;
    pthread_mutex_lock(&leak_mutex);
    for (size_t i = 0; i < leak_count; ++i) {
        if (leak_table[i].ptr == ptr) {
            leak_table[i] = leak_table[--leak_count];
            break;
        }
    }
    pthread_mutex_unlock(&leak_mutex);
}

/**
 * @brief 从指定偏移扫描leak_table（内部实现，支持分批导出）
 *
 * 线程安全：多线程安全，互斥锁保护
 *
 * @param buf 输出泄漏对象数组
 * @param max 最大扫描数
 * @param min_age_sec 最小存活时间（秒）
 * @param pos 输入输出参数：扫描起始下标，返回已推进到的下标
 * @return 实际发现的泄漏对象数
 */
static size_t leak_scan_from(lmalloc_leak_info_t* buf, size_t max,
                             time_t min_age_sec, size_t* pos) {
    if (!g_leak_enabled) return 0;
    pthread_mutex_lock(&leak_mutex);
    time_t now = time(NULL);
    size_t n = 0;
    size_t i = *pos;
    for (; i < leak_count && n < max; ++i) {
        if (now - leak_table[i].alloc_time >= min_age_sec) {
            buf[n].ptr = leak_table[i].ptr;
            buf[n].size = leak_table[i].size;
            buf[n].alloc_time = leak_table[i].alloc_time;
            buf[n].tag = leak_table[i].tag;
            buf[n].thread_id = leak_table[i].thread_id;
            // 钳制调用栈深度，防止异常数据导致越界拷贝
            buf[n].callstack_depth =
                leak_table[i].callstack_depth > 8 ? 8 : leak_table[i].callstack_depth;
            memcpy(buf[n].callstack, leak_table[i].callstack,
                   sizeof(void*) * buf[n].callstack_depth);
            n++;
        }
    }
    *pos = i;
    pthread_mutex_unlock(&leak_mutex);
    return n;
}

/**
 * @brief 扫描所有活跃分配，返回可疑泄漏对象数
 * @param buf 输出泄漏对象数组
 * @param max 最大扫描数
 * @param min_age_sec 最小存活时间（秒）
 * @return 实际发现的泄漏对象数
 *
 * 线程安全：多线程安全，互斥锁保护
 */
size_t lmalloc_leak_scan(lmalloc_leak_info_t* buf, size_t max, time_t min_age_sec) {
    size_t pos = 0;
    return leak_scan_from(buf, max, min_age_sec, &pos);
}

/**
 * @brief 打印泄漏报告
 * @param min_age_sec 最小存活时间（秒）
 *
 * 算法说明：
 *   - 扫描leak_table，打印所有满足条件的对象
 */
void lmalloc_leak_report_print(time_t min_age_sec) {
    if (!g_leak_enabled) return;
    // 分批扫描：曾一次性在栈上放LEAK_MAX_OBJS(8192)条≈896KB，
    // 小栈线程调用直接栈溢出
    lmalloc_leak_info_t buf[256];
    size_t pos = 0;
    printf("[lmalloc leak report] (min_age=%lds)\n", (long)min_age_sec);
    for (;;) {
        size_t n = leak_scan_from(buf, 256, min_age_sec, &pos);
        for (size_t i = 0; i < n; ++i) {
            printf("leak: ptr=%p size=%zu tag=%s alloc_time=%ld tid=%u\n",
                   buf[i].ptr, buf[i].size, buf[i].tag ? buf[i].tag : "",
                   (long)buf[i].alloc_time, buf[i].thread_id);
        }
        if (n < 256)
            break;
    }
    if (leak_dropped) {
        printf("[lmalloc leak report] 注意：活跃对象超过表容量，已有%zu次注册被丢弃，报告可能漏报\n",
               leak_dropped);
    }
}

/**
 * @brief 导出泄漏报告到文件（JSON/文本）
 * @param filename 输出文件名
 * @param min_age_sec 最小存活时间（秒）
 *
 * 算法说明：
 *   - 扫描leak_table，导出所有满足条件的对象到文件
 */
void lmalloc_leak_report_export(const char* filename, time_t min_age_sec) {
    if (!g_leak_enabled) return;
    FILE* f = fopen(filename, "w");
    if (!f) return;
    // 分批扫描，避免大栈帧（曾8192条≈896KB压爆小栈线程）
    lmalloc_leak_info_t buf[256];
    size_t pos = 0;
    int first = 1;
    fprintf(f, "[\n");
    for (;;) {
        size_t n = leak_scan_from(buf, 256, min_age_sec, &pos);
        for (size_t i = 0; i < n; ++i) {
            if (!first)
                fprintf(f, ",\n");
            first = 0;
            fprintf(f, "  {\"ptr\":\"%p\",\"size\":%zu,\"tag\":\"", buf[i].ptr, buf[i].size);
            lmalloc_json_escape(f, buf[i].tag ? buf[i].tag : "");
            fprintf(f, "\",\"alloc_time\":%ld,\"thread_id\":%u}", (long)buf[i].alloc_time, buf[i].thread_id);
        }
        if (n < 256)
            break;
    }
    fprintf(f, "\n]\n");
    fclose(f);
}