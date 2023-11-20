//
// Created by 74535 on 2023/10/2.
//

#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "VulkanDescriptor.h"
#include "VulkanExtension.h"
#include "VulkanDevice.h"
#include "VulkanUtil.h"

#include <set>
#include <string>
#include <vector>

namespace VkUtil = Moer::RHI::Vulkan::Util;

VulkanDevice::VulkanDevice()
    : m_gpu(VK_NULL_HANDLE), m_gpu_props(), m_gpu_features(), m_gpu_mem_props(), m_gpu_extensions(), m_queue_family_props(), m_queue_family_indices(),
      m_device(VK_NULL_HANDLE), m_graphics_queue(VK_NULL_HANDLE), m_present_queue(VK_NULL_HANDLE), m_compute_queue(VK_NULL_HANDLE), m_transfer_queue(VK_NULL_HANDLE),
      m_default_pool(VK_NULL_HANDLE), m_transfer_pool(VK_NULL_HANDLE),
      m_allocator(VK_NULL_HANDLE), m_descriptor_allocator(nullptr) {}

/**
 * Initialize GPU, GPU related resources, create the logical device, etc.
 * @param DeviceInitializer
 */
void VulkanDevice::Init(const DeviceInitializer& _initializer) {
    m_gpu = SelectGpu(_initializer);
    if (m_gpu == VK_NULL_HANDLE) {
        VkUtil::ExitFatal("No available GPU found.", -1);
    }

    InitGpu(_initializer);

    LOG_INFO("\n- DeviceName: {}."
             "\n- API={}.{}.{} (0x{:x}) Driver=0x{:x} VendorId=0x{:x}."
             "\n- DeviceID=0x{:x} Type={}."
             "\n- Max Descriptor Sets Bound {}, Timestamps {}.",
             m_gpu_props.properties.deviceName,
             VK_API_VERSION_MAJOR(m_gpu_props.properties.apiVersion),
             VK_API_VERSION_MINOR(m_gpu_props.properties.apiVersion),
             VK_API_VERSION_PATCH(m_gpu_props.properties.apiVersion),
             m_gpu_props.properties.apiVersion,
             m_gpu_props.properties.driverVersion,
             m_gpu_props.properties.vendorID,
             m_gpu_props.properties.deviceID,
             std::to_string(m_gpu_props.properties.deviceType),
             m_gpu_props.properties.limits.maxBoundDescriptorSets,
             m_gpu_props.properties.limits.timestampComputeAndGraphics);

    CreateDevice(_initializer);
    CreateCommandPools();
    CreateDescriptorAllocator();
}

void VulkanDevice::InitMemoryAllocator(VkInstance _instance) {
    VmaAllocatorCreateInfo alloc_create_info{};

    VmaVulkanFunctions vma_functions{};
    vma_functions.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)vkGetInstanceProcAddr;
    vma_functions.vkGetDeviceProcAddr   = (PFN_vkGetDeviceProcAddr)vkGetDeviceProcAddr;

    alloc_create_info.vulkanApiVersion = VK_API_VERSION_1_3;

    alloc_create_info.instance         = _instance;
    alloc_create_info.physicalDevice   = m_gpu;
    alloc_create_info.device           = m_device;
    alloc_create_info.pVulkanFunctions = &vma_functions;

    //capable of using buffer via device address(64bit) passed to shader.
    alloc_create_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VK_CHECK_RESULT(vmaCreateAllocator(&alloc_create_info, &m_allocator));

    LOG_INFO("Vulkan Memory Allocator initialized with api version: {}.", alloc_create_info.vulkanApiVersion);
}

void VulkanDevice::Destroy() {
    vmaDestroyAllocator(m_allocator);
}

bool VulkanDevice::GetDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, std::vector<VulkanDescriptorSetWriter>& _writers, std::vector<VkDescriptorSet>& _sets) {
    return m_descriptor_allocator->GetDescriptorSets(_hash_key, _layout, _writers, _sets);
}

/**
 * Select gpu, check extension support, etc.
 * @param _initializer
 * @return selected gpu
 * only check core extensions and core features support.
 */
