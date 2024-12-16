#include "page.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

size_t os_page;

static void os_pages_unmap(void* addr, size_t size)
{
    if (munmap(addr, size) == -1)
    {
        abort();
    }
}

static void*
os_pages_map(void* addr, size_t size, size_t alignment, bool* commit)
{
    (void)alignment;
    assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
    assert(ALIGNMENT_CEILING(size, os_page) == size);
    assert(size != 0);

    void* ret = NULL;

    int prot = *commit ? PROT_READ | PROT_WRITE : PROT_NONE;
    ret = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    assert(ret != NULL);

    if (ret == MAP_FAILED)
    {
        ret = NULL;
    }
    else if (addr != NULL && ret != addr)
    {
        os_pages_unmap(ret, size);
        ret = NULL;
    }

    return ret;
}

void* pages_map(void* addr, size_t size, size_t alignment, bool* commit)
{
    assert(alignment >= PAGE);
    assert(ALIGNMENT_ADDR2BASE(addr, alignment) == addr);

    void* ret = os_pages_map(addr, size, os_page, commit);
    if (ret == NULL || ret == addr)
    {
        return ret;
    }
    assert(addr == NULL);
    if (ALIGNMENT_ADDR2OFFSET(ret, alignment) != 0)
    {
        os_pages_unmap(ret, size);
        /*不支持mmap无法对齐分配的平台*/
        abort();
    }

    assert(PAGE_ADDR2BASE(ret) == ret);
    return ret;
}

void pages_unmap(void* addr, size_t size)
{
    assert(PAGE_ADDR2BASE(addr) == addr);
    assert(PAGE_CEILING(size) == size);

    os_pages_unmap(addr, size);
}

bool pages_commit(void* addr, size_t size)
{
    // pages_map(commit=false)以PROT_NONE映射，commit必须恢复访问权限；
    // MADV_WILLNEED只是预读提示，无法解除PROT_NONE，曾导致首次访问SIGSEGV
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}

bool pages_decommit(void* addr, size_t size)
{
    // 与commit对应：撤销访问权限（页面内容保持，再次commit可恢复）
    return mprotect(addr, size, PROT_NONE) == 0;
}

bool pages_purge_lazy(void* addr, size_t size)
{
#ifdef MADV_FREE
    return madvise(addr, size, MADV_FREE) == 0;
#else
    (void)addr;
    (void)size;
    return false;
#endif
}

bool pages_purge_forced(void* addr, size_t size)
{
    return madvise(addr, size, MADV_DONTNEED) == 0;
}

bool pages_dontdump(void* addr, size_t size)
{
#ifdef MADV_DONTDUMP
    return madvise(addr, size, MADV_DONTDUMP) == 0;
#else
    (void)addr;
    (void)size;
    return false;
#endif
}

bool pages_dodump(void* addr, size_t size)
{
#ifdef MADV_DODUMP
    return madvise(addr, size, MADV_DODUMP) == 0;
#else
    (void)addr;
    (void)size;
    return false;
#endif
}

static size_t os_page_detect(void)
{

    long result = sysconf(_SC_PAGESIZE);
    if (result == -1)
    {
        return (size_t)PAGE;
    }
    return (size_t)result;
}

bool pages_boot(void)
{
    os_page = os_page_detect();
    if (os_page > PAGE)
    {
        os_page = PAGE;
    }

    return true;
}
