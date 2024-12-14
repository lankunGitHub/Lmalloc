#include "edata.h"
#include <string.h>

void edata_binit(
    edata_t* edata, void* addr, size_t size, size_t sn, bool is_reused)
{
    edata->addr = addr;
    edata->bsize = size;
    edata->e_sn = sn;
    // 其他字段初始化
    edata->prev = NULL;
    edata->next = NULL;
    edata->region_aux_arr = NULL;
    edata->nregions = 0;
    (void)is_reused;
}

void edata_heap_new(edata_heap_t* heap) { memset(heap, 0, sizeof(*heap)); }

void edata_avail_new(edata_avail_t* avail) { memset(avail, 0, sizeof(*avail)); }

void edata_avail_insert(edata_avail_t* avail, edata_t* edata)
{
    // 简单链表头插法，复用ph_t的root字段作为链表头
    edata->next = (edata_t*)avail->ph.root;
    avail->ph.root = edata;
}