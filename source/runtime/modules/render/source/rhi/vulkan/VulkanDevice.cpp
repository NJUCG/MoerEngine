//
// Created by 74535 on 2023/10/2.
//

#include "misc/VulkanMacroUtils.h"
#include "VulkanDevice.h"
#include "VulkanUtil.h"

#include <set>

namespace VUtil = MoerEngine::RHI::Vulkan::Util;

/**
 * Initialize GPU, GPU related resources, create the logical device, etc.
 * @param DeviceInitializer
 */
void VulkanDevice::Init(const DeviceInitializer& _initializer) {
    m_gpu = SelectGpu(_initializer.instance, _initializer.gpu_type, _initializer.surface, _initializer.enabled_extensions);
    if (m_gpu == VK_NULL_HANDLE) {
        VUtil::ExitFatal("No available GPU found.", -1);
    }
    vkGetPhysicalDeviceProperties(m_gpu, &m_gpu_props);
    vkGetPhysicalDeviceFeatures(m_gpu, &m_gpu_features);
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &m_gpu_mem_props);
    m_gpu_extensions       = GetGpuExtensions(m_gpu);
    m_queue_family_indices = QueryQueueFamilyIndices(m_gpu, _initializer.surface);

    MOER_LOG_INFO("- DeviceName: {}. \n"
                  "- API={}.{}.{} (0x{:x}) Driver=0x{:x} VendorId=0x{:x}. \n"
                  "- DeviceID=0x{:x} Type={}. \n"
                  "- Max Descriptor Sets Bound {}, Timestamps {}.",
                  m_gpu_props.deviceName,
                  VK_API_VERSION_MAJOR(m_gpu_props.apiVersion),
                  VK_API_VERSION_MINOR(m_gpu_props.apiVersion),
                  VK_API_VERSION_PATCH(m_gpu_props.apiVersion),
                  m_gpu_props.apiVersion,
                  m_gpu_props.driverVersion,
                  m_gpu_props.vendorID,
                  m_gpu_props.deviceID,
                  std::to_string(m_gpu_props.deviceType),
                  m_gpu_props.limits.maxBoundDescriptorSets,
                  m_gpu_props.limits.timestampComputeAndGraphics);

    CreateDevice(_initializer);
}

void VulkanDevice::Destroy() {
}

/**
 * Select gpu, check extension support, etc.
 * @param _instance
 * @param _type
 * @param _surface
 * @param _enabled_extensions
 * @return selected gpu
 */
VkPhysicalDevice VulkanDevice::SelectGpu(VkInstance _instance, VkPhysicalDeviceType _type, VkSurfaceKHR _surface, const std::vector<const char*>& _enabled_extensions) {
    uint32_t gpu_count = 0;
    VK_CHECK_RESULT(vkEnumeratePhysicalDevices(_instance, &gpu_count, nullptr))
    if (gpu_count == 0) {
        MOER_LOG_WARN("No GPU with Vulkan support found!");
        return VK_NULL_HANDLE;
    }
    std::vector<VkPhysicalDevice> gpu_list(gpu_count);
    VK_CHECK_RESULT(vkEnumeratePhysicalDevices(_instance, &gpu_count, gpu_list.data()))

    for (const auto& gpu : gpu_list) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        bool type_matched         = props.deviceType == _type;
        bool extensions_supported = CheckEnabledExtensionsSupported(gpu, _enabled_extensions);
        bool swap_chain_adequate  = false;
        if (extensions_supported) {
            auto details        = VUtil::QuerySwapChainSupport(gpu, _surface);
            swap_chain_adequate = !details.formats.empty() && !details.present_modes.empty();
        }

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(gpu, &features);

        auto indices = QueryQueueFamilyIndices(gpu, _surface);

        if (indices.IsComplete() && extensions_supported && swap_chain_adequate && type_matched) {
            return gpu;
        }
    }

    MOER_LOG_WARN("No available target type (discrete, etc.) GPU found!");

    return VK_NULL_HANDLE;
}

void VulkanDevice::CreateDevice(const DeviceInitializer& _initializer) {
    std::set<uint32_t> unique_family_indices = {m_queue_family_indices.graphics.value(), m_queue_family_indices.present.value(), m_queue_family_indices.compute.value(), m_queue_family_indices.transfer.value()};

    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;

    const float default_queue_priority = 1.0f;
    for (const auto& queue_family_index : unique_family_indices) {
        VkDeviceQueueCreateInfo queue_create_info{};
        queue_create_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info.queueFamilyIndex = queue_family_index;
        queue_create_info.queueCount       = 1;
        queue_create_info.pQueuePriorities = &default_queue_priority;

        queue_create_infos.push_back(queue_create_info);
    }

    VkDeviceCreateInfo device_create_info{};
    device_create_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
    device_create_info.pQueueCreateInfos    = queue_create_infos.data();

    if (!_initializer.enabled_extensions.empty()) {
        device_create_info.enabledExtensionCount   = static_cast<uint32_t>(_initializer.enabled_extensions.size());
        device_create_info.ppEnabledExtensionNames = _initializer.enabled_extensions.data();
    }

    VkPhysicalDeviceFeatures2 gpu_features2{};
    if (_initializer.p_next_chain) {
        gpu_features2.sType                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        gpu_features2.pNext                 = _initializer.p_next_chain;
        gpu_features2.features              = _initializer.enabled_features;
        device_create_info.pEnabledFeatures = nullptr;
        device_create_info.pNext            = &gpu_features2;
    } else {
        device_create_info.pEnabledFeatures = &_initializer.enabled_features;
        device_create_info.pNext            = nullptr;
    }

    VK_CHECK_RESULT(vkCreateDevice(m_gpu, &device_create_info, nullptr, &m_device));

    vkGetDeviceQueue(m_device, m_queue_family_indices.graphics.value(), 0, &m_graphics_queue);
    vkGetDeviceQueue(m_device, m_queue_family_indices.present.value(), 0, &m_present_queue);
    vkGetDeviceQueue(m_device, m_queue_family_indices.compute.value(), 0, &m_compute_queue);
    vkGetDeviceQueue(m_device, m_queue_family_indices.transfer.value(), 0, &m_transfer_queue);
}

