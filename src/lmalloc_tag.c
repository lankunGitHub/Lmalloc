// lmalloc_tag.c
// 本文件实现了基于标签的内存分配器，用于统计和管理内存分配情况。
// 它允许用户为每个分配的内存块指定一个标签，以便于跟踪和分析。
// 标签统计表使用哈希表实现，支持线程安全。
// 主要功能：
// 1. 为每个分配的内存块记录其大小和标签。
// 2. 维护每个标签的分配计数、释放计数、当前使用字节数和峰值字节数。
// 3. 提供统计信息打印和导出功能。
// 4. 支持线程安全，使用互斥锁保护标签表。
// 设计trade-off：
// 1. 标签表大小固定为128，哈希冲突可能导致性能下降。
// 2. 标签字符串需要复制，增加了内存开销。
// 3. 标签统计是原子操作，但读取时需要加锁，可能影响性能。
// 4. 内存分配和释放需要额外的内存头，增加了内存开销。
// 5. 标签统计是基于标签的，无法精确到具体的内存块。

#include "lmalloc_tag.h"
#include "lmalloc_inline.h"
#include "lmalloc_json.h"
#include "memory/lmalloc.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool g_tag_enabled = false;
void lmalloc_enable_tag(bool enable) { g_tag_enabled = enable; }
#define TAG_TABLE_SIZE 128

// 标签统计表结构
// 每个标签条目包含其名称、分配统计信息和指向下一个条目的指针。
typedef struct tag_entry_s
{
    char* tag;
    lmalloc_tag_stat_t stat;
    struct tag_entry_s* next;
} tag_entry_t;

// 标签哈希表，用于存储所有已注册的标签条目。
// 使用固定大小的数组实现，通过哈希函数映射到数组索引。
static tag_entry_t* tag_table[TAG_TABLE_SIZE];
// 互斥锁，用于保护标签表的线程安全访问。
static pthread_mutex_t tag_table_mutex = PTHREAD_MUTEX_INITIALIZER;

// 哈希函数，将标签字符串映射到标签表的索引。
// 使用DJB2哈希算法，具有较好的分布性。
// tag为NULL时按空标签处理（曾直接解引用导致崩溃）。
static unsigned tag_hash(const char* tag)
{
    if (!tag)
        tag = "";
    unsigned h = 5381;
    for (const char* p = tag; *p; ++p)
        h = ((h << 5) + h) + (unsigned char)(*p);
    return h % TAG_TABLE_SIZE;
}

// 查找或创建标签条目。
// 如果标签已存在，则返回现有条目；否则创建新条目并添加到哈希表。
// 分配失败返回NULL（调用方必须判空，曾对NULL直接解引用崩溃）。
static tag_entry_t* tag_entry_find_or_create(const char* tag)
{
    if (!tag)
        tag = "";
    unsigned h = tag_hash(tag);
    tag_entry_t* e = tag_table[h];
    while (e)
    {
        if (strcmp(e->tag, tag) == 0)
            return e;
        e = e->next;
    }
    // 新建
    e = (tag_entry_t*)lmalloc(sizeof(tag_entry_t));
    if (!e)
        return NULL;
    memset(e, 0, sizeof(tag_entry_t));
    char* t = strdup(tag);
    e->tag = t ? t : (char*)""; // strdup失败退化为空标签，避免NULL比较崩溃
    e->next = tag_table[h];
    tag_table[h] = e;
    return e;
}

// 分配带标签的内存。
// 实际内存分配由主分配器完成，本函数仅记录统计信息。
// 参数：
// size: 所需内存大小。
// tag: 内存块的标签。
// 返回值：
// 成功时返回分配的内存指针，失败时返回NULL。
void* lmalloc_tag(size_t size, const char* tag)
{
    if (!g_tag_enabled)
        return lmalloc(size);
    // 走lmalloc_tagged：头部记录tag、泄漏检测按tag注册，
    // 标签统计由account_alloc记账
    void* p = lmalloc_tagged(size, tag);
    if (!p)
        return NULL;
    lmalloc_tag_account_alloc(tag, size);
    return p;
}

