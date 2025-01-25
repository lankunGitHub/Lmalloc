#pragma once
/*
 * lmalloc_json.h - JSON导出辅助
 *
 * 提供字符串转义输出，避免tag等用户字符串中的引号/反斜杠破坏JSON格式。
 */
#include <stdio.h>

// JSON字符串转义输出（NULL安全）
static inline void lmalloc_json_escape(FILE* f, const char* s)
{
    if (!s)
        return;
    for (const char* p = s; *p; ++p)
    {
        switch (*p)
        {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        default: fputc(*p, f); break;
        }
    }
}
