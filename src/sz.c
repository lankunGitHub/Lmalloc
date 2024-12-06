#include "../include/sz.h"
#include "../include/sc.h"
#include "../include/sc_ext.h"

// 全局当前策略指针
static sz_policy_t* g_sz_policy = NULL;

void sz_set_policy(sz_policy_t* policy) { g_sz_policy = policy; }

size_t sz_size2index(size_t size)
{
    return g_sz_policy->size2index(g_sz_policy->data, size);
}
size_t sz_index2size(size_t index)
{
    return g_sz_policy->index2size(g_sz_policy->data, index);
}
size_t sz_align(size_t size)
{
    return g_sz_policy->align(g_sz_policy->data, size);
}
void sz_adapt()
{
    if (g_sz_policy && g_sz_policy->adapt)
        g_sz_policy->adapt(g_sz_policy->data);
}

// ---- 静态策略适配器 ----
static size_t sz_static_size2index(void* data, size_t size)
{
    return sc_data_find((sc_data_t*)data, size);
}
static size_t sz_static_index2size(void* data, size_t index)
{
    return sc_data_index2size((sc_data_t*)data, index);
}
static size_t sz_static_align(void* data, size_t size)
{
    return sc_data_align((sc_data_t*)data, size);
}
static void sz_static_adapt(void* data)
{
    // 静态策略无需自适应
    (void)data;
}
sz_policy_t g_sz_static_policy = {
    .name = "static",
    .data = &g_sc_data,
    .size2index = sz_static_size2index,
    .index2size = sz_static_index2size,
    .align = sz_static_align,
    .adapt = sz_static_adapt,
};

// ---- 动态/自适应策略适配器 ----
static size_t sz_dynamic_size2index(void* data, size_t size)
{
    return sc_ext_data_find((sc_ext_data_t*)data, size);
}
static size_t sz_dynamic_index2size(void* data, size_t index)
{
    return sc_ext_data_index2size((sc_ext_data_t*)data, index);
}
static size_t sz_dynamic_align(void* data, size_t size)
{
    return sc_ext_data_align((sc_ext_data_t*)data, size);
}
static void sz_dynamic_adapt(void* data)
{
    sc_ext_data_adapt((sc_ext_data_t*)data);
}
sz_policy_t g_sz_dynamic_policy = {
    .name = "dynamic",
    .data = &g_sc_ext_data,
    .size2index = sz_dynamic_size2index,
    .index2size = sz_dynamic_index2size,
    .align = sz_dynamic_align,
    .adapt = sz_dynamic_adapt,
};

void sz_boot()
{
    sc_data_init(&g_sc_data);
    sc_ext_data_init(&g_sc_ext_data,
                     &g_sc_data,
                     &sc_ext_dynamic_policy); // 如需动态策略可切换
    sz_set_policy(&g_sz_static_policy);       // 默认静态策略
}