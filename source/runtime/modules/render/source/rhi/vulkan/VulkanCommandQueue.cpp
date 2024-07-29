#include "VulkanCommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"
#include "VulkanRHIResource.h"
#include "VulkanDevice.h"

#include <algorithm>
#include <vulkan/vulkan.h>
namespace Moer::Render {
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
            case ECommandQueueType::RAYTRACING:
                queue = _device->GetRayTracingQueue();
                break;
            default:
                queue = _device->GetGraphicsQueue();
                break;
        }
    }
    VulkanRHICommandQueue::~VulkanRHICommandQueue() {
    }
    void VulkanRHICommandQueue::SubmitCommands(
        uint32_t                  _num_command_lists,
        const RHICommandListBase* _command_lists,
        const RHISubmitInfo*      _submit_info) {

        const auto& signal_infos = _submit_info->GetSignalInfos();
        const auto& wait_infos   = _submit_info->GetWaitInfos();

        VkSubmitInfo2 submits{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};

        Moer::Array<VkCommandBufferSubmitInfo> cmd_submit_infos(_num_command_lists);
        for (uint32_t cmd_index = 0; cmd_index < _num_command_lists; cmd_index++) {
            cmd_submit_infos[cmd_index].commandBuffer = (VkCommandBuffer)(_command_lists[cmd_index]).GetNativeHandle();
            cmd_submit_infos[cmd_index].sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        }
        Moer::Array<VkSemaphoreSubmitInfo> vk_signal_infos;
        vk_signal_infos.reserve(signal_infos.size());
        Moer::Array<VkSemaphoreSubmitInfo> vk_wait_infos;
        vk_wait_infos.reserve(wait_infos.size());

        uint32_t extra_biranry_semaphores = 0;
        for (uint32_t signal_index = 0; signal_index < signal_infos.size(); signal_index++) {
            VulkanRHIFence* target_fence = (VulkanRHIFence*)signal_infos[signal_index].signal_fence;

            auto usage = target_fence->GetUsage();

            if (EnumHasAnyFlag(usage, EFenceUsageFlags::PRESENT)) {
                vk_signal_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                             VK_NULL_HANDLE,
                                             target_fence->GetSemaphoreHandle(),
                                             signal_infos[signal_index].signal_value,
                                             signal_infos[signal_index].signal_stage);
                vk_signal_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                             VK_NULL_HANDLE,
                                             target_fence->GetBinaryHandle(),
                                             0,
                                             signal_infos[signal_index].signal_stage);
            } else {
                //for timeline signals
                if (EnumHasAnyFlag(usage, EFenceUsageFlags::TIMELINE)) {
                    vk_signal_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                 VK_NULL_HANDLE,
                                                 target_fence->GetSemaphoreHandle(),
                                                 signal_infos[signal_index].signal_value,
                                                 signal_infos[signal_index].signal_stage);
                }

                //for binary signals to present stage wait
                if (EnumHasAnyFlag(usage, EFenceUsageFlags::BINARY)) {
                    vk_signal_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                 VK_NULL_HANDLE,
                                                 target_fence->GetBinaryHandle(),
                                                 0,
                                                 signal_infos[signal_index].signal_stage);
                }
            }
        }
        for (uint32_t wait_index = 0; wait_index < wait_infos.size(); wait_index++) {

            VulkanRHIFence* target_fence = (VulkanRHIFence*)wait_infos[wait_index].wait_fence;

            auto usage = target_fence->GetUsage();

            if (EnumHasAnyFlag(usage, EFenceUsageFlags::PRESENT)) {
                vk_wait_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                           VK_NULL_HANDLE,
                                           target_fence->GetSemaphoreHandle(),
                                           wait_infos[wait_index].wait_value,
                                           wait_infos[wait_index].wait_stage);
            } else {
                //for binary signals to present stage wait
                if (EnumHasAnyFlag(usage, EFenceUsageFlags::BINARY)) {
                    vk_wait_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                               VK_NULL_HANDLE,
                                               target_fence->GetBinaryHandle(),
                                               0,
                                               wait_infos[wait_index].wait_stage);
                }

                if (EnumHasAnyFlag(usage, EFenceUsageFlags::TIMELINE)) {
                    vk_wait_infos.emplace_back(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                               VK_NULL_HANDLE,
                                               target_fence->GetSemaphoreHandle(),
                                               wait_infos[wait_index].wait_value,
                                               wait_infos[wait_index].wait_stage);
                }
            }
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

    void VulkanRHICommandQueue::WaitForQueueComplete() {
        vkQueueWaitIdle(queue);
    }
}// namespace Moer::Render