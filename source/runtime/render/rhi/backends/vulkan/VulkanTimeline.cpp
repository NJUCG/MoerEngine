#include "VulkanTimeline.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "misc/Types.h"

#include <algorithm>
#include <chrono>

#if WITH_CUDA
#include "platform/windows/WindowsSecurityAttributes.h"
#endif

namespace Moer::Render {

VulkanTimeline::VulkanTimeline(VulkanDevice& device) : VulkanDeviceObject(&device) {
    VkSemaphoreCreateInfo create_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    VkSemaphoreTypeCreateInfo timeline_semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_semaphore_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_semaphore_info.initialValue  = 0;

#if WITH_CUDA
    WindowsSecurityAttributes winSecurityAttributes;

    VkExportSemaphoreWin32HandleInfoKHR vulkanExportSemaphoreWin32HandleInfoKHR = {};
    vulkanExportSemaphoreWin32HandleInfoKHR.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    vulkanExportSemaphoreWin32HandleInfoKHR.pNext = NULL;
    vulkanExportSemaphoreWin32HandleInfoKHR.pAttributes = &winSecurityAttributes;
    // Opaque synchronization handles need generic write access for CUDA signal.
    // DXGI_SHARED_RESOURCE_WRITE is a resource-specific bit, not GENERIC_WRITE.
    vulkanExportSemaphoreWin32HandleInfoKHR.dwAccess = GENERIC_READ | GENERIC_WRITE;
    vulkanExportSemaphoreWin32HandleInfoKHR.name     = (LPCWSTR)NULL;

    VkExportSemaphoreCreateInfoKHR vulkanExportSemaphoreCreateInfo = {};
    vulkanExportSemaphoreCreateInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO_KHR;
    vulkanExportSemaphoreCreateInfo.pNext       = &vulkanExportSemaphoreWin32HandleInfoKHR;
    vulkanExportSemaphoreCreateInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    timeline_semaphore_info.pNext = &vulkanExportSemaphoreCreateInfo;
#endif

    create_info.pNext = &timeline_semaphore_info;
    VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &m_timeline));
}

VulkanTimeline::~VulkanTimeline() {
    if (m_timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device->GetDevice(), m_timeline, VK_NULL_HANDLE);
    }
}

uint64 VulkanTimeline::GetValue() const {
    std::lock_guard lock(m_state_mutex);
    return m_cached_gpu_completed_value;
}

VkResult VulkanTimeline::QueryGpuCompletion(uint64 value) {
    const VkResult cached_status = GetCachedGpuCompletionStatus(value);
    return cached_status != VK_NOT_READY ? cached_status : RefreshGpuCompletedValue(value);
}

bool VulkanTimeline::Wait(uint64 value) {
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pValues        = &value;
    info.pSemaphores    = &m_timeline;
    constexpr uint64 k_wait_slice_ns = 50'000'000;
    while (true) {
        const VkResult cached_status = GetCachedGpuCompletionStatus(value);
        if (cached_status != VK_NOT_READY) {
            return cached_status == VK_SUCCESS;
        }

        // Native timed waits allow CPU rejection/failure to interrupt a value
        // that will never be signalled; no completion-thread notification needed.
        const VkResult result = vkWaitSemaphores(m_device->GetDevice(), &info, k_wait_slice_ns);
        switch (result) {
        case VK_SUCCESS:
            RecordGpuCompletion(value);
            return GetCachedGpuCompletionStatus(value) == VK_SUCCESS;
        case VK_TIMEOUT:
            continue;
        default:
            HandleGpuOperationFailure(result, value, EVulkanFaultOperation::TimelineHostWait);
            return false;
        }
    }
}

void VulkanTimeline::RecordGpuCompletion(uint64 value) {
    std::lock_guard lock(m_state_mutex);
    m_cached_gpu_completed_value = std::max(m_cached_gpu_completed_value, value);
}

void VulkanTimeline::MarkSubmitted(uint64 value) {
    {
        std::lock_guard lock(m_state_mutex);
        m_submitted_value = std::max(m_submitted_value, value);
    }
    m_submission_cv.notify_all();
}

bool VulkanTimeline::WaitSubmitted(
    uint64                  value,
    const std::atomic_bool* continue_waiting
) {
    std::unique_lock<std::mutex> lock(m_state_mutex);
    while (m_submitted_value < value && !m_rejected_values.contains(value) && !m_is_failed &&
           !m_device->IsFaulted() &&
           (continue_waiting == nullptr || continue_waiting->load(std::memory_order_acquire))) {
        m_submission_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return m_submitted_value >= value && !m_rejected_values.contains(value) && !m_is_failed &&
           !m_device->IsFaulted();
}

void VulkanTimeline::Reject(uint64 value) noexcept {
    {
        std::lock_guard lock(m_state_mutex);
        try {
            m_rejected_values.emplace(value);
        } catch (...) {
            // A rejected value must always become terminal. If the exact
            // value set cannot grow, conservatively fail the whole fence
            // rather than leaving submission waiters blocked forever.
            m_is_failed      = true;
            m_failure_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    m_submission_cv.notify_all();
}

bool VulkanTimeline::IsRejected(uint64 value) const {
    std::lock_guard lock(m_state_mutex);
    return m_rejected_values.contains(value);
}

void VulkanTimeline::Fail(VkResult result) {
    {
        std::lock_guard lock(m_state_mutex);
        m_is_failed      = true;
        m_failure_result = result;
    }
    m_submission_cv.notify_all();
}

bool VulkanTimeline::IsFailed() const {
    std::lock_guard lock(m_state_mutex);
    return m_is_failed;
}

VkResult VulkanTimeline::GetCachedGpuCompletionStatus(uint64 value) const {
    std::lock_guard lock(m_state_mutex);
    // A later successful signal may pass a rejected value. Check the outcome
    // before using the monotonic GPU watermark, even on the cached fast path.
    if (m_rejected_values.contains(value)) {
        return VK_ERROR_UNKNOWN;
    }
    if (m_is_failed) {
        return m_failure_result != VK_SUCCESS ? m_failure_result : VK_ERROR_UNKNOWN;
    }
    if (m_device->IsFaulted()) {
        return m_device->GetFirstFaultResult();
    }
    return m_cached_gpu_completed_value >= value ? VK_SUCCESS : VK_NOT_READY;
}

VkResult VulkanTimeline::RefreshGpuCompletedValue(uint64 target_value) {
    uint64         completed_value = 0;
    const VkResult result = vkGetSemaphoreCounterValue(m_device->GetDevice(), m_timeline, &completed_value);
    if (result != VK_SUCCESS) {
        return HandleGpuOperationFailure(result, target_value, EVulkanFaultOperation::TimelineQuery);
    }
    RecordGpuCompletion(completed_value);
    // Recheck rejection/failure: it may have been published during the query.
    return GetCachedGpuCompletionStatus(target_value);
}

VkResult VulkanTimeline::HandleGpuOperationFailure(
    VkResult              result,
    uint64                value,
    EVulkanFaultOperation operation
) {
    const VulkanOperationContext fault_context{.operation = operation, .timeline = value};
    m_device->TryLatchFirstFault(fault_context, result);
    if (result != VK_ERROR_DEVICE_LOST) {
        m_device->EmergencyExitWithoutVulkanCleanup(fault_context, result);
    }
    Fail(result);
    return result;
}

} // namespace Moer::Render
