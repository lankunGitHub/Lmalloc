#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdbool.h>
#include "arena.h"
#include "arena_struct.h"
#include "bin.h"
#include "bin_struct.h"
#include "lmalloc_inline.h"
#include "lmalloc_stats.h"
#include "edata.h"


bool g_stats_enabled = false;
void lmalloc_enable_stats(bool enable) { g_stats_enabled = enable; }
struct lmalloc_stats_s g_lmalloc_stats = {0};

extern arena_t* arenas[];
extern unsigned n_arenas;
extern struct numa_stats_s g_numa_stats;
extern struct pac_stats_s g_pac_stats;
extern void pac_meta_export_csv(const char* filename);
extern void slab_region_stats_export_csv(const char* filename);
extern void bin_slab_migrate_stats_export_csv(const char* filename);
extern void lmalloc_profile_export_json(const char* filename);
extern void lmalloc_profile_export_csv(const char* filename);

#ifdef LMALLOC_PROFILE
#include <pthread.h>
#include <unistd.h>

static int g_profile_export_interval = 30; // profile导出周期（秒）
static int g_profile_export_running = 0;
static pthread_t g_profile_export_thread;

void* profile_export_thread_func(void* arg) {
    (void)arg;
    while (g_profile_export_running) {
        sleep(g_profile_export_interval);
        lmalloc_profile_export_json("profile_auto.json");
        lmalloc_profile_export_csv("profile_auto.csv");
        lmalloc_heap_dump();
        printf("[profile_export] 自动导出profile/heap dump\n");
    }
    return NULL;
}

void profile_export_thread_start(int interval_sec) {
    g_profile_export_interval = interval_sec;
    g_profile_export_running = 1;
    pthread_create(&g_profile_export_thread, NULL, profile_export_thread_func, NULL);
    printf("[profile_export] 后台profile/heap dump导出线程已启动，周期=%ds\n", interval_sec);
}

void profile_export_thread_stop() {
    g_profile_export_running = 0;
    pthread_join(g_profile_export_thread, NULL);
    printf("[profile_export] 后台profile/heap dump导出线程已停止\n");
}
#endif

// slab/region 生命周期导出为CSV
void lmalloc_slab_region_lifetime_export_csv(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;
    fprintf(f, "arena,bin,slab,region,alloc_time,free_time,live_sec\n");
    for (unsigned a = 0; a < n_arenas; ++a) {
        arena_t* arena = arenas[a];
        for (unsigned b = 0; b < arena->n_bins; ++b) {
            bin_t* bin = &arena->bins[b];
            slab_t* slab = (slab_t*)bin->slabcur;
            if (slab) {
                for (size_t r = 0; r < slab->edata.nregions; ++r) {
                    region_aux_meta_t* meta = &slab->edata.region_aux_arr[r];
                    if (meta->alloc_time) {
                        time_t free_time = meta->type == 0 ? time(NULL) : 0;
                        long live_sec = free_time ? (free_time - meta->alloc_time) : 0;
                        fprintf(f, "%u,%u,%p,%zu,%u,%ld,%ld\n", a, b, slab, r, meta->alloc_time, free_time, live_sec);
                    }
                }
            }
        }
    }
    fclose(f);
}

// 导出所有slab/region的详细统计
void slab_region_stats_export_csv(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;
    fprintf(f, "arena,bin,slab,region,alloc_count,free_count,alloc_time,free_time,alloc_tid,free_tid\n");
    for (unsigned a = 0; a < n_arenas; ++a) {
        arena_t* arena = arenas[a];
        for (unsigned b = 0; b < arena->n_bins; ++b) {
            bin_t* bin = &arena->bins[b];
            // slabcur
            slab_t* slab = (slab_t*)bin->slabcur;
            if (slab) {
                for (size_t r = 0; r < slab->edata.nregions; ++r) {
                    region_aux_meta_t* meta = &slab->edata.region_aux_arr[r];
                    fprintf(f, "%u,%u,%p,%zu,%u,%u,%u,%u,%u,%u\n", a, b, slab, r, meta->alloc_count, meta->free_count, meta->alloc_time, meta->free_time, meta->alloc_tid, meta->free_tid);
                }
            }
            // unfull slabs
            slab = (slab_t*)bin->unfull_slabs;
            while (slab) {
                for (size_t r = 0; r < slab->edata.nregions; ++r) {
                    region_aux_meta_t* meta = &slab->edata.region_aux_arr[r];
                    fprintf(f, "%u,%u,%p,%zu,%u,%u,%u,%u,%u,%u\n", a, b, slab, r, meta->alloc_count, meta->free_count, meta->alloc_time, meta->free_time, meta->alloc_tid, meta->free_tid);
                }
                slab = slab->link.qre_next;
            }
            // full slabs
            slab = (slab_t*)bin->full_slabs.qlh_first;
            while (slab) {
                for (size_t r = 0; r < slab->edata.nregions; ++r) {
                    region_aux_meta_t* meta = &slab->edata.region_aux_arr[r];
                    fprintf(f, "%u,%u,%p,%zu,%u,%u,%u,%u,%u,%u\n", a, b, slab, r, meta->alloc_count, meta->free_count, meta->alloc_time, meta->free_time, meta->alloc_tid, meta->free_tid);
                }
                slab = slab->link.qre_next;
            }
        }
    }
    fclose(f);
}

