#pragma once

#include "VulkanCommon.h"

#if defined(_WIN32) || defined(_WIN64)
#define VMA_EXTERNAL_MEMORY       1
#define VMA_EXTERNAL_MEMORY_WIN32 1
#endif

#include <vk_mem_alloc.h>

// Creation-chain values belong to the calling allocation, never to shared state.
namespace Moer::Render {

inline VkExternalMemoryImageCreateInfo MakeExternalMemoryImageCreateInfo(const void* next = nullptr) {
    return {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = next,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    };
}

inline VkExternalMemoryBufferCreateInfo MakeExternalMemoryBufferCreateInfo(const void* next = nullptr) {
    return {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .pNext = next,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    };
}

} // namespace Moer::Render
