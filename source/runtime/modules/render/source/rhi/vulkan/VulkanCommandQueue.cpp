#include "VulkanCommandQueue.h"
#include "rhi/RHICommon.h"
#include "VulkanCommandList.h"
#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"
#include "VulkanRHIResource.h"
#include "VulkanDevice.h"

#include <algorithm>
#include <vector>
#include <vulkan/vulkan.h>
VulkanRHICommandQueue::VulkanRHICommandQueue(VulkanDevice* _device, ECommandQueueType _type) : VulkanDeviceObject(_device) {

    switch (_type) {

        case ECommandQueueType::GRAPHICS:
            queue = _device->GetGraphicsQueue();
            break;
        case ECommandQueueType::COMPUTE:
            queue = _device->GetComputeQueue();
            break;
        case ECommandQueueType::COPY:
            queue = _device->GetTransferQueue();
            break;
        default:
            queue = _device->GetGraphicsQueue();
            break;
    }
    device = _device;
}
VulkanRHICommandQueue::~VulkanRHICommandQueue() {
}
void VulkanRHICommandQueue::SubmitCommands(
    uint32_t                  _num_command_lists,
    const RHICommandListBase* _command_lists,
    const RHISubmitInfo*      _submit_info) {

    const auto& signal_infos = _submit_info->GetSignalInfos();
    const auto& wait_infos   = _submit_info->GetWaitInfos();

    VkSubmitInfo2 submits;

    std::vector<VkCommandBufferSubmitInfo> cmd_submit_infos(_num_command_lists);
    for (uint32_t cmd_index; cmd_index < _num_command_lists; cmd_index++) {
        cmd_submit_infos[cmd_index].commandBuffer = (VkCommandBuffer)(_command_lists[cmd_index].GetNativeHandle());
        cmd_submit_infos[cmd_index].sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    }
    std::vector<VkSemaphoreSubmitInfo> vk_signal_infos(signal_infos.size());
    std::vector<VkSemaphoreSubmitInfo> vk_wait_infos(wait_infos.size());

    uint32_t extra_biranry_semaphores = 0;
    for (uint32_t signal_index = 0; signal_index < vk_signal_infos.size(); signal_index++) {
        VulkanRHIFence* target_fence = (VulkanRHIFence*)signal_infos[signal_index].signal_fence;
        vk_signal_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                     VK_NULL_HANDLE,
                                     target_fence->GetSemaphoreHandle(),
                                     signal_infos[signal_index].signal_value);

        //for binary signals to present stage wait
        if (target_fence->GetBinaryHandle() != VK_NULL_HANDLE) {
            vk_signal_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                         VK_NULL_HANDLE,
                                         target_fence->GetBinaryHandle(),
                                         0);
        }
    }

    for (uint32_t wait_index = 0; wait_index < vk_wait_infos.size(); wait_index++) {
        vk_wait_infos[wait_index].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        vk_wait_infos[wait_index].semaphore = ((VulkanRHIFence*)wait_infos[wait_index].wait_fence)->GetSemaphoreHandle();
        vk_wait_infos[wait_index].value     = wait_infos[wait_index].wait_value;
    }

    submits.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submits.commandBufferInfoCount = cmd_submit_infos.size();
    submits.pCommandBufferInfos    = cmd_submit_infos.data();

    submits.pSignalSemaphoreInfos    = vk_signal_infos.data();
    submits.signalSemaphoreInfoCount = vk_signal_infos.size();

    submits.pWaitSemaphoreInfos    = vk_wait_infos.data();
    submits.waitSemaphoreInfoCount = vk_wait_infos.size();

    vkQueueSubmit2(queue, _num_command_lists, &submits, VK_NULL_HANDLE);
}