// 释放带标签的内存。
// 标签统计由lfree内部按hdr->tag记账（account_free），
// 本函数只做校验后转交lfree，避免重复计数。
void lfree_tag(void* ptr)
{
    if (!g_tag_enabled)
    {
        lfree(ptr);
        return;
    }
    if (!ptr)
        return;
    // 与lfree一致的校验：支持lmemalign返回的对齐指针，防双重释放
    lmalloc_hdr_t* hdr = hdr_of(ptr);
    if (hdr->magic != LMAGIC_ALLOC)
    {
        fprintf(stderr, "[lmalloc] Double free or corruption detected!\n");
        return;
    }
    lfree(ptr);
}

// 主分配路径标签记账：分配时累加，释放时递减
void lmalloc_tag_account_alloc(const char* tag, size_t size)
{
    if (!g_tag_enabled || !tag)
        return;
    pthread_mutex_lock(&tag_table_mutex);
    tag_entry_t* e = tag_entry_find_or_create(tag);
    if (e)
    {
        atomic_fetch_add(&e->stat.alloc_count, 1);
        atomic_fetch_add(&e->stat.current_bytes, size);
        size_t cur = atomic_load(&e->stat.current_bytes);
        size_t peak = atomic_load(&e->stat.peak_bytes);
        if (cur > peak)
            atomic_store(&e->stat.peak_bytes, cur);
    }
    pthread_mutex_unlock(&tag_table_mutex);
}

void lmalloc_tag_account_free(const char* tag, size_t size)
{
    if (!g_tag_enabled || !tag)
        return;
    pthread_mutex_lock(&tag_table_mutex);
    tag_entry_t* e = tag_entry_find_or_create(tag);
    if (e)
    {
        atomic_fetch_add(&e->stat.free_count, 1);
        size_t cur = atomic_load(&e->stat.current_bytes);
        if (cur >= size)
            atomic_fetch_sub(&e->stat.current_bytes, size);
    }
    pthread_mutex_unlock(&tag_table_mutex);
}

// 获取指定标签的统计信息。
// 参数：
// tag: 要查询的标签。
// out: 输出参数，用于接收统计信息。
// 返回值：
// 成功时返回1，失败时返回0。
int lmalloc_tag_stats(const char* tag, lmalloc_tag_stat_t* out)
{
    pthread_mutex_lock(&tag_table_mutex);
    tag_entry_t* e = tag_entry_find_or_create(tag);
    if (e)
        *out = e->stat;
    pthread_mutex_unlock(&tag_table_mutex);
    return e != NULL;
}

// 打印所有标签的统计信息。
// 此函数在多线程环境下不安全，建议在单线程或临界区中调用。
void lmalloc_tag_stats_print(void)
{
    pthread_mutex_lock(&tag_table_mutex);
    printf("[lmalloc tag stats]\n");
    for (int i = 0; i < TAG_TABLE_SIZE; ++i)
    {
        for (tag_entry_t* e = tag_table[i]; e; e = e->next)
        {
            printf("tag=%s alloc=%zu free=%zu cur=%zu peak=%zu\n",
                   e->tag,
                   e->stat.alloc_count,
                   e->stat.free_count,
                   e->stat.current_bytes,
                   e->stat.peak_bytes);
        }
    }
    pthread_mutex_unlock(&tag_table_mutex);
}

// 导出所有标签的统计信息到JSON文件。
// 参数：
// filename: 输出文件的名称。
// 注意：
// 导出过程是线程安全的，但文件操作本身不是线程安全的。
void lmalloc_tag_stats_export(const char* filename)
{
    pthread_mutex_lock(&tag_table_mutex);
    FILE* f = fopen(filename, "w");
    if (!f)
    {
        pthread_mutex_unlock(&tag_table_mutex);
        return;
    }
    fprintf(f, "[\n");
    int first = 1;
    for (int i = 0; i < TAG_TABLE_SIZE; ++i)
    {
        for (tag_entry_t* e = tag_table[i]; e; e = e->next)
        {
            if (!first)
                fprintf(f, ",\n");
            first = 0;
            // tag含引号/反斜杠时需转义，否则导出非法JSON
            fprintf(f, "  {\"tag\":\"");
            lmalloc_json_escape(f, e->tag);
            fprintf(f,
                    "\",\"alloc\":%zu,\"free\":%zu,\"cur\":%zu,"
                    "\"peak\":%zu}",
                    e->stat.alloc_count,
                    e->stat.free_count,
                    e->stat.current_bytes,
                    e->stat.peak_bytes);
        }
    }
    fprintf(f, "\n]\n");
    fclose(f);
    pthread_mutex_unlock(&tag_table_mutex);
}