VkPhysicalDevice VulkanDevice::SelectGpu(const DeviceInitializer& _init) {
    uint32_t gpu_count = 0;
    VK_CHECK_RESULT(vkEnumeratePhysicalDevices(_init.instance, &gpu_count, nullptr))
    if (gpu_count == 0) {
        LOG_WARNING("No GPU with Vulkan support found!");
        return VK_NULL_HANDLE;
    }
    std::vector<VkPhysicalDevice> gpu_list(gpu_count);
    VK_CHECK_RESULT(vkEnumeratePhysicalDevices(_init.instance, &gpu_count, gpu_list.data()))

    for (const auto& gpu : gpu_list) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        bool type_matched              = props.deviceType == _init.gpu_type;
        bool core_extensions_supported = CheckEnabledExtensionsSupported(gpu, _init.enabled_extensions);
        bool core_features_supported   = CheckEnabledFeaturesSupported(gpu, _init.enabled_features, _init.api_version);
        bool swap_chain_adequate       = false;
        if (core_extensions_supported) {
            auto details        = VkUtil::QuerySwapChainSupport(gpu, _init.surface);
            swap_chain_adequate = !details.formats.empty() && !details.present_modes.empty();
        }

        auto indices = QueryQueueFamilyIndices(gpu, _init.surface);

        if (indices.IsComplete() && swap_chain_adequate && core_extensions_supported && type_matched) {
            return gpu;
        }
    }

    LOG_WARNING("No available target type (discrete, etc.) GPU found!");

    return VK_NULL_HANDLE;
}

void VulkanDevice::InitGpu(const DeviceInitializer& _initializer) {
    // Query advanced properties.
    {
        m_gpu_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        for (const auto& extension : _initializer.enabled_extensions) {
            extension->PreGpuProperties(m_gpu_props);
        }
        vkGetPhysicalDeviceProperties2(m_gpu, &m_gpu_props);
    }

    // Query advanced features, use advanced features as GPU supported, and developers cannot specify them.
    {
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        for (const auto& extension : _initializer.enabled_extensions) {
            extension->PreGpuFeatures(features2);
        }
        vkGetPhysicalDeviceFeatures2(m_gpu, &features2);
    }

    m_gpu_features.Query(m_gpu, _initializer.api_version);

    m_gpu_mem_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    vkGetPhysicalDeviceMemoryProperties2(m_gpu, &m_gpu_mem_props);

    m_gpu_extensions       = GetGpuExtensions(m_gpu);
    m_queue_family_indices = QueryQueueFamilyIndices(m_gpu, _initializer.surface);
    m_queue_family_props   = GetQueueFamilyProperties(m_gpu);

    LOG_INFO("VulkanRHI: GPU initialized.");
}

void VulkanDevice::CreateDevice(const DeviceInitializer& _initializer) {
    std::set<uint32_t> unique_family_indices = {m_queue_family_indices.graphics.value(), m_queue_family_indices.present.value(), m_queue_family_indices.compute.value(), m_queue_family_indices.transfer.value()};

    // setup queue info
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

    // setup extension and feature info
    std::vector<const char*> enabled_extensions;
    for (const auto& extension : _initializer.enabled_extensions) {
        enabled_extensions.emplace_back(extension->GetExtensionName().c_str());
        extension->PreCreateDevice(device_create_info);
    }

    if (!enabled_extensions.empty()) {
        device_create_info.enabledExtensionCount   = static_cast<uint32_t>(enabled_extensions.size());
        device_create_info.ppEnabledExtensionNames = enabled_extensions.data();
    }
    VkPhysicalDeviceFeatures2 enabled_features;
    if (_initializer.api_version > VK_API_VERSION_1_0) {
        VulkanPhysicalDeviceFeatures features(_initializer.enabled_features);
        enabled_features.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        enabled_features.features = features.core_1_0;
        enabled_features.pNext    = &features.core_1_1;
        features.PreCreateDevice(device_create_info, _initializer.api_version);
        device_create_info.pNext = &enabled_features;
    } else {
        device_create_info.pEnabledFeatures = &_initializer.enabled_features.core_1_0;
    }

    VK_CHECK_RESULT(vkCreateDevice(m_gpu, &device_create_info, nullptr, &m_device));

    vkGetDeviceQueue(m_device, m_queue_family_indices.graphics.value(), 0, &m_graphics_queue);
    vkGetDeviceQueue(m_device, m_queue_family_indices.present.value(), 0, &m_present_queue);
    vkGetDeviceQueue(m_device, m_queue_family_indices.compute.value(), 0, &m_compute_queue);
    vkGetDeviceQueue(m_device, m_queue_family_indices.transfer.value(), 0, &m_transfer_queue);
}

