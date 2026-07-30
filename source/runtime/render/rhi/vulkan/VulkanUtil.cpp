//
// Created by 74535 on 2023/10/1.
//

#include "misc/Crc32.h"

#include "VulkanPlatform.h"
#include "VulkanUtil.h"

namespace Moer { namespace RHI { namespace Vulkan { namespace Util {

namespace {

template<typename Value, typename Query>
VkResult EnumerateSurfaceValues(Query&& query, Moer::Array<Value>& values) {
    constexpr uint32_t max_attempts = 4;
    for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
        uint32_t count  = 0;
        VkResult result = query(&count, nullptr);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            values.clear();
            return result;
        }
        if (count == 0) {
            values.clear();
            return VK_SUCCESS;
        }

        values.resize(count);
        uint32_t fetched_count = count;
        result                 = query(&fetched_count, values.data());
        if (result == VK_INCOMPLETE) {
            continue;
        }
        if (result != VK_SUCCESS) {
            values.clear();
            return result;
        }
        values.resize(fetched_count);
        return VK_SUCCESS;
    }
    values.clear();
    return VK_INCOMPLETE;
}

} // namespace

SwapChainSupportQueryResult
QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface, uint32_t _present_queue_family) {
    SwapChainSupportQueryResult query_result{};

    VkBool32 present_supported = VK_FALSE;
    VkResult result            = vkGetPhysicalDeviceSurfaceSupportKHR(
        _gpu, _present_queue_family, _surface, &present_supported
    );
    if (result != VK_SUCCESS) {
        query_result.native_result = result;
        return query_result;
    }
    if (present_supported != VK_TRUE) {
        query_result.status        = ESwapChainSupportQueryStatus::PresentUnsupported;
        query_result.native_result = VK_ERROR_INITIALIZATION_FAILED;
        return query_result;
    }

    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        _gpu, _surface, &query_result.details.capabilities
    );
    if (result != VK_SUCCESS) {
        query_result.native_result = result;
        return query_result;
    }

    result = EnumerateSurfaceValues<VkSurfaceFormatKHR>(
        [_gpu, _surface](uint32_t* count, VkSurfaceFormatKHR* values) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, count, values);
        },
        query_result.details.formats
    );
    if (result != VK_SUCCESS) {
        query_result.status = result == VK_INCOMPLETE ?
                                  ESwapChainSupportQueryStatus::EnumerationIncomplete :
                                  ESwapChainSupportQueryStatus::NativeFailure;
        query_result.native_result = result;
        return query_result;
    }
    if (query_result.details.formats.empty()) {
        query_result.status        = ESwapChainSupportQueryStatus::NoSurfaceFormats;
        query_result.native_result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        return query_result;
    }

    result = EnumerateSurfaceValues<VkPresentModeKHR>(
        [_gpu, _surface](uint32_t* count, VkPresentModeKHR* values) {
            return vkGetPhysicalDeviceSurfacePresentModesKHR(_gpu, _surface, count, values);
        },
        query_result.details.present_modes
    );
    if (result != VK_SUCCESS) {
        query_result.status = result == VK_INCOMPLETE ?
                                  ESwapChainSupportQueryStatus::EnumerationIncomplete :
                                  ESwapChainSupportQueryStatus::NativeFailure;
        query_result.native_result = result;
        return query_result;
    }
    if (query_result.details.present_modes.empty()) {
        query_result.status        = ESwapChainSupportQueryStatus::NoPresentModes;
        query_result.native_result = VK_ERROR_INITIALIZATION_FAILED;
        return query_result;
    }

    query_result.status        = ESwapChainSupportQueryStatus::Success;
    query_result.native_result = VK_SUCCESS;
    return query_result;
}

uint32_t MemCrc32(const void* data, size_t data_size) {
    return crc32_8bytes(data, data_size);
}
}}}} // namespace Moer::RHI::Vulkan::Util
