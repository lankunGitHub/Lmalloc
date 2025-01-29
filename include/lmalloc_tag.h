/*
 * lmalloc_tag.h - 标签分配与分组统计API
 *
 * 支持为每次分配打标签，便于分组统计、热点分析、内存归因。
 * 线程安全，适合高并发场景。
 *
 * 典型用法：
 *   void* p = lmalloc_tagged(256, "session");
 *   lfree_tag(p);
 *   lmalloc_tag_stats_print();
 *   lmalloc_tag_stats_export("tag_stats.json");
 */
#ifndef LMALLOC_TAG_H
#define LMALLOC_TAG_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct lmalloc_tag_stat_s {
    _Atomic size_t alloc_count;
    _Atomic size_t free_count;
    _Atomic size_t current_bytes;
    _Atomic size_t peak_bytes;
} lmalloc_tag_stat_t;

// 带标签分配/释放
void* lmalloc_tag(size_t size, const char* tag);
void lfree_tag(void* ptr);

// 查询指定标签统计
int lmalloc_tag_stats(const char* tag, lmalloc_tag_stat_t* out);
// 打印所有标签统计
void lmalloc_tag_stats_print(void);
// 导出所有标签统计到文件（文本/JSON）
void lmalloc_tag_stats_export(const char* filename);

// 内部记账接口：主分配路径（lmalloc_tagged/lfree）的标签统计。
// tag为NULL或未开启标签功能时为空操作。
void lmalloc_tag_account_alloc(const char* tag, size_t size);
void lmalloc_tag_account_free(const char* tag, size_t size);

#ifdef __cplusplus
}
#endif
#endif // LMALLOC_TAG_H 