void VulkanDevice::CreateCommandPools() {
    VkCommandPoolCreateInfo pool_create_info{};
    pool_create_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_create_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_create_info.queueFamilyIndex = m_queue_family_indices.graphics.value();

    VK_CHECK_RESULT(vkCreateCommandPool(m_device, &pool_create_info, nullptr, &m_default_pool));

    pool_create_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_create_info.queueFamilyIndex = m_queue_family_indices.transfer.value();
    VK_CHECK_RESULT(vkCreateCommandPool(m_device, &pool_create_info, nullptr, &m_transfer_pool));

    LOG_INFO("VulkanRHI: Command pools created, graphics: {}, transfer: {}", (void*)m_default_pool, (void*)m_transfer_pool);
}

void VulkanDevice::CreateDescriptorAllocator() {
    m_descriptor_allocator = new VulkanDescriptorSetAllocator();
    m_descriptor_allocator->Init(this);
    LOG_INFO("VulkanRHI: Descriptor allocator created.");
}

TExtensionArray VulkanDevice::GetGpuExtensions(VkPhysicalDevice _gpu) const {
    uint32_t gpu_extension_count;
    // check extensions
    vkEnumerateDeviceExtensionProperties(_gpu, nullptr, &gpu_extension_count, nullptr);
    std::vector<VkExtensionProperties> gpu_extensions(gpu_extension_count);
    vkEnumerateDeviceExtensionProperties(_gpu, nullptr, &gpu_extension_count, gpu_extensions.data());

    TExtensionArray ret;
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

TQueueFamilyPropertiesArray VulkanDevice::GetQueueFamilyProperties(VkPhysicalDevice _gpu) const {
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_gpu, &queue_family_count, nullptr);
    assert(queue_family_count > 0);
    TQueueFamilyPropertiesArray queue_family_props(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(_gpu, &queue_family_count, queue_family_props.data());

    return queue_family_props;
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

    auto queue_family_props = GetQueueFamilyProperties(_gpu);

    auto graphics = GetQueueFamilyIndex(queue_family_props, VK_QUEUE_GRAPHICS_BIT);
    if (graphics >= 0) {
        indices.graphics = graphics;
    }
    auto transfer = GetQueueFamilyIndex(queue_family_props, VK_QUEUE_TRANSFER_BIT);
    if (transfer >= 0) {
        indices.transfer = transfer;
    } else {
        indices.transfer = indices.graphics;
    }
    auto compute = GetQueueFamilyIndex(queue_family_props, VK_QUEUE_COMPUTE_BIT);
    if (compute >= 0) {
        indices.compute = compute;
    } else {
        indices.compute = indices.transfer;
    }

    for (uint32_t i = 0; i < queue_family_props.size(); ++i) {
        VkBool32 present_supported = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(_gpu, i, _surface, &present_supported);
        if (present_supported) {
            indices.present = i;
            break;
        }
    }

    return indices;
}

bool VulkanDevice::CheckEnabledExtensionsSupported(VkPhysicalDevice _gpu, const TVulkanDeviceExtensionArray& _enabled_extensions) const {
    auto gpu_extensions = GetGpuExtensions(_gpu);

    bool supported = true;
    for (const auto& extension : _enabled_extensions) {
        if (std::find(gpu_extensions.begin(), gpu_extensions.end(), extension->GetExtensionName()) == gpu_extensions.end()) {
            LOG_WARNING("Enabled GPU extension '" + extension->GetExtensionName() + "' is not supported!");
            if (extension->IsOptional()) {
                supported = false;
            }
        }
    }

    return supported;
}

bool VulkanDevice::CheckEnabledFeaturesSupported(VkPhysicalDevice _gpu, const VulkanPhysicalDeviceFeatures& _enabled_features, uint32_t _api_version) {
    m_gpu_features.Query(_gpu, _api_version);

    return m_gpu_features.Contains(_enabled_features);
}