// arena/bin/slab/pac统计导出为JSON
void lmalloc_stats_export_json(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;
    fprintf(f, "{\n  \"arenas\": [\n");
    for (unsigned a = 0; a < n_arenas; ++a) {
        arena_t* arena = arenas[a];
        fprintf(f, "    {\"arena_id\":%u,\"alloc_count\":%zu,\"free_count\":%zu,\"current_bytes\":%zu,\"peak_bytes\":%zu,\"bins\":[", a, arena->alloc_count, arena->free_count, arena->current_bytes, arena->peak_bytes);
        for (unsigned b = 0; b < arena->n_bins; ++b) {
            bin_t* bin = &arena->bins[b];
            fprintf(f, "{\"bin_id\":%u,\"alloc_count\":%zu,\"free_count\":%zu,\"current_bytes\":%zu,\"peak_bytes\":%zu}", b, bin->alloc_count, bin->free_count, bin->current_bytes, bin->peak_bytes);
            if (b + 1 < arena->n_bins) fprintf(f, ",");
        }
        fprintf(f, "]}");
        if (a + 1 < n_arenas) fprintf(f, ",\n");
    }
    fprintf(f, "\n  ],\n  \"pac\": {\"alloc_count\":%zu,\"free_count\":%zu,\"current_bytes\":%zu,\"peak_bytes\":%zu}\n}\n",
        atomic_fetch_add(&(g_pac_stats.alloc_count), 0),
        atomic_fetch_add(&(g_pac_stats.free_count), 0),
        atomic_fetch_add(&(g_pac_stats.current_bytes), 0),
        atomic_fetch_add(&(g_pac_stats.peak_bytes), 0));
    fclose(f);
}

