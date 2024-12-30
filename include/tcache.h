#pragma once

#include "tcache_struct.h"

typedef struct tsd_s tsd_t;
typedef unsigned szind_t;

// 初始化 tcache（线程本地缓存）
void tcache_boot(void);

// 获取当前线程的 tcache 指针
tcache_t* tsd_tcachep_get(tsd_t* tsd);

// 从 tcache 分配一个小块
void* tcache_alloc(tsd_t* tsd, size_t size, szind_t ind);

// 回收一个小块到 tcache
void tcache_dalloc(tsd_t* tsd, void* ptr, size_t size, szind_t ind);

// 跨线程回收
void tcache_handoff(void* ptr, size_t size, int binind);

// tcache flush批量回收
void tcache_flush_all_bins(tcache_t* tcache);

// ================== tcache 统计、调试接口 ==================
// 打印tcache整体统计信息
void tcache_stats_print(tcache_t* tcache);
// 导出tcache bin详细统计为CSV
void tcache_bin_stats_export_csv(const char* filename);