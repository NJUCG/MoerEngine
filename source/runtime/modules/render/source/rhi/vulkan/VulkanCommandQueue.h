#ifndef VULKAN_COMMAND_QUEUE_H
#define VULKAN_COMMAND_QUEUE_H
#include "rhi/RHICommandQueue.h"
#include "rhi/RHICommon.h"

#include "VulkanRHIResource.h"

#include <vulkan/vulkan.h>
class VulkanRHICommandQueue final : public RHICommandQueue, public VulkanDeviceObject {
public:
    VulkanRHICommandQueue(class VulkanDevice* _device, ECommandQueueType _type);
    virtual ~VulkanRHICommandQueue();
    virtual void SubmitCommands(
        uint32_t                  _num_command_lists,
        const RHICommandListBase* _command_lists,
        const RHISubmitInfo*      _submit_info = nullptr) override;
    inline VkQueue GetHandle() { return queue; }

private:
    VkQueue       queue;
    VulkanDevice* device;
};
#endif