void lmalloc_heap_dump(void) {
    printf("========== lmalloc heap dump =========\n");
    for (unsigned a = 0; a < n_arenas; ++a) {
        arena_t* arena = arenas[a];
        printf("[arena %u] n_threads=%u\n", arena->arena_id, arena->n_threads);
        // extent decay 状态
        int extent_count = 0;
        size_t extent_bytes = 0;
        extent_node_t* enode = arena->extent_free_list;
        while (enode) {
            extent_count++;
            extent_bytes += enode->size;
            enode = enode->next;
        }
        printf("  [extent] free_count=%d free_bytes=%zu\n", extent_count, extent_bytes);
        for (unsigned b = 0; b < arena->n_bins; ++b) {
            bin_t* bin = &arena->bins[b];
            int slab_count = 0, used = 0, free = 0;
            double total_util = 0.0;
            slab_t* slab = (slab_t*)bin->slabcur;
            if (slab) {
                ++slab_count;
                used += slab->nused;
                free += slab->edata.nregions - slab->nused;
                total_util += (double)slab->nused / slab->edata.nregions;
            }
            slab = (slab_t*)bin->unfull_slabs;
            while (slab) {
                ++slab_count;
                used += slab->nused;
                free += slab->edata.nregions - slab->nused;
                total_util += (double)slab->nused / slab->edata.nregions;
                slab = slab->link.qre_next;
            }
            slab = (slab_t*)bin->full_slabs.qlh_first;
            while (slab) {
                ++slab_count;
                used += slab->nused;
                total_util += (double)slab->nused / slab->edata.nregions;
                slab = slab->link.qre_next;
            }
            if (slab_count > 0) {
                double avg_util = total_util / slab_count;
                double frag = (double)free / (used + free + 1e-9);
                printf("  [bin %u] slabs=%d used=%d free=%d avg_util=%.2f frag=%.2f\n", b, slab_count, used, free, avg_util, frag);
            }
        }
    }
    // NUMA统计
    printf("[NUMA] 分配统计：\n");
    for (int i = 0; i < 64; ++i) {
        if (g_numa_stats.alloc_count[i] || g_numa_stats.free_count[i] || g_numa_stats.hugepage_count[i]) {
            printf("  node %d: alloc=%zu free=%zu alloc_bytes=%zu free_bytes=%zu hugepage=%zu huge_bytes=%zu\n",
                i, g_numa_stats.alloc_count[i], g_numa_stats.free_count[i],
                g_numa_stats.alloc_bytes[i], g_numa_stats.free_bytes[i],
                g_numa_stats.hugepage_count[i], g_numa_stats.hugepage_bytes[i]);
        }
    }
    // PAC大页统计
    printf("[PAC] hugepage_count=%zu hugepage_bytes=%zu\n",
        (size_t)atomic_fetch_add(&g_pac_stats.hugepage_count, 0),
        (size_t)atomic_fetch_add(&g_pac_stats.hugepage_bytes, 0));
    // 导出大块分配元数据
    pac_meta_export_csv("pac_blocks.csv");
    printf("[heap dump] 已导出大块分配元数据到 pac_blocks.csv\n");
    slab_region_stats_export_csv("slab_region_stats.csv");
    printf("[heap dump] 已导出slab/region详细统计到 slab_region_stats.csv\n");
    bin_slab_migrate_stats_export_csv("bin_slab_migrate_stats.csv");
    printf("[heap dump] 已导出bin/slab迁移/合并/拆分统计到 bin_slab_migrate_stats.csv\n");
    lmalloc_profile_export_json("profile_dump.json");
    lmalloc_profile_export_csv("profile_dump.csv");
    printf("[heap dump] 已导出profile到 profile_dump.json/profile_dump.csv\n");
    printf("=======================================\n");
}

void lmalloc_arena_stats_print(void) {
    printf("========== arena/bin/slab stats =========\n");
    for (unsigned a = 0; a < n_arenas; ++a) {
        arena_t* arena = arenas[a];
        arena_stats_print(arena);
        for (unsigned b = 0; b < arena->n_bins; ++b) {
            bin_t* bin = &arena->bins[b];
            if (bin->alloc_count || bin->free_count) {
                bin_stats_print(bin);
            }
        }
    }
    printf("========================================\n");
}

// 分配器整体统计打印（对外主接口）
void lmalloc_stats_print(void) {
    printf("========== lmalloc stats =========\n");
    printf("total: alloc=%zu free=%zu current=%zu peak=%zu\n",
           (size_t)atomic_load(&g_lmalloc_stats.alloc_count),
           (size_t)atomic_load(&g_lmalloc_stats.free_count),
           (size_t)atomic_load(&g_lmalloc_stats.current_bytes),
           (size_t)atomic_load(&g_lmalloc_stats.peak_bytes));
    lmalloc_arena_stats_print();
    printf("==================================\n");
}

void lmalloc_slab_stats_print(void) {
    printf("========== slab/region stats =========\n");
    for (unsigned a = 0; a < n_arenas; ++a) {
        arena_t* arena = arenas[a];
        for (unsigned b = 0; b < arena->n_bins; ++b) {
            bin_t* bin = &arena->bins[b];
            slab_t* slab = (slab_t*)bin->slabcur;
            if (slab) {
                printf("[arena %u bin %u slab] nused=%zu nregions=%zu\n", a, b, slab->nused, slab->edata.nregions);
            }
            slab = (slab_t*)bin->unfull_slabs;
            while (slab) {
                printf("[arena %u bin %u slab] nused=%zu nregions=%zu\n", a, b, slab->nused, slab->edata.nregions);
                slab = slab->link.qre_next;
            }
        }
    }
    printf("=======================================\n");
}

static char last_heap_dump_file[256] = {0};

// heap dump信号处理器
static void sigusr2_handler(int signo) {
    (void)signo;
    char fname[256];
    snprintf(fname, sizeof(fname), "heapdump_%ld.json", time(NULL));
    lmalloc_stats_export_json(fname);
    lmalloc_heap_dump();
    printf("[signal] Heap dump导出到%s\n", fname);
    snprintf(last_heap_dump_file, sizeof(last_heap_dump_file), "%s", fname);
#ifdef LMALLOC_PROFILE
    char pfname[256];
    snprintf(pfname, sizeof(pfname), "profile_%ld.json", time(NULL));
    lmalloc_profile_export_json(pfname);
    printf("[signal] Profile导出到%s\n", pfname);
#endif
}

