#include "VulkanTimeline.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "log/LogSystem.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if WITH_CUDA
#include "platform/windows/WindowsSecurityAttributes.h"
#endif

namespace Moer::Render {

VulkanTimeline::VulkanTimeline(VulkanDevice& _device) : VulkanDeviceObject(&_device) {
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
    vulkanExportSemaphoreWin32HandleInfoKHR.dwAccess = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
    vulkanExportSemaphoreWin32HandleInfoKHR.name     = (LPCWSTR)NULL;

    VkExportSemaphoreCreateInfoKHR vulkanExportSemaphoreCreateInfo = {};
    vulkanExportSemaphoreCreateInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO_KHR;
    vulkanExportSemaphoreCreateInfo.pNext       = &vulkanExportSemaphoreWin32HandleInfoKHR;
    vulkanExportSemaphoreCreateInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    timeline_semaphore_info.pNext = &vulkanExportSemaphoreCreateInfo;
#endif

    create_info.pNext = &timeline_semaphore_info;
    VK_CHECK_RESULT(vkCreateSemaphore(m_device->GetDevice(), &create_info, nullptr, &timeline));
}

uint64 VulkanTimeline::GetValue() const {
    // uint64_t value;
    // vkGetSemaphoreCounterValue(m_device->GetDevice(), timeline, &value);
    // // vkGetSemaphoreCounterValue(m_device->GetDevice(), m_fence.timeline, &value);
    // return value;
    std::unique_lock<std::mutex> lock(cv_m);
    return current_value;
}

uint64 VulkanTimeline::GetDeviceValue() const {
    uint64_t value;
    {
        std::unique_lock<std::mutex> lock(cv_m);
        value = current_value;
    }
    VkResult result = vkGetSemaphoreCounterValue(m_device->GetDevice(), timeline, &value);
    if (result != VK_SUCCESS) {
        m_device->TryLatchFirstFault(
            VulkanOperationContext{.operation = EVulkanFaultOperation::TimelineHostWait}, result
        );
    }
    // vkGetSemaphoreCounterValue(m_device->GetDevice(), m_fence.timeline, &value);
    return value;
}

void VulkanTimeline::Wait(uint64_t _value) {
    std::unique_lock<std::mutex> _(cv_m);
    while (current_value < _value && !rejected_values.contains(_value) && !failed && !m_device->IsFaulted()) {
        cv.wait_for(_, std::chrono::milliseconds(50));
    }
    if (current_value < _value && !rejected_values.contains(_value) && !failed && m_device->IsFaulted()) {
        failed         = true;
        failure_result = m_device->GetFirstFaultResult();
    }
}

void VulkanTimeline::Reject(uint64_t _value) noexcept {
    {
        std::unique_lock<std::mutex> lock(cv_m);
        try {
            rejected_values.emplace(_value);
        } catch (...) {
            // A rejected value must always become terminal. If the exact
            // value set cannot grow, conservatively fail the whole fence
            // rather than leaving submission waiters blocked forever.
            failed         = true;
            failure_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    cv.notify_all();
}

void VulkanTimeline::Notify(uint64_t _value) {
    {
        std::unique_lock<std::mutex> _(cv_m);
        current_value   = std::max(current_value, _value);
        submitted_value = std::max(submitted_value, _value);
    }
    cv.notify_all();
}

void VulkanTimeline::MarkSubmitted(uint64_t _value) {
    {
        std::unique_lock<std::mutex> lock(cv_m);
        submitted_value = std::max(submitted_value, _value);
    }
    cv.notify_all();
}

bool VulkanTimeline::WaitSubmitted(
    uint64_t                _value,
    const std::atomic_bool* _continue_waiting
) {
    std::unique_lock<std::mutex> lock(cv_m);
    while (submitted_value < _value && !rejected_values.contains(_value) && !failed &&
           !m_device->IsFaulted() &&
           (_continue_waiting == nullptr || _continue_waiting->load(std::memory_order_acquire))) {
        cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return submitted_value >= _value && !rejected_values.contains(_value) && !failed &&
           !m_device->IsFaulted();
}

bool VulkanTimeline::IsRejected(uint64_t _value) const {
    std::unique_lock<std::mutex> lock(cv_m);
    return rejected_values.contains(_value);
}

void VulkanTimeline::Sync(uint64_t _value) {
    std::unique_lock<std::mutex> _(cv_m);
    while (current_value < _value && !rejected_values.contains(_value) && !failed && !m_device->IsFaulted()) {
        cv.wait_for(_, std::chrono::milliseconds(50));
    }
    if (current_value < _value && !rejected_values.contains(_value) && !failed && m_device->IsFaulted()) {
        failed         = true;
        failure_result = m_device->GetFirstFaultResult();
    }
}

VkResult VulkanTimeline::HostWait(uint64_t _value, const VulkanOperationContext& _context) {
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount              = 1;
    info.pValues                     = &_value;
    info.pSemaphores                 = &timeline;
    constexpr uint64_t wait_slice_ns = 50'000'000;
    while (!m_device->IsDeviceLost()) {
        {
            std::unique_lock<std::mutex> lock(cv_m);
            if (rejected_values.contains(_value)) {
                return VK_ERROR_UNKNOWN;
            }
            if (failed) {
                return failure_result != VK_SUCCESS ? failure_result : VK_ERROR_UNKNOWN;
            }
        }
        const VkResult result = vkWaitSemaphores(m_device->GetDevice(), &info, wait_slice_ns);
        if (result == VK_SUCCESS) {
            return VK_SUCCESS;
        }
        if (result == VK_TIMEOUT) {
            continue;
        }

        VulkanOperationContext context = _context;
        if (context.operation == EVulkanFaultOperation::None) {
            context.operation = EVulkanFaultOperation::TimelineHostWait;
        }
        context.timeline = _value;
        m_device->TryLatchFirstFault(context, result);
        if (result != VK_ERROR_DEVICE_LOST) {
            m_device->EmergencyExitWithoutVulkanCleanup(context, result);
        }
        Fail(result);
        return result;
    }
    const VkResult result = VK_ERROR_DEVICE_LOST;
    Fail(result);
    return result;
}

void VulkanTimeline::Fail(VkResult _result) {
    {
        std::unique_lock<std::mutex> lock(cv_m);
        failed         = true;
        failure_result = _result;
    }
    cv.notify_all();
}

bool VulkanTimeline::IsFailed() const {
    std::unique_lock<std::mutex> lock(cv_m);
    return failed;
}

void VulkanTimeline::SignalHost(uint64_t _value) {
    VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    info.semaphore = timeline;
    info.value     = _value;
    if (m_device->IsFaulted()) {
        Fail(m_device->GetFirstFaultResult());
        return;
    }
    const VkResult result = vkSignalSemaphore(m_device->GetDevice(), &info);
    if (result != VK_SUCCESS) {
        m_device->TryLatchFirstFault(
            VulkanOperationContext{.operation = EVulkanFaultOperation::TimelineHostWait}, result
        );
        Fail(result);
        return;
    }
    Notify(_value);
}

VulkanTimeline::~VulkanTimeline() {
    if (timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device->GetDevice(), timeline, VK_NULL_HANDLE);
    }
}

} // namespace Moer::Render
