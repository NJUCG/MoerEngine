#include "VulkanQueue.h"
#include "VulkanDevice.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#include "vulkan/VulkanFaultInjection.h"
#endif
#include "log/LogSystem.h"
#include "rhi/RHIThreadHeartbeat.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace Moer::Render {

VulkanNativeQueue::VulkanNativeQueue(EQueueType _type, VulkanDevice& _device) : device(_device), type(_type) {
    switch (_type) {
        case EQueueType::Graphics:
            queue = _device.GetGraphicsQueue();
            break;
        case EQueueType::Compute:
            queue = _device.GetComputeQueue();
            break;
        case EQueueType::Copy:
            queue = _device.GetTransferQueue();
            break;
        default:
            assert(false && "Invalid queue type");
    }
    assert(queue != VK_NULL_HANDLE && "Invalid queue type!");
}

VulkanNativeQueue::~VulkanNativeQueue() {}

VulkanOperationResult VulkanNativeQueue::SubmitEmpty(
    const VulkanOperationContext& _context,
    VkFence                       _fence
) {
    if (device.IsFaulted()) {
        wait_infos.clear();
        signal_infos.clear();
        device.RecordRejectedSubmit();
        return {EVulkanOperationStatus::Rejected, device.GetFirstFaultResult()};
    }

    VkSubmitInfo2 submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

    submit_info.pNext                    = nullptr;
    submit_info.waitSemaphoreInfoCount   = wait_infos.size();
    submit_info.pWaitSemaphoreInfos      = wait_infos.data();
    submit_info.signalSemaphoreInfoCount = signal_infos.size();
    submit_info.pSignalSemaphoreInfos    = signal_infos.data();
    submit_info.commandBufferInfoCount   = 0;
    submit_info.pCommandBufferInfos      = VK_NULL_HANDLE;
    VulkanOperationContext context = _context;
    context.operation  = EVulkanFaultOperation::QueueSubmitEmpty;
    context.queue_type = type;
    context.queue      = queue;
    const VulkanOperationResult outcome =
        device.SubmitOnQueue(queue, submit_info, _fence, context);
#if MOER_VULKAN_TEST_SUPPORT
    NotifyVulkanNativeSubmission(type, queue, outcome, true);
#endif
    wait_infos.clear();
    signal_infos.clear();
    return outcome;
}

VulkanOperationResult
VulkanNativeQueue::Submit(VulkanCommandList& _cmdlist, const VulkanOperationContext& _context, VkFence _fence) {
    VulkanCommandList* command_lists[] = {&_cmdlist};
    return Submit(std::span<VulkanCommandList* const>(command_lists), _context, _fence);
}

VulkanOperationResult VulkanNativeQueue::Submit(
    std::span<VulkanCommandList* const> _cmdlists,
    const VulkanOperationContext&       _context,
    VkFence                             _fence
) {
    assert(!_cmdlists.empty() && "ordered command-buffer submit must not be empty");
    if (device.IsFaulted()) {
        wait_infos.clear();
        signal_infos.clear();
        device.RecordRejectedSubmit();
        return {EVulkanOperationStatus::Rejected, device.GetFirstFaultResult()};
    }

    VulkanOperationContext context = _context;
    context.queue_type = type;
    context.queue      = queue;
#if MOER_VULKAN_TEST_SUPPORT
    if (context.operation == EVulkanFaultOperation::PresentSubmit &&
        VulkanFaultInjection::ShouldInjectPresentSubmit()) {
        LOG_INFO(
            "[VulkanFault][Injection] point=present-submit trigger={} mode=synthetic-device-lost",
            VulkanFaultInjection::GetPresentTrigger()
        );
        wait_infos.clear();
        signal_infos.clear();
        return VulkanFaultInjection::InjectPresentSubmitFault(device, context);
    }
#endif

    VkSubmitInfo2 submit_info{};
    Array<VkCommandBufferSubmitInfo> cmd_infos;
    cmd_infos.reserve(_cmdlists.size());
    for (VulkanCommandList* cmd_list : _cmdlists) {
        assert(cmd_list != nullptr && "ordered command-buffer submit contains null entry");
        cmd_infos.push_back(
            VkCommandBufferSubmitInfo{
                .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext         = nullptr,
                .commandBuffer = cmd_list->GetHandle()
            }
        );
    }
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

    submit_info.pNext                    = nullptr;
    submit_info.waitSemaphoreInfoCount   = wait_infos.size();
    submit_info.pWaitSemaphoreInfos      = wait_infos.data();
    submit_info.signalSemaphoreInfoCount = signal_infos.size();
    submit_info.pSignalSemaphoreInfos    = signal_infos.data();
    submit_info.commandBufferInfoCount   = static_cast<uint32_t>(cmd_infos.size());
    submit_info.pCommandBufferInfos      = cmd_infos.data();

    if (context.operation == EVulkanFaultOperation::None) {
        context.operation = EVulkanFaultOperation::QueueSubmit;
    }
    const VulkanOperationResult outcome =
        device.SubmitOnQueue(queue, submit_info, _fence, context);
#if MOER_VULKAN_TEST_SUPPORT
    NotifyVulkanNativeSubmission(type, queue, outcome, false);
#endif
    wait_infos.clear();
    signal_infos.clear();
    return outcome;
}

void VulkanNativeQueue::Wait(VulkanTimeline* _fence, uint64 _fence_val, VkPipelineStageFlags2 _stage) {
    VkSemaphore sem = _fence->GetSemaphore();
    wait_infos.push_back(
        VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = sem,
            .value     = _fence_val,
            .stageMask = _stage
        }
    );
}

void VulkanNativeQueue::Wait(VkSemaphore _sem, VkPipelineStageFlags2 _stage) {
    wait_infos.push_back(
        VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = _sem,
            .value     = 0,
            .stageMask = _stage
        }
    );
}
void VulkanNativeQueue::Signal(VulkanTimeline* _fence, uint64 _fence_val, VkPipelineStageFlags2 _stage) {
    VkSemaphore sem = _fence->GetSemaphore();
    signal_infos.push_back(
        VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = sem,
            .value     = _fence_val,
            .stageMask = _stage
        }
    );
}
void VulkanNativeQueue::Signal(VkSemaphore _sem, VkPipelineStageFlags2 _stage) {
    signal_infos.push_back(
        VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = _sem,
            .value     = 0,
            .stageMask = _stage
        }
    );
}


void VulkanNativeQueue::BeginLabel(std::string_view _label, float4 _color) {
    std::unique_lock<std::mutex> guard(*submit_mutex);
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = _label.data();
    label.color[0]   = _color.x;
    label.color[1]   = _color.y;
    label.color[2]   = _color.z;
    label.color[3]   = _color.w;
    vkQueueBeginDebugUtilsLabelEXT(queue, &label);
}

void VulkanNativeQueue::EndLabel() {
    std::unique_lock<std::mutex> guard(*submit_mutex);
    vkQueueEndDebugUtilsLabelEXT(queue);
}

void VulkanNativeQueue::InsertLabel(std::string_view _label, float4 _color) {
    std::unique_lock<std::mutex> guard(*submit_mutex);
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = _label.data();
    label.color[0]   = _color.x;
    label.color[1]   = _color.y;
    label.color[2]   = _color.z;
    label.color[3]   = _color.w;
    vkQueueInsertDebugUtilsLabelEXT(queue, &label);
}

} // namespace Moer::Render
