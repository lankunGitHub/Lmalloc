#pragma once

#include "ts.h"

void* pages_map(void* addr, size_t size, size_t alignment, bool* commit);
void pages_unmap(void* addr, size_t size);
bool pages_commit(void* addr, size_t size);
bool pages_decommit(void* addr, size_t size);
bool pages_purge_lazy(void* addr, size_t size);
bool pages_purge_forced(void* addr, size_t size);

bool pages_dontdump(void* addr, size_t size);
bool pages_dodump(void* addr, size_t size);
bool pages_boot(void);