// 注册信号处理器
void lmalloc_debug_signal_init(void) {
    struct sigaction sa = {0};
    sa.sa_handler = sigusr2_handler;
    sigaction(SIGUSR2, &sa, NULL);
}

// heap snapshot diff（简化版：比较两次heap dump文件大小）
void lmalloc_heap_snapshot_diff(const char* file1, const char* file2) {
    FILE* f1 = fopen(file1, "r");
    FILE* f2 = fopen(file2, "r");
    if (!f1 || !f2) { printf("[diff] 打开文件失败\n"); return; }
    fseek(f1, 0, SEEK_END); long sz1 = ftell(f1); rewind(f1);
    fseek(f2, 0, SEEK_END); long sz2 = ftell(f2); rewind(f2);
    printf("[diff] %s: %ld bytes, %s: %ld bytes, diff: %+ld bytes\n", file1, sz1, file2, sz2, sz2-sz1);
    fclose(f1); fclose(f2);
}

// 自动调优接口：根据统计/负载自动调整参数
void lmalloc_auto_tune(void) {
    // 示例：根据碎片率自动调整tcache/slab/decay参数
    size_t cur_bytes = atomic_load(&g_lmalloc_stats.current_bytes);
    size_t peak_bytes = atomic_load(&g_lmalloc_stats.peak_bytes);
    double frag = (peak_bytes > 0) ? (double)(peak_bytes - cur_bytes) / peak_bytes : 0.0;
    extern int g_decay_interval_sec;
    // tcache/slab参数可通过全局变量或配置项调整
    extern int tcache_bin_max_global;
    if (frag > 0.3) {
        // 碎片率高，收缩tcache/slab，缩短decay周期
        printf("[auto_tune] 碎片率%.2f，自动收缩tcache/slab，缩短decay周期\n", frag);
        // 缩短decay周期
        if (g_decay_interval_sec > 2) g_decay_interval_sec /= 2;
        // 收缩tcache bin最大容量
        if (tcache_bin_max_global > 16) tcache_bin_max_global /= 2;
        // 可扩展：收缩slab size等
    } else if (frag < 0.1) {
        // 碎片率低，适当扩容tcache/slab，延长decay周期
        printf("[auto_tune] 碎片率%.2f，自动扩容tcache/slab，延长decay周期\n", frag);
        // 延长decay周期
        if (g_decay_interval_sec < 60) g_decay_interval_sec *= 2;
        // 扩容tcache bin最大容量
        if (tcache_bin_max_global < 256) tcache_bin_max_global *= 2;
        // 可扩展：扩展slab size等
    }
    // 可扩展：根据热点/NUMA分布/分配失败等自动调优
}

// Prometheus风格metrics导出
void lmalloc_metrics_export_prometheus(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;
    fprintf(f, "# HELP lmalloc_alloc_count Total allocation count\n");
    fprintf(f, "# TYPE lmalloc_alloc_count counter\n");
    fprintf(f, "lmalloc_alloc_count %zu\n", (size_t)atomic_load(&g_lmalloc_stats.alloc_count));
    fprintf(f, "# HELP lmalloc_free_count Total free count\n");
    fprintf(f, "# TYPE lmalloc_free_count counter\n");
    fprintf(f, "lmalloc_free_count %zu\n", (size_t)atomic_load(&g_lmalloc_stats.free_count));
    fprintf(f, "# HELP lmalloc_current_bytes Current allocated bytes\n");
    fprintf(f, "# TYPE lmalloc_current_bytes gauge\n");
    fprintf(f, "lmalloc_current_bytes %zu\n", (size_t)atomic_load(&g_lmalloc_stats.current_bytes));
    fprintf(f, "# HELP lmalloc_peak_bytes Peak allocated bytes\n");
    fprintf(f, "# TYPE lmalloc_peak_bytes gauge\n");
    fprintf(f, "lmalloc_peak_bytes %zu\n", (size_t)atomic_load(&g_lmalloc_stats.peak_bytes));
    // 可扩展：NUMA/大页/decay/tcache/slab等详细metrics
    fclose(f);
    printf("[metrics] 已导出Prometheus风格metrics到 %s\n", filename);
} 