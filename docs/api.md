# Lmalloc API 参考

本文档描述 `include/memory/lmalloc.h` 中的全部公开接口。所有接口均为线程安全，支持高并发。

## 类型定义

### lmalloc_tag_stat_t

```c
typedef struct lmalloc_tag_stat_s {
    _Atomic size_t alloc_count;   // 分配次数
    _Atomic size_t free_count;    // 释放次数
    _Atomic size_t current_bytes; // 当前分配字节数
    _Atomic size_t peak_bytes;    // 峰值分配字节数
} lmalloc_tag_stat_t;
```

### lmalloc_leak_info_t

```c
typedef struct lmalloc_leak_info_s {
    void* ptr;            // 泄漏对象指针
    size_t size;          // 对象大小
    time_t alloc_time;    // 分配时间
    const char* tag;      // 分配标签
    uint32_t thread_id;   // 分配线程ID
    void* callstack[8];   // 分配时调用栈
    int callstack_depth;  // 调用栈深度
} lmalloc_leak_info_t;
```

### lmalloc_profile_entry_t

```c
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
```

## 分配/释放 API

### lmalloc

```c
void* lmalloc(size_t size);
```

分配 `size` 字节内存，类似 `malloc`。成功返回指针，失败返回 `NULL`。

### lfree

```c
void lfree(void* ptr);
```

释放由 `lmalloc`/`lcalloc`/`lrealloc`/`lmemalign` 分配的内存。释放 `NULL` 是安全的。

### lcalloc

```c
void* lcalloc(size_t nmemb, size_t size);
```

分配 `nmemb * size` 字节的零初始化内存，类似 `calloc`。

### lrealloc

```c
void* lrealloc(void* ptr, size_t size);
```

调整已分配内存大小，类似 `realloc`：
- `ptr == NULL` 时等价于 `lmalloc(size)`
- `size <= 原大小` 时原地返回原指针
- 扩容时分配新块、拷贝数据、释放原块

### lmemalign

```c
void* lmemalign(size_t alignment, size_t size);
```

按 `alignment`（2 的幂）字节对齐分配 `size` 字节内存，类似 `posix_memalign`。
大块（> 1MB）或高对齐（> 4096）请求直接走 pac/mmap 路径。

### lmalloc_tag / lfree_tag

```c
void* lmalloc_tag(size_t size, const char* tag);
void lfree_tag(void* ptr);
```

带标签分配/释放，便于分组统计、热点分析与内存归因。标签统计需先
`lmalloc_enable_tag(true)`。

## 统计与监控 API

### lmalloc_stats_print

```c
void lmalloc_stats_print(void);
```

打印分配/释放/内存使用等实时统计（含 arena/bin/slab 明细）。

### lmalloc_heap_dump

```c
void lmalloc_heap_dump(void);
```

打印当前堆快照：各 arena 的 extent 空闲状态、bin/slab 利用率、NUMA 与大页统计，
并导出 `pac_blocks.csv`、`slab_region_stats.csv` 等明细文件。

### lmalloc_auto_tune

```c
void lmalloc_auto_tune(void);
```

根据碎片率自动调整 tcache 容量、decay 周期等参数，可定期调用。

### lmalloc_metrics_export_prometheus

```c
void lmalloc_metrics_export_prometheus(const char* filename);
```

导出 Prometheus 风格 metrics 到文件，便于监控系统采集。

## 标签统计 API

```c
int lmalloc_tag_stats(const char* tag, lmalloc_tag_stat_t* out); // 查询指定标签统计
void lmalloc_tag_stats_print(void);                              // 打印所有标签统计
void lmalloc_tag_stats_export(const char* filename);             // 导出标签统计
```

## 泄漏检测 API

```c
size_t lmalloc_leak_scan(lmalloc_leak_info_t* buf, size_t max, time_t min_age_sec);
void lmalloc_leak_report_print(time_t min_age_sec);
void lmalloc_leak_report_export(const char* filename, time_t min_age_sec);
```

扫描所有活跃分配，返回存活时间超过 `min_age_sec` 秒的可疑泄漏对象。
需先 `lmalloc_enable_leak(true)`。

## 分配追踪（profile）API

```c
size_t lmalloc_profile_events_count(void);
size_t lmalloc_profile_events_copy(lmalloc_profile_entry_t* buf, size_t max);
void lmalloc_profile_export(const char* filename);
```

实时记录分配/释放事件（含调用栈采样），支持导出分析。需先
`lmalloc_enable_trace(true)` / `lmalloc_enable_profile(true)`。

## 兼容接口

```c
int my_mallopt(int param, int value);          // mallopt 兼容
size_t my_malloc_usable_size(void* ptr);       // malloc_usable_size 兼容
void my_malloc_stats(void);                    // malloc_stats 兼容
#define mallopt my_mallopt
#define malloc_usable_size my_malloc_usable_size
#define malloc_stats my_malloc_stats
```

## 功能开关

```c
void lmalloc_enable_leak(bool enable);
void lmalloc_enable_stats(bool enable);
void lmalloc_enable_profile(bool enable);
void lmalloc_enable_tag(bool enable);
void lmalloc_enable_trace(bool enable);
```

## 使用示例

```c
#include <memory/lmalloc.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    lmalloc_enable_stats(true);
    lmalloc_enable_leak(true);

    // 基本分配
    void* buf = lmalloc(4096);
    memset(buf, 0, 4096);
    lfree(buf);

    // 标签分配
    void* s = lmalloc_tag(1024, "session");
    lfree_tag(s);

    // 统计与泄漏报告
    lmalloc_stats_print();
    lmalloc_tag_stats_print();
    lmalloc_leak_report_print(0);
    lmalloc_metrics_export_prometheus("lmalloc.prom");

    printf("done\n");
    return 0;
}
```