std::vector<const char*> VulkanDevice::GetGpuExtensions(VkPhysicalDevice _gpu) const {
    uint32_t gpu_extension_count;
    // check extensions
    vkEnumerateDeviceExtensionProperties(_gpu, nullptr, &gpu_extension_count, nullptr);
    std::vector<VkExtensionProperties> gpu_extensions(gpu_extension_count);
    vkEnumerateDeviceExtensionProperties(_gpu, nullptr, &gpu_extension_count, gpu_extensions.data());

    std::vector<const char*> ret;
    for (const auto& extension : gpu_extensions) {
        ret.push_back(extension.extensionName);
    }

    return ret;
}

VkPhysicalDeviceMemoryProperties VulkanDevice::GetMemoryProperties(VkPhysicalDevice _gpu) const {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(_gpu, &props);
    return props;
}

//uint32_t VulkanDevice::GetMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties, VkBool32* mem_type_found) const {
//    return 0;
//}

int32_t VulkanDevice::GetQueueFamilyIndex(const std::vector<VkQueueFamilyProperties>& queue_family_props, VkQueueFlags _queue_flags) const {
    // Dedicated queue for transfer
    if ((_queue_flags & VK_QUEUE_TRANSFER_BIT) == _queue_flags) {
        for (uint32_t i = 0; i < queue_family_props.size(); ++i) {
            if ((queue_family_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && (queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                return i;
            }
        }
    }

    // Dedicated queue for compute
    if ((_queue_flags & VK_QUEUE_COMPUTE_BIT) == _queue_flags) {
        for (uint32_t i = 0; i < queue_family_props.size(); ++i) {
            if ((queue_family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 && (queue_family_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) == 0) {
                return i;
            }
        }
    }

    // Default queue
    for (uint32_t i = 0; i < queue_family_props.size(); ++i) {
        if ((queue_family_props[i].queueFlags & _queue_flags) == _queue_flags) {
            return i;
        }
    }

    return -1;
    // CRITICAL_AND_THROW("No suitable queue family found for " + std::to_string(_queue_flags));
}

QueueFamilyIndices VulkanDevice::QueryQueueFamilyIndices(VkPhysicalDevice _gpu, VkSurfaceKHR _surface) const {
    QueueFamilyIndices indices;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_gpu, &queue_family_count, nullptr);
    assert(queue_family_count > 0);
    std::vector<VkQueueFamilyProperties> queue_family_props(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(_gpu, &queue_family_count, queue_family_props.data());

    auto graphics = GetQueueFamilyIndex(queue_family_props, VK_QUEUE_GRAPHICS_BIT);
    if (graphics > 0) {
        indices.graphics = graphics;
    }
    auto transfer = GetQueueFamilyIndex(queue_family_props, VK_QUEUE_TRANSFER_BIT);
    if (transfer > 0) {
        indices.transfer = transfer;
    } else {
        indices.transfer = indices.graphics;
    }
    auto compute = GetQueueFamilyIndex(queue_family_props, VK_QUEUE_COMPUTE_BIT);
    if (compute > 0) {
        indices.compute = compute;
    } else {
        indices.compute = indices.transfer;
    }

    for (uint32_t i = 0; i < queue_family_count; ++i) {
        VkBool32 present_supported = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(_gpu, i, _surface, &present_supported);
        if (present_supported) {
            indices.present = i;
            break;
        }
    }

    return indices;
}

bool VulkanDevice::CheckEnabledExtensionsSupported(VkPhysicalDevice _gpu, const std::vector<const char*>& _enabled_extensions) const {
    auto gpu_extensions = GetGpuExtensions(_gpu);

    std::set<const char*> enabled_extensions(_enabled_extensions.begin(), _enabled_extensions.end());
    for (const auto& extension : gpu_extensions) {
        enabled_extensions.erase(extension);
    }

    if (!enabled_extensions.empty()) {
        for (const auto& extension : enabled_extensions) {
            MOER_LOG_CRITICAL("Enabled GPU extension '" + std::string(extension) + "' is not supported!");
        }
        return false;
    }

    return true;
}
