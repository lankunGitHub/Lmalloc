# Lmalloc

一个借鉴 jemalloc 设计的高性能多线程内存分配器（C 实现），支持大小类（size class）、per-thread tcache、per-core arena、bin/slab 小块分配、extent 红黑树大块分配、NUMA 感知、大页（hugepage）、泄漏检测、标签统计、分配追踪（profile）与 Prometheus 风格监控导出。

## 特性

- **多级分配路径**：tcache（线程缓存）→ arena/bin/slab（小块）→ extent 红黑树（大块）→ pac/mmap（超大块）
- **大小类**：jemalloc 风格的 size class 分组，静态/动态/自适应三种策略（`sz.h`）
- **多线程扩展**：per-core arena 隔离、bin 级锁、lock-free tcache
- **NUMA 与大页**：NUMA 节点感知分配、2MB 以上自动大页（`pac.c`）
- **可观测性**：实时统计、heap dump、slab/region 生命周期导出、CSV/JSON/Prometheus 导出
- **调试与安全**：magic 越界检测、双重释放检测、泄漏检测、分配标签、调用栈追踪
- **自动调优**：根据碎片率自动调整 tcache 容量与 decay 周期（`lmalloc_auto_tune`）

## 构建

```bash
make            # 生成 libmemory.a 和 libmemory.so
make static     # 仅静态库
make shared     # 仅动态库
make test       # 编译并运行 tests/test_main 与 tests/check
make install PREFIX=/usr/local   # 安装库和头文件
make uninstall
make clean
```

## 快速上手

```c
#include <memory/lmalloc.h>

int main(void) {
    void* p = lmalloc(128);
    // ...
    lfree(p);

    void* q = lmalloc_tag(256, "session");  // 带标签分配
    lfree_tag(q);

    lmalloc_stats_print();                  // 打印统计
    lmalloc_leak_report_print(0);           // 泄漏报告
    lmalloc_metrics_export_prometheus("lmalloc.prom");
    return 0;
}
```

编译链接：

```bash
gcc -Iinclude app.c -L. -lmemory -lpthread
```

## 主要 API

| 分类 | 接口 |
|---|---|
| 分配/释放 | `lmalloc` `lfree` `lcalloc` `lrealloc` `lmemalign` |
| 标签分配 | `lmalloc_tag` `lfree_tag` `lmalloc_tag_stats` `lmalloc_tag_stats_print` |
| 统计监控 | `lmalloc_stats_print` `lmalloc_heap_dump` `lmalloc_auto_tune` `lmalloc_metrics_export_prometheus` |
| 泄漏检测 | `lmalloc_leak_scan` `lmalloc_leak_report_print` `lmalloc_leak_report_export` |
| 分配追踪 | `lmalloc_profile_events_count` `lmalloc_profile_events_copy` `lmalloc_profile_export` |
| 功能开关 | `lmalloc_enable_leak` `lmalloc_enable_stats` `lmalloc_enable_profile` `lmalloc_enable_tag` `lmalloc_enable_trace` |

完整 API 参考见 [docs/api.md](docs/api.md)。

## 目录结构

```
Lmalloc/
├── include/          # 公开头文件（memory/lmalloc.h 为主入口）
├── src/              # 实现
│   ├── lmalloc.c     # 主分配/释放 API
│   ├── tcache.c      # 线程缓存
│   ├── arena.c       # arena 管理、extent 红黑树、decay
│   ├── bin.c         # bin/slab 小块分配
│   ├── base.c        # base 元数据分配器
│   ├── pac.c         # 大块 mmap/大页分配
│   ├── sc.c sc_ext.c sz.c  # 大小类
│   ├── lmalloc_stats.c     # 统计与监控
│   ├── lmalloc_leak.c      # 泄漏检测
│   ├── lmalloc_tag.c       # 标签统计
│   └── ...
├── tests/            # 测试（test_main 综合测试、check 正确性测试）
└── docs/             # 设计文档
```

## 文档

- [使用说明](docs/usage.md)
- [API 参考](docs/api.md)
- [大小类设计](docs/slab.md) · [bin 设计](docs/bin.md) · [arena 设计](docs/arena.md)
- [tcache 设计](docs/tcache.md) · [extent 管理](docs/extent.md)
- [泄漏检测](docs/leak.md) · [标签统计](docs/tag.md) · [profile](docs/profile.md)
- [安全机制](docs/security.md) · [自动调优](docs/auto_tune.md)

## License

MIT
