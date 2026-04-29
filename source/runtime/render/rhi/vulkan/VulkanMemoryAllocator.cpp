#define VMA_IMPLEMENTATION           1
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

#include <volk.h>

#include "VulkanMemoryAllocator.h"

namespace Moer::Render {

VkExternalMemoryImageCreateInfo* GetExternalMemoryImageCreateInfoPtr(const void* p_next) {
    static VkExternalMemoryImageCreateInfo vkExternalMemImageCreateInfo = {};
    static bool                            firstLoad                    = true;
    if (firstLoad) {
        firstLoad = false;

        vkExternalMemImageCreateInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        vkExternalMemImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    }
    vkExternalMemImageCreateInfo.pNext = p_next;
    return &vkExternalMemImageCreateInfo;
}

VkExternalMemoryBufferCreateInfo* GetExternalMemoryBufferCreateInfoPtr(const void* p_next) {
    static VkExternalMemoryBufferCreateInfo vkExternalBufImageCreateInfo = {};
    static bool                             firstLoad                    = true;
    if (firstLoad) {
        firstLoad = false;

        vkExternalBufImageCreateInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        vkExternalBufImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    }
    vkExternalBufImageCreateInfo.pNext = p_next;
    return &vkExternalBufImageCreateInfo;
}

} // namespace Moer::Render