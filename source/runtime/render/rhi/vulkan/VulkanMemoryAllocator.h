#pragma once

#include "VulkanCommon.h"

#if defined(_WIN32) || defined(_WIN64)
#define VMA_EXTERNAL_MEMORY       1
#define VMA_EXTERNAL_MEMORY_WIN32 1
#endif

#include <vk_mem_alloc.h>

// 这两个函数的作用是，在调用vmaCreateImage和vmaCreateBuffer时，将对应资源设置为win32导出
namespace Moer::Render {

VkExternalMemoryImageCreateInfo* GetExternalMemoryImageCreateInfoPtr(const void* pNext);

VkExternalMemoryBufferCreateInfo* GetExternalMemoryBufferCreateInfoPtr(const void* pNext);

} // namespace Moer::Render
