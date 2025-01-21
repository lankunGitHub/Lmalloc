/*
 * lmalloc_stats.h - 分配器统计与监控API
 *
 * 提供分配/释放/内存使用等实时统计，支持自动调优、Prometheus风格metrics导出，
 * 便于与监控系统集成和线上自愈。
 *
 * 典型用法：
 *   lmalloc_stats_print();
 *   lmalloc_auto_tune();
 *   lmalloc_metrics_export_prometheus("lmalloc_metrics.prom");
 *
 * 线程安全：所有统计均为多线程安全。
 * 注意事项：可结合定时任务/信号/后台线程自动调用。
 */
#ifndef LMALLOC_STATS_H
#define LMALLOC_STATS_H
#include <stddef.h>
#include <stdatomic.h>

struct lmalloc_stats_s {
    _Atomic size_t alloc_count;
    _Atomic size_t free_count;
    _Atomic size_t current_bytes;
    _Atomic size_t peak_bytes;
};
extern struct lmalloc_stats_s g_lmalloc_stats;

// 自动调优接口
void lmalloc_auto_tune(void);
// Prometheus风格metrics导出
void lmalloc_metrics_export_prometheus(const char* filename);
// 分配器整体统计打印（对外主接口）
void lmalloc_stats_print(void);
// 堆快照导出/对比
void lmalloc_heap_dump(void);
void lmalloc_heap_snapshot_diff(const char* file1, const char* file2);
// arena/bin/slab/pac统计导出为JSON
void lmalloc_stats_export_json(const char* filename);
// 注册SIGUSR2堆dump信号处理器（处理器内只置标志）
void lmalloc_debug_signal_init(void);
// 在安全上下文（分配路径）轮询信号请求并执行堆dump
void lmalloc_debug_signal_poll(void);
#endif