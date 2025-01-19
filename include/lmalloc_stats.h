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
#endif 