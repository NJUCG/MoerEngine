#pragma once

#if defined(_WIN32) || defined(_WIN64)
#define VMA_EXTERNAL_MEMORY       1
#define VMA_EXTERNAL_MEMORY_WIN32 1
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#endif

#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// 这两个函数的作用是，在调用vmaCreateImage和vmaCreateBuffer时，将对应资源设置为win32导出
namespace Moer::Render {

VkExternalMemoryImageCreateInfo* GetExternalMemoryImageCreateInfoPtr(const void* p_next);

VkExternalMemoryBufferCreateInfo* GetExternalMemoryBufferCreateInfoPtr(const void* p_next);

} // namespace Moer::Render
