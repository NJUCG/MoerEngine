#include "VulkanCommand.h"
#include "VulkanDevice.h"
namespace Moer::Render {
    VulkanCommandAllocator::VulkanCommandAllocator(VulkanDevice* _device) : VulkanDeviceObject(_device) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        for (uint32_t i = 0; i < (uint32_t)ECommandListType::Num; ++i) {
            pool_info.queueFamilyIndex = VulkanEnumTranslator::METoVkQueueFamilyIndex((ECommandListType)i, m_device);
            VK_CHECK_RESULT(vkCreateCommandPool(_device->GetDevice(), &pool_info, nullptr, &m_command_pool[i]));
        }
    }

    void VulkanCommandAllocator::Dispose() {

        for (auto& pool : m_command_pool) {
            vkDestroyCommandPool(m_device->GetDevice(), pool, nullptr);
        }
    }

    void VulkanCommandAllocator::Reset() {
        for (uint32_t i = 0; i < (uint32_t)ECommandListType::Num; ++i) {
            vkResetCommandPool(m_device->GetDevice(), m_command_pool[i], 0);
        }
    }
}// namespace Moer::Render