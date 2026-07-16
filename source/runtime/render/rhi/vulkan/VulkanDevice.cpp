//
// Created by 74535 on 2023/10/2.
//

#include "VulkanDevice.h"
#include "PixelFormat.h"
#include "VulkanCommand.h"
#include "VulkanDebugCallback.h"
#include "VulkanMacroUtils.h"
#include "VulkanPlatform.h"
#include "VulkanQueue.h"
#include "VulkanRHIResource.h"
#include "VulkanUtil.h"
#include "plugin/VulkanCooperativeSupport.h"
#include "plugin/VulkanNrdPlugin.h"
#include "vulkanextension/VulkanExtension.h"
#include <string_view>

#include "log/LogSystem.h"
// #include "misc/MacroUtils.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"

#include "Core.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/ThreadManager.h"
#include "vulkan/vk_enum_string_helper.h"
#include "vulkan/vulkan_core.h"

// #include <vk_mem_alloc.h>
#include "VulkanMemoryAllocator.h"

#if PLATFORM_WINDOWS
#include <vulkan/vulkan_win32.h>
#endif

#include "VulkanIOService.h"
#include <algorithm>
#include <config.h>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <platform/Platform.h>
#include <shared_mutex>
#include <variant>

#ifndef MOER_STR
#define MOER_STR(x)  #x
#define MOER_XSTR(x) MOER_STR(x)
#endif

namespace Moer::Render {
namespace VkUtil = Moer::RHI::Vulkan::Util;

VulkanDevice::VulkanDevice(const VulkanRHIConfig&& _config) : RenderDevice::Impl() {
    rhi_thread_enabled      = _config.rhi_thread && !_config.rhi_bypass;
    thread_profile_logging = _config.thread_profile_logging;
    present_submit_fault_trigger = _config.present_submit_fault_trigger;
    if (present_submit_fault_trigger != 0) {
        LOG_INFO(
            "[VulkanFault][Config] point=present-submit trigger={} mode=synthetic-device-lost",
            present_submit_fault_trigger
        );
    }
    if (_config.rhi_thread && _config.rhi_bypass) {
        LOG_INFO("[Threading] Vulkan RHI Thread bypass is enabled; graphics submissions stay synchronous.");
    } else if (!_config.rhi_thread && !_config.rhi_bypass) {
        LOG_INFO("[Threading] rhi_bypass=false is ignored while rhi_thread=false.");
    }

    InitVulkanInstance(_config.api_version);

    m_gpu = SelectGpu(_config.api_version);
    InitGpu(_config.api_version);

    CreateDevice(_config.api_version);
    CreateMemoryAllocator(m_instance, _config.api_version);

    CreateInternalResources();

    LoadDefaultExtensions();
}

void VulkanDevice::PostInit() {
    CreateInternalShaders();
}

VulkanDevice::~VulkanDevice() {
    Destroy();
}

/**
     * @brief init vulkan instance
     * check instance layers and extensions, create instance, load functions using volk
     * @param _api_version
     */
void VulkanDevice::InitVulkanInstance(uint32 _api_version) {
    VK_CHECK_RESULT(volkInitialize());

    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // application_info.pApplicationName = MACRO_STR(__ENGINE_NAME__);
    // application_info.applicationVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    application_info.pEngineName = MACRO_STR(__ENGINE_NAME__);
    application_info.engineVersion =
        VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    application_info.apiVersion = _api_version;

    VkInstanceCreateInfo instance_create_info{};
    instance_create_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pNext            = nullptr;
    instance_create_info.flags            = 0;
    instance_create_info.pApplicationInfo = &application_info;

    // instance layer
    constexpr std::string_view vk_layer_path = MOER_XSTR(MOER_VK_LAYER_PATH);
    std::filesystem::path      layer_path(vk_layer_path);
    if (std::filesystem::exists(layer_path)) {
        Platform::SetEnv("VK_LAYER_PATH", MOER_XSTR(MOER_VK_LAYER_PATH));
        LOG_INFO("Set VK_LAYER_PATH to {}", MOER_XSTR(MOER_VK_LAYER_PATH));
    }

    const auto instance_layers = [&]() {
        uint32 instance_layer_count = 0;
        vkEnumerateInstanceLayerProperties(&instance_layer_count, nullptr);
        Array<VkLayerProperties> instance_layer_properties(instance_layer_count);
        vkEnumerateInstanceLayerProperties(&instance_layer_count, instance_layer_properties.data());

        Set<std::string> layers;
        for (auto layer_property : instance_layer_properties)
            layers.insert(layer_property.layerName);

        return layers;
    }();

    const auto& is_layer_valid = [&](std::string_view _view) {
        if (!instance_layers.contains(_view.data())) {
            LOG_WARNING("Layer '{}' is not supported.", _view.data());
            return false;
        }
        return true;
    };

    TLayerArray instance_layers_required;
    VulkanPlatform::GetInstanceLayers(instance_layers_required);

    VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};
    Array<const char*>                 instance_layers_loaded;
    bool                               b_validation_layer_enabled = false;

    for (const auto& layer : instance_layers_required) {
        if (is_layer_valid(layer)) {
            if (layer == "VK_LAYER_KHRONOS_validation") {
                PopulateDebugMessengerCreateInfo(debug_create_info);
                debug_create_info.pNext    = instance_create_info.pNext;
                instance_create_info.pNext = &debug_create_info;
                b_validation_layer_enabled = true;
            }
            instance_layers_loaded.emplace_back(layer.data());
        }
        // other layers, not fully implemented
    }
    instance_create_info.enabledLayerCount   = instance_layers_loaded.size();
    instance_create_info.ppEnabledLayerNames = instance_layers_loaded.data();

    // instance extension
    const auto instance_extensions = [&]() {
        Set<std::string> extensions;

        uint32_t prop_count = 0;
        VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(nullptr, &prop_count, nullptr));
        Array<VkExtensionProperties> extension_props;
        if (prop_count > 0) {
            extension_props.resize(prop_count);
            VK_CHECK_RESULT(
                vkEnumerateInstanceExtensionProperties(nullptr, &prop_count, extension_props.data())
            );
            for (const auto& prop : extension_props) {
                extensions.insert(prop.extensionName);
            }
        }

        return extensions;
    }();

    const auto& is_extension_supported = [&](const std::string& _ext) {
        if (!instance_extensions.contains(_ext)) {
            LOG_ERROR("Reqired instance extension '{}' is not supported", _ext);
            return false;
        }
        return true;
    };

    const auto instance_extensions_required = VulkanInstanceExtension::GetMERequiredInstanceExtensions();

    Array<const char*> instance_extensions_loaded;
    bool               b_instance_extensions_fully_supported = true;
    for (const auto& ext : instance_extensions_required) {
        if (is_extension_supported(ext.data()))
            instance_extensions_loaded.emplace_back(ext.data());
        else
            b_instance_extensions_fully_supported = false;
    }
    CHECK_ASSERT(
        b_instance_extensions_fully_supported, "Not all required instance extensions are supported."
    );

    instance_create_info.enabledExtensionCount   = instance_extensions_loaded.size();
    instance_create_info.ppEnabledExtensionNames = instance_extensions_loaded.data();

    VK_CHECK_RESULT(vkCreateInstance(&instance_create_info, nullptr, &m_instance))
    volkLoadInstance(m_instance);

    if (b_validation_layer_enabled)
        SetupDebugUtilsMessengerEXT();
}

/**
    * @brief Select gpu, check extension support, etc.
    * Only check core extensions and core features support.
    * @param _api_version
    * @return selected gpu
    */
VkPhysicalDevice VulkanDevice::SelectGpu(uint32 _api_version) {
    uint32 gpu_count = 0;
    VK_CHECK_RESULT(vkEnumeratePhysicalDevices(m_instance, &gpu_count, nullptr))
    CHECK_ASSERT(gpu_count, "No GPU with Vulkan support found!");

    Array<VkPhysicalDevice> gpu_list(gpu_count);
    VK_CHECK_RESULT(vkEnumeratePhysicalDevices(m_instance, &gpu_count, gpu_list.data()))

    // lambda helpers
    const auto& extensions_required = VulkanDeviceExtension::GetMERequiredDeviceExtensions();

    const auto& is_extensions_required_supported = [&](VkPhysicalDevice _gpu) {
        // 只检查 GPU 是否声明支持对应扩展，不检查 feature 结构体细节
        auto gpu_extensions = VulkanDevice::GetGpuExtensions(_gpu);

        for (const auto& extension : extensions_required) {
            const auto& required_name = extension->GetExtensionName();
            const bool  is_optional   = extension->IsOptional();

            if (!gpu_extensions.contains(required_name.data()) && !is_optional) {
                return false;
            }
        }

        return true;
    };

    const auto& features_required          = VulkanDeviceFeatures::GetMERequiredFeatures(_api_version);
    const auto& is_core_features_supported = [&](VkPhysicalDevice _gpu) {
        auto gpu_features = VulkanDeviceFeatures::GetGpuFeatures(_gpu, _api_version);
        bool ok           = gpu_features.Contains(features_required);
        if (!ok) {
            LOG_ERROR("GPU does NOT satisfy required core Vulkan features.");
        }
        return ok;
    };

    // 候选 GPU：与 priority 成对保存，避免仅部分 GPU 满足条件时 priority 下标与 gpu_list 错位
    struct SelectGpuCandidate {
        VkPhysicalDevice gpu;
        uint8            priority;
    };
    Array<SelectGpuCandidate> gpu_candidates;
    gpu_candidates.reserve(gpu_count);

    for (auto* gpu : gpu_list) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);

        // // 以下代码用于Debug AMD GPU
        // // 笔记本等多 GPU 场景：名称含 "RTX" 时跳过（例如 RTX 5070 Laptop），便于强制走 A 卡等其它适配器
        // const std::string_view gpu_name(props.deviceName);
        // if (gpu_name.find("NVIDIA") != std::string_view::npos) {
        //     LOG_INFO("SelectGpu: skipping '{}' (name contains '5070').", props.deviceName);
        //     continue;
        // }

        uint8 priority = 0;
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                priority += 0;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                priority += 1;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                priority += 2;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                priority += 3;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                priority += 4;
                break;
            default:
                LOG_ERROR("GPU Device type is invalid!");
        }

        auto indices = QueryQueueFamilyIndices(gpu);
        if (indices.IsComplete() && is_extensions_required_supported(gpu) &&
            is_core_features_supported(gpu)) {
            priority += 100;
            gpu_candidates.push_back(SelectGpuCandidate{gpu, priority});
        }
    }

    if (gpu_candidates.empty()) {
        LOG_ERROR("No available GPU. Details:");

        // 仅在失败分支里输出调试信息：按 GPU 输出缺失的必需扩展
        for (const auto& gpu : gpu_list) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(gpu, &props);

            auto gpu_extensions = VulkanDevice::GetGpuExtensions(gpu);

            std::string log_msg;
            log_msg += std::string("GPU '") + props.deviceName + "':\n";

            bool has_missing = false;
            for (const auto& extension : extensions_required) {
                const auto& required_name = extension->GetExtensionName();
                const bool  is_optional   = extension->IsOptional();

                if (!gpu_extensions.contains(required_name.data()) && !is_optional) {
                    has_missing = true;
                    log_msg += std::string(required_name) + "\n";
                }
            }

            if (has_missing) {
                LOG_ERROR("{}", log_msg);
            }
        }

        CHECK_ASSERT(false, "No available GPU(discrete, etc.) found!");
    }

    const auto highest_priority_iter = std::max_element(
        gpu_candidates.begin(),
        gpu_candidates.end(),
        [](const SelectGpuCandidate& a, const SelectGpuCandidate& b) {
            return a.priority < b.priority;
        }
    );

    return highest_priority_iter->gpu;
}

/**
     * @brief Initialize GPU, query features, properties, memory, queue family, etc.
     * @param _api_version
     */
void VulkanDevice::InitGpu(uint32 _api_version) {
    // Enable extensions
    auto gpu_extensions = VulkanDevice::GetGpuExtensions(m_gpu);

    m_device_info.enabled_extensions = VulkanDeviceExtension::GetMEEnabledDeviceExtensions(gpu_extensions);

    // Query core features
    m_device_info.core_features = VulkanDeviceFeatures::GetGpuFeatures(m_gpu, _api_version);
    // 先从 core feature 中提取 cooperative bundle 需要的前置能力。
    UpdateCooperativePrerequisites(m_device_info.core_features, m_device_info.optional_extensions);
    // Query advanced features, use advanced features as GPU supported, and developers cannot specify them.
    {
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        for (const auto& extension : m_device_info.enabled_extensions) {
            extension->PreGpuFeatures(features2);
        }
        vkGetPhysicalDeviceFeatures2(m_gpu, &features2);
        for (const auto& extension : m_device_info.enabled_extensions) {
            extension->PostGpuFeatures(m_device_info.optional_extensions);
        }
    }

    // Query core properties
    m_device_info.core_properties = VulkanCoreDeviceProperties::GetGpuCoreProperties(m_gpu, _api_version);
    // Query advanced properties.
    {
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        for (const auto& extension : m_device_info.enabled_extensions) {
            extension->PreGpuProperties(this, props2);
        }
        vkGetPhysicalDeviceProperties2(m_gpu, &props2);
        for (const auto& extension : m_device_info.enabled_extensions) {
            extension->PostGpuProperties(this);
        }
    }

    // Query core memory properties
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &m_device_info.memery_properties);

    // Query queue family info
    m_device_info.queue_family_indices = VulkanDevice::QueryQueueFamilyIndices(m_gpu);
    m_device_info.queue_family_props   = VulkanDevice::GetQueueFamilyProperties(m_gpu);

    LOG_INFO("VulkanRHI: GPU initialized.");
    LOG_INFO(
        "\n- DeviceName: {}."
        "\n- API={}.{}.{} (0x{:x}) Driver=0x{:x} VendorId=0x{:x}."
        "\n- DeviceID=0x{:x} Type={}."
        "\n- Max Descriptor Sets Bound {}, Timestamps {}.",
        m_device_info.core_properties.core_1_0.deviceName,
        VK_API_VERSION_MAJOR(m_device_info.core_properties.core_1_0.apiVersion),
        VK_API_VERSION_MINOR(m_device_info.core_properties.core_1_0.apiVersion),
        VK_API_VERSION_PATCH(m_device_info.core_properties.core_1_0.apiVersion),
        m_device_info.core_properties.core_1_0.apiVersion,
        m_device_info.core_properties.core_1_0.driverVersion,
        m_device_info.core_properties.core_1_0.vendorID,
        m_device_info.core_properties.core_1_0.deviceID,
        VK_TYPE_TO_STRING(VkPhysicalDeviceType, m_device_info.core_properties.core_1_0.deviceType),
        m_device_info.core_properties.core_1_0.limits.maxBoundDescriptorSets,
        m_device_info.core_properties.core_1_0.limits.timestampComputeAndGraphics
    );
    // 启动时输出 cooperative 支持摘要，便于后续调试 shader/toolchain 问题。
    LogCooperativeSupportSummary(m_device_info.optional_extensions, m_device_info.optional_properties);
    m_cooperative_extension_info =
        BuildCooperativeExtensionInfo(m_device_info.optional_extensions, m_device_info.optional_properties);
}

void VulkanDevice::CreateDevice(uint32 _api_version) {
    std::set<uint32> unique_family_indices = {
        m_device_info.queue_family_indices.graphics.value(),
        m_device_info.queue_family_indices.present.value(),
        m_device_info.queue_family_indices.compute.value(),
        m_device_info.queue_family_indices.transfer.value()
    };

    // setup queue info
    Moer::Array<VkDeviceQueueCreateInfo> queue_create_infos;

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
    device_create_info.queueCreateInfoCount = static_cast<uint32>(queue_create_infos.size());
    device_create_info.pQueueCreateInfos    = queue_create_infos.data();

    // setup extension and feature info
    Moer::Array<const char*> extensions_loaded;
    for (const auto& extension : m_device_info.enabled_extensions) {
        if (!extension || !extension->ShouldEnableDeviceCreate()) {
            // 如果拓展不启用或者不可用，则跳过
            continue;
        }
        extensions_loaded.emplace_back(extension->GetExtensionName().data());
        extension->PreCreateDevice(device_create_info);
        LOG_INFO("Loading VulkanDeviceExtension: {}", extension->GetExtensionName());
    }
    device_create_info.enabledExtensionCount   = static_cast<uint32>(extensions_loaded.size());
    device_create_info.ppEnabledExtensionNames = extensions_loaded.data();

    VkPhysicalDeviceFeatures2 features_loaded;
    if (_api_version > VK_API_VERSION_1_0) {
        features_loaded.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features_loaded.features = m_device_info.core_features.core_1_0;
        features_loaded.pNext    = &m_device_info.core_features.core_1_1;
        m_device_info.core_features.PreCreateDevice(device_create_info, _api_version);
        device_create_info.pNext = &features_loaded;
    } else {
        device_create_info.pEnabledFeatures = &m_device_info.core_features.core_1_0;
    }
    VK_CHECK_RESULT(vkCreateDevice(m_gpu, &device_create_info, nullptr, &m_device));
    volkLoadDevice(m_device);

    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.graphics.value(), 0, &m_graphics_queue);

    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.present.value(), 0, &m_present_queue);
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.compute.value(), 0, &m_compute_queue);
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.transfer.value(), 0, &m_transfer_queue);
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.raytracing.value(), 0, &m_raytracing_queue);

    gfx_queue = MakeUnique<VkCommandQueue>(
        *this,
        EQueueType::Graphics,
        rhi_thread_enabled,
        thread_profile_logging
    );
    SetResourceName(uint64(m_graphics_queue), VK_OBJECT_TYPE_QUEUE, "GraphicsQueue");
    compute_queue = MakeUnique<VkCommandQueue>(*this, EQueueType::Compute, false);
    SetResourceName(uint64(m_compute_queue), VK_OBJECT_TYPE_QUEUE, "ComputeQueue");
    // transfer_queue = MakeUnique<VkCommandQueue>(*this, EQueueType::Copy);
    copy_queue = MakeUnique<VkCopyQueue>(*this);
    SetResourceName(uint64(m_transfer_queue), VK_OBJECT_TYPE_QUEUE, "TransferQueue");

    gfx_queue->SetQueueSubmitMutex(&GetQueueHostMutex(m_graphics_queue));
    compute_queue->SetQueueSubmitMutex(&GetQueueHostMutex(m_compute_queue));
    copy_queue->SetQueueSubmitMutex(&GetQueueHostMutex(m_transfer_queue));

    if (m_graphics_queue == m_transfer_queue) {
        LOG_WARNING(
            "gfx and transfer share the same VkQueue handle. "
            "Using shared host synchronization for queue operations."
        );
    }
    if (m_compute_queue == m_graphics_queue) {
        LOG_WARNING(
            "compute and gfx share the same VkQueue handle. "
            "Using shared host synchronization for queue operations."
        );
    }
}

void VulkanDevice::CreateMemoryAllocator(VkInstance _instance, uint32 _api_version) {
    VmaAllocatorCreateInfo alloc_create_info{};

    VmaVulkanFunctions vma_functions{};
    vma_functions.vkGetPhysicalDeviceProperties           = vkGetPhysicalDeviceProperties;
    vma_functions.vkGetPhysicalDeviceMemoryProperties     = vkGetPhysicalDeviceMemoryProperties;
    vma_functions.vkAllocateMemory                        = vkAllocateMemory;
    vma_functions.vkFreeMemory                            = vkFreeMemory;
    vma_functions.vkMapMemory                             = vkMapMemory;
    vma_functions.vkUnmapMemory                           = vkUnmapMemory;
    vma_functions.vkFlushMappedMemoryRanges               = vkFlushMappedMemoryRanges;
    vma_functions.vkInvalidateMappedMemoryRanges          = vkInvalidateMappedMemoryRanges;
    vma_functions.vkBindBufferMemory                      = vkBindBufferMemory;
    vma_functions.vkBindImageMemory                       = vkBindImageMemory;
    vma_functions.vkGetBufferMemoryRequirements           = vkGetBufferMemoryRequirements;
    vma_functions.vkGetImageMemoryRequirements            = vkGetImageMemoryRequirements;
    vma_functions.vkCreateBuffer                          = vkCreateBuffer;
    vma_functions.vkDestroyBuffer                         = vkDestroyBuffer;
    vma_functions.vkCreateImage                           = vkCreateImage;
    vma_functions.vkDestroyImage                          = vkDestroyImage;
    vma_functions.vkCmdCopyBuffer                         = vkCmdCopyBuffer;
    vma_functions.vkGetBufferMemoryRequirements2KHR       = vkGetBufferMemoryRequirements2;
    vma_functions.vkGetImageMemoryRequirements2KHR        = vkGetImageMemoryRequirements2;
    vma_functions.vkBindBufferMemory2KHR                  = vkBindBufferMemory2;
    vma_functions.vkBindImageMemory2KHR                   = vkBindImageMemory2;
    vma_functions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;

    alloc_create_info.vulkanApiVersion = _api_version;

    alloc_create_info.instance         = _instance;
    alloc_create_info.physicalDevice   = m_gpu;
    alloc_create_info.device           = m_device;
    alloc_create_info.pVulkanFunctions = &vma_functions;

    //capable of using buffer via device address(64bit) passed to shader.
    alloc_create_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (m_device_info.optional_extensions.m_has_memory_priority) {
        alloc_create_info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    }

#if WITH_CUDA
    // 因为上文 vma_functions 传的是指针，所以这里可以直接修改
    vma_functions.vkGetMemoryWin32HandleKHR = vkGetMemoryWin32HandleKHR;

    // 这里的代码是应vma要求写的，需要手动设置handleTypes，以便跨graphics api使用
    std::vector<VkExternalMemoryHandleTypeFlagsKHR> handleTypes(
        m_device_info.memery_properties.memoryTypeCount
    );
    for (uint i = 0; i < handleTypes.size(); i++) {
        if ((m_device_info.memery_properties.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) > 0) {
            // 只针对gpu内存，设置winn32标记位
            handleTypes[i] = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            // handleTypes[i] = 0;
        }
    }

    alloc_create_info.pTypeExternalMemoryHandleTypes = handleTypes.data();
#endif

    VK_CHECK_RESULT(vmaCreateAllocator(&alloc_create_info, &m_allocator));

    LOG_INFO("Vulkan Memory Allocator initialized with api version: {}.", alloc_create_info.vulkanApiVersion);
}

void VulkanDevice::CreateDescriptorHeap() {
    new (&m_global_descriptor_heap) VulkanDescriptorHeap(*this);
    //create empty descriptor set layout
    VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    descriptor_set_layout_create_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    vkCreateDescriptorSetLayout(
        m_device, &descriptor_set_layout_create_info, VK_NULL_HANDLE, &empty_descriptor_set_layout
    );

    LOG_INFO("VulkanRHI: Descriptor Heap initialized.");
}

void VulkanDevice::CreateInternalShaders() {
    internal_shaders = MakeUnique<DeviceInternalShaders>();
    internal_shaders->sd_component_shuffle =
        ShaderManager::Get().Compute<ComponentShuffleShader>("core/utils/ShuffleBufferIndices.hlsl");
}

void VulkanDevice::DestroyInternalShaders() {
    internal_shaders.reset();
}

void VulkanDevice::CreateInternalResources() {

    CreateImmutableSamplers();
    CreateDescriptorHeap();
}

void VulkanDevice::DestroyInternalResources() {
    DestroyImmutableSamplers();
    DestroyDescriptorHeap();
}

void VulkanDevice::CreateImmutableSamplers() {

    VkSamplerCreateInfo sampler_create_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_create_info.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sampler_create_info.unnormalizedCoordinates = VK_FALSE;
    sampler_create_info.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_create_info.mipLodBias              = 0.0f;
    sampler_create_info.minLod                  = 0.0f;
    sampler_create_info.maxLod                  = VK_LOD_CLAMP_NONE;
    for (uint i = 0; i < immutable_sampler_count; ++i) {
        ESamplerFilter          filter           = ESamplerFilter(i % SF_Num);
        ESamplerAddressMode     address_mode     = ESamplerAddressMode((i / SF_Num) % SAM_Num);
        ESamplerCompareFunction compare_function = ESamplerCompareFunction(i / (SAM_Num * uint(SF_Num)));

        sampler_create_info.minFilter = sampler_create_info.magFilter =
            VulkanEnumTranslator::METoVKMinMagFilterMode(filter);
        sampler_create_info.addressModeU     = sampler_create_info.addressModeV =
            sampler_create_info.addressModeW = VulkanEnumTranslator::METoVKWrapMode(address_mode);

        sampler_create_info.compareOp =
            VulkanEnumTranslator::METoVKCompareOp(ECompareOption(compare_function));
        sampler_create_info.compareEnable = compare_function != SCF_NEVER;

        vkCreateSampler(m_device, &sampler_create_info, VK_NULL_HANDLE, &immutable_samplers[i]);
    }
}

void VulkanDevice::DestroyImmutableSamplers() {
    for (auto& sampler : immutable_samplers) {
        vkDestroySampler(m_device, sampler, VK_NULL_HANDLE);
    }
}

void VulkanDevice::DestroyDescriptorHeap() {
    m_global_descriptor_heap.~VulkanDescriptorHeap();
    vkDestroyDescriptorSetLayout(m_device, empty_descriptor_set_layout, VK_NULL_HANDLE);
}

void VulkanDevice::Destroy() {
    // for (auto& cmd_allocator : m_command_allocators) {
    //     CHECK_AND_DELETE(cmd_allocator);
    // }
    compute_queue.reset();
    gfx_queue.reset();
    copy_queue.reset();
    shutdown_sync_completed.store(
        queue_sync_complete_count.load(std::memory_order_acquire) >= 3,
        std::memory_order_release
    );
    LogFaultSummary();
    DestroyInternalShaders();
    FlushDeferredReleases();
    DestroyInternalResources();
    FlushDeferredReleases();
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, VK_NULL_HANDLE);

    LOG_INFO("VulkanRHI: Device destroyed.");

    // Assertion failed: m_pMetadata->IsEmpty() && "Some allocations were not freed before destruction of this memory block!"
}

Set<std::string> VulkanDevice::GetGpuExtensions(VkPhysicalDevice _gpu, const char* _layer_name) {
    uint32_t gpu_extension_count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &gpu_extension_count, nullptr);
    if (result != VK_SUCCESS || gpu_extension_count == 0) {
        return {};
    }

    Array<VkExtensionProperties> gpu_extensions(gpu_extension_count);
    result =
        vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &gpu_extension_count, gpu_extensions.data());
    if (result != VK_SUCCESS) {
        return {};
    }

    Set<std::string> ret;
    for (const auto& extension : gpu_extensions)
        ret.insert(extension.extensionName);

    return ret;
}

TQueueFamilyPropertiesArray VulkanDevice::GetQueueFamilyProperties(VkPhysicalDevice _gpu) {
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_gpu, &queue_family_count, nullptr);
    assert(queue_family_count > 0);
    TQueueFamilyPropertiesArray queue_family_props(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(_gpu, &queue_family_count, queue_family_props.data());

    return queue_family_props;
}

static uint32_t GetQueueFamilyIndice(
    std::span<const VkQueueFamilyProperties> _queue_family_props,
    VkQueueFlags                             _target_queue_flags,
    VkQueueFlags                             _exclude_queue_flags
) {
    for (uint32_t i = 0; i < _queue_family_props.size(); ++i) {
        if (_queue_family_props[i].queueFlags & _target_queue_flags &&
            !(_queue_family_props[i].queueFlags & _exclude_queue_flags)) {
            return i;
        }
    }
    return -1;
}

int32_t VulkanDevice::GetQueueFamilyIndex(
    const Moer::Array<VkQueueFamilyProperties>& queue_family_props,
    VkQueueFlags                                _queue_flags
) const {
    // Dedicated queue for transfer
    if ((_queue_flags & VK_QUEUE_TRANSFER_BIT) == _queue_flags) {
        for (uint32_t i = 0; i < queue_family_props.size(); ++i) {
            if ((queue_family_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                (queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                return i;
            }
        }
    }

    // Dedicated queue for compute
    if ((_queue_flags & VK_QUEUE_COMPUTE_BIT) == _queue_flags) {
        for (uint32_t i = 0; i < queue_family_props.size(); ++i) {
            if ((queue_family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                (queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
                (queue_family_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) == 0) {
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

uint VulkanDevice::GetQueueFamilyIndex(EQueueType _type) const {
    switch (_type) {

        case EQueueType::Graphics:
            return m_device_info.queue_family_indices.graphics.value();
        case EQueueType::Compute:
            return m_device_info.queue_family_indices.compute.value();
        case EQueueType::Copy:
            return m_device_info.queue_family_indices.transfer.value();
        case EQueueType::Num: {
            break;
        }
        case EQueueType::Ignore: {
            return VK_QUEUE_FAMILY_IGNORED;
        }
    }
    assert(false && "Invalid queue type.");
    return VK_QUEUE_FAMILY_IGNORED;
}

QueueFamilyIndices VulkanDevice::QueryQueueFamilyIndices(VkPhysicalDevice _gpu) const {
    QueueFamilyIndices indices;

    auto queue_family_props = GetQueueFamilyProperties(_gpu);
    // AMD GPU的DMA/Transfer队列功能受限，提交包含Barrier/LayoutTransition的命令会导致DeviceLost
    // 所以如果检测到AMD GPU，就强制transfer队列 = graphics队列
    VkPhysicalDeviceProperties gpu_props{};
    vkGetPhysicalDeviceProperties(_gpu, &gpu_props);
    const bool is_amd = (gpu_props.vendorID == 0x1002);

    auto graphics = GetQueueFamilyIndice(queue_family_props, VK_QUEUE_GRAPHICS_BIT, VkQueueFlagBits(0));
    if (graphics >= 0) {
        indices.graphics   = graphics;
        indices.raytracing = graphics;
        indices.present    = graphics;
    }
    if (is_amd) {
        indices.transfer = indices.graphics.value();
        LOG_WARNING(
            "AMD GPU '{}' (vendorID={:#x}) detected. "
            "Forcing transfer queue = graphics to avoid DMA queue VK_ERROR_DEVICE_LOST.",
            gpu_props.deviceName,
            gpu_props.vendorID
        );
    } else {
        auto transfer = GetQueueFamilyIndice(
            queue_family_props, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT
        );
        if (transfer < 0) {
            transfer = GetQueueFamilyIndice(queue_family_props, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT);
        }
        if (transfer < 0) {
            transfer = indices.graphics.value();
        }
        indices.transfer = transfer;
    }

    auto compute = GetQueueFamilyIndice(queue_family_props, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
    if (compute >= 0) {
        indices.compute = compute;
    } else {
        indices.compute = indices.graphics.value();
    }

    return indices;
}

VkDescriptorType METoVkDescriptorType(uint _desc_type) {
    EVulkanDescriptorType desc_type = EVulkanDescriptorType(_desc_type);
    switch (desc_type) {
        case VDT_UNIFORM_BUFFER:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case VDT_STORAGE_BUFFER:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case VDT_UNIFORM_TEXEL_BUFFER:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case VDT_STORAGE_TEXEL_BUFFER:
            return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case VDT_SAMPLER:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case VDT_SAMPLED_IMAGE:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case VDT_STORAGE_IMAGE:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case VDT_INPUT_ATTACHMENT:
            return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        case VDT_ACCELERATION_STRUCTURE:
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        default:
            assert(false && "Invalid Descriptor Type.");
            break;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

bool VulkanDevice::HasDeviceExtension(std::string_view _ext_name) const {
    for (const auto& ext : m_device_info.enabled_extensions) {
        if (ext && ext->ShouldEnableDeviceCreate() && ext->GetExtensionName() == _ext_name) {
            return true;
        }
    }
    return false;
}

bool VulkanDevice::IsExtensionCooperativeEnabled() const {
    return m_device_info.optional_extensions.IsExtensionCooperativeEnabled();
}

const CooperativeExtensionInfo& VulkanDevice::GetCooperativeExtensionInfo() const {
    return m_cooperative_extension_info;
}

bool VulkanDevice::TryConvertCooperativeVectorMatrix(
    const CooperativeVectorConversionDesc& _desc,
    std::span<const byte>                  _src_data,
    std::span<byte>                        _dst_data
) const {
    const auto log_error = [&](std::string_view _message) {
        LOG_ERROR("VulkanRHI: TryConvertCooperativeVectorMatrix failed: {}", _message);
    };

    if (!m_device_info.optional_extensions.SupportsCooperativeVector()) {
        log_error("VK_NV_cooperative_vector is not enabled on the current device.");
        return false;
    }
    if (vkConvertCooperativeVectorMatrixNV == nullptr) {
        log_error("vkConvertCooperativeVectorMatrixNV is unavailable.");
        return false;
    }
    if (_src_data.empty() || _dst_data.empty()) {
        log_error("Source and destination buffers must be non-empty.");
        return false;
    }

    size_t required_dst_size = _dst_data.size_bytes();

    VkConvertCooperativeVectorMatrixInfoNV info{};
    info.sType               = VK_STRUCTURE_TYPE_CONVERT_COOPERATIVE_VECTOR_MATRIX_INFO_NV;
    info.srcSize             = _src_data.size_bytes();
    info.srcData.hostAddress = _src_data.data();
    info.pDstSize            = &required_dst_size;
    info.dstData.hostAddress = _dst_data.data();
    info.srcComponentType    = static_cast<VkComponentTypeKHR>(_desc.src_component_type);
    info.dstComponentType    = static_cast<VkComponentTypeKHR>(_desc.dst_component_type);
    info.numRows             = _desc.num_rows;
    info.numColumns          = _desc.num_columns;
    info.srcLayout           = static_cast<VkCooperativeVectorMatrixLayoutNV>(_desc.src_layout);
    info.srcStride           = _desc.src_stride;
    info.dstLayout           = static_cast<VkCooperativeVectorMatrixLayoutNV>(_desc.dst_layout);
    info.dstStride           = _desc.dst_stride;

    const VkResult result = vkConvertCooperativeVectorMatrixNV(m_device, &info);
    if (result != VK_SUCCESS) {
        log_error(string_VkResult(result));
        return false;
    }
    if (required_dst_size > _dst_data.size_bytes()) {
        LOG_ERROR(
            "VulkanRHI: TryConvertCooperativeVectorMatrix failed: destination buffer is too small "
            "(required={} bytes, actual={} bytes).",
            required_dst_size,
            _dst_data.size_bytes()
        );
        return false;
    }

    return true;
}

bool VulkanDevice::IsAmdGpu() const {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_gpu, &props);
    return props.vendorID == 0x1002;
}

void VulkanDevice::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& _create_info) {
    _create_info = {};

    _create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    _create_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    _create_info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    _create_info.pfnUserCallback = DebugCallback;
}

void VulkanDevice::FlushDebugMessages() const {
    FlushBufferedDebugMessages();
}

void VulkanDevice::WaitIdle() {
    if (IsDeviceLost()) {
        return;
    }

    std::unique_lock<std::shared_mutex> gate(native_queue_gate);
    if (IsDeviceLost()) {
        return;
    }

    Array<std::unique_lock<std::mutex>> queue_locks;
    const auto                         queue_mutexes = GetUniqueQueueHostMutexes();
    queue_locks.reserve(queue_mutexes.size());
    for (auto* mutex : queue_mutexes) {
        queue_locks.emplace_back(*mutex);
    }

    const VulkanOperationContext context{.operation = EVulkanFaultOperation::DeviceWaitIdle};
    const VkResult               result = vkDeviceWaitIdle(m_device);
    if (result != VK_SUCCESS) {
        if (TryBeginFirstFault(result)) {
            PublishFirstFaultLocked(context, result, false, false);
        }
        if (result != VK_ERROR_DEVICE_LOST) {
            EmergencyExitWithoutVulkanCleanup(context, result);
        }
    }
}

std::mutex& VulkanDevice::GetQueueHostMutex(VkQueue _queue) {
    assert(_queue != VK_NULL_HANDLE);
    if (_queue == m_graphics_queue) {
        return m_graphics_queue_mutex;
    }
    if (_queue == m_present_queue) {
        return m_present_queue_mutex;
    }
    if (_queue == m_compute_queue) {
        return m_compute_queue_mutex;
    }
    if (_queue == m_transfer_queue) {
        return m_transfer_queue_mutex;
    }
    if (_queue == m_raytracing_queue) {
        return m_raytracing_queue_mutex;
    }
    assert(false && "Unknown VkQueue handle");
    return m_graphics_queue_mutex;
}

Array<std::mutex*> VulkanDevice::GetUniqueQueueHostMutexes() {
    Array<std::mutex*> mutexes;
    const VkQueue queues[] = {
        m_graphics_queue,
        m_present_queue,
        m_compute_queue,
        m_transfer_queue,
        m_raytracing_queue,
    };
    for (VkQueue queue : queues) {
        if (queue == VK_NULL_HANDLE) {
            continue;
        }
        std::mutex* mutex = &GetQueueHostMutex(queue);
        if (std::find(mutexes.begin(), mutexes.end(), mutex) == mutexes.end()) {
            mutexes.push_back(mutex);
        }
    }
    return mutexes;
}

VulkanOperationResult VulkanDevice::SubmitOnQueue(
    VkQueue                       _queue,
    const VkSubmitInfo2&          _submit_info,
    VkFence                       _fence,
    const VulkanOperationContext& _context
) {
    std::shared_lock<std::shared_mutex> gate(native_queue_gate);
    std::unique_lock<std::mutex>        queue_lock(GetQueueHostMutex(_queue));
    if (IsFaulted()) {
        queue_lock.unlock();
        gate.unlock();
        RecordRejectedSubmit();
        return {EVulkanOperationStatus::Rejected, GetFirstFaultResult()};
    }

    native_submit_call_count.fetch_add(1, std::memory_order_relaxed);
    const VkResult result = vkQueueSubmit2(_queue, 1, &_submit_info, _fence);
    if (result == VK_SUCCESS) {
        return {};
    }

    const bool publish_fault = TryBeginFirstFault(result);
    queue_lock.unlock();
    gate.unlock();
    if (publish_fault) {
        std::unique_lock<std::shared_mutex> publish_gate(native_queue_gate);
        PublishFirstFaultLocked(_context, result, false, false);
    }
    return {EVulkanOperationStatus::Faulted, result};
}

VulkanOperationResult VulkanDevice::PresentOnQueue(
    VkQueue                       _queue,
    const VkPresentInfoKHR&       _present_info,
    const VulkanOperationContext& _context
) {
    std::shared_lock<std::shared_mutex> gate(native_queue_gate);
    std::unique_lock<std::mutex>        queue_lock(GetQueueHostMutex(_queue));
    if (IsFaulted()) {
        queue_lock.unlock();
        gate.unlock();
        RecordRejectedPresent();
        return {EVulkanOperationStatus::Rejected, GetFirstFaultResult()};
    }

    native_present_call_count.fetch_add(1, std::memory_order_relaxed);
    const VkResult result = vkQueuePresentKHR(_queue, &_present_info);
    if (result == VK_SUCCESS) {
        return {};
    }
    if (result == VK_NOT_READY || result == VK_TIMEOUT) {
        return {EVulkanOperationStatus::Retry, result};
    }
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ||
        result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        return {EVulkanOperationStatus::Recreate, result};
    }

    const bool publish_fault = TryBeginFirstFault(result);
    queue_lock.unlock();
    gate.unlock();
    if (publish_fault) {
        std::unique_lock<std::shared_mutex> publish_gate(native_queue_gate);
        PublishFirstFaultLocked(_context, result, false, false);
    }
    return {EVulkanOperationStatus::Faulted, result};
}

VulkanOperationResult VulkanDevice::AcquireNextImage(
    VkSwapchainKHR                _swapchain,
    uint64                        _timeout,
    VkSemaphore                   _semaphore,
    VkFence                       _fence,
    uint32*                       _image_index,
    const VulkanOperationContext& _context
) {
    std::shared_lock<std::shared_mutex> gate(native_queue_gate);
    if (IsFaulted()) {
        gate.unlock();
        RecordRejectedPresent();
        return {EVulkanOperationStatus::Rejected, GetFirstFaultResult()};
    }

    const VkResult result = vkAcquireNextImageKHR(
        m_device, _swapchain, _timeout, _semaphore, _fence, _image_index
    );
    if (result == VK_SUCCESS) {
        return {};
    }
    if (result == VK_NOT_READY || result == VK_TIMEOUT) {
        return {EVulkanOperationStatus::Retry, result};
    }
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ||
        result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        return {EVulkanOperationStatus::Recreate, result};
    }

    const bool publish_fault = TryBeginFirstFault(result);
    gate.unlock();
    if (publish_fault) {
        std::unique_lock<std::shared_mutex> publish_gate(native_queue_gate);
        PublishFirstFaultLocked(_context, result, false, false);
    }
    return {EVulkanOperationStatus::Faulted, result};
}

VkResult VulkanDevice::WaitQueueIdle(
    VkQueue _queue, const VulkanOperationContext& _context
) {
    if (IsDeviceLost()) {
        return VK_ERROR_DEVICE_LOST;
    }

    std::shared_lock<std::shared_mutex> gate(native_queue_gate);
    std::unique_lock<std::mutex>        queue_lock(GetQueueHostMutex(_queue));
    if (IsDeviceLost()) {
        return VK_ERROR_DEVICE_LOST;
    }

    const VkResult result = vkQueueWaitIdle(_queue);
    if (result == VK_SUCCESS) {
        return result;
    }

    const bool publish_fault = TryBeginFirstFault(result);
    queue_lock.unlock();
    gate.unlock();
    if (publish_fault) {
        std::unique_lock<std::shared_mutex> publish_gate(native_queue_gate);
        PublishFirstFaultLocked(_context, result, false, false);
    }
    if (result != VK_ERROR_DEVICE_LOST) {
        EmergencyExitWithoutVulkanCleanup(_context, result);
    }
    return result;
}

VkResult VulkanDevice::ResetCommandPool(
    VkCommandPool _pool, const VulkanOperationContext& _context
) {
    std::shared_lock<std::shared_mutex> gate(native_queue_gate);
    if (IsFaulted()) {
        gate.unlock();
        return GetFirstFaultResult();
    }

    const VkResult result = vkResetCommandPool(m_device, _pool, 0);
    if (result == VK_SUCCESS) {
        return result;
    }

    const bool publish_fault = TryBeginFirstFault(result);
    gate.unlock();
    if (publish_fault) {
        std::unique_lock<std::shared_mutex> publish_gate(native_queue_gate);
        PublishFirstFaultLocked(_context, result, false, false);
    }
    return result;
}

VkResult VulkanDevice::ResetFence(VkFence _fence, const VulkanOperationContext& _context) {
    std::shared_lock<std::shared_mutex> gate(native_queue_gate);
    if (IsFaulted()) {
        gate.unlock();
        return GetFirstFaultResult();
    }

    const VkResult result = vkResetFences(m_device, 1, &_fence);
    if (result == VK_SUCCESS) {
        return result;
    }

    const bool publish_fault = TryBeginFirstFault(result);
    gate.unlock();
    if (publish_fault) {
        std::unique_lock<std::shared_mutex> publish_gate(native_queue_gate);
        PublishFirstFaultLocked(_context, result, false, false);
    }
    return result;
}

bool VulkanDevice::TryBeginFirstFault(VkResult _result) {
    if (_result == VK_ERROR_DEVICE_LOST) {
        device_lost_observed.store(true, std::memory_order_release);
    }
    EVulkanFaultPublishState expected = EVulkanFaultPublishState::Healthy;
    return fault_state.compare_exchange_strong(
        expected,
        EVulkanFaultPublishState::Publishing,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

void VulkanDevice::PublishFirstFaultLocked(
    const VulkanOperationContext& _context,
    VkResult                      _result,
    bool                          _injected,
    bool                          _predrained
) {
    first_fault = VulkanFaultRecord{
        .operation     = _context.operation,
        .result        = _result,
        .queue_type    = _context.queue_type,
        .queue_handle  = uint64(_context.queue),
        .timeline      = _context.timeline,
        .work_serial   = _context.work_serial,
        .thread_id     = Platform::GetCurrentThreadID(),
        .injected      = _injected,
        .predrained    = _predrained,
    };
    first_fault_count.store(1, std::memory_order_relaxed);
    fault_submit_call_snapshot.store(
        native_submit_call_count.load(std::memory_order_relaxed), std::memory_order_relaxed
    );
    fault_present_call_snapshot.store(
        native_present_call_count.load(std::memory_order_relaxed), std::memory_order_relaxed
    );
    fault_state.store(EVulkanFaultPublishState::Faulted, std::memory_order_release);

    LOG_ERROR(
        "[VulkanFault][First] operation={} result={} result_code={} queue={} queue_handle={:#x} "
        "timeline={} work_serial={} thread={} injected={} predrained={}",
        VulkanFaultOperationName(first_fault.operation),
        VulkanResultName(first_fault.result),
        int(first_fault.result),
        VulkanQueueName(first_fault.queue_type),
        first_fault.queue_handle,
        first_fault.timeline,
        first_fault.work_serial,
        first_fault.thread_id,
        first_fault.injected,
        first_fault.predrained
    );
}

bool VulkanDevice::TryLatchFirstFault(
    const VulkanOperationContext& _context,
    VkResult                      _result,
    bool                          _injected,
    bool                          _predrained,
    bool                          _force_terminal
) {
    if (_result == VK_SUCCESS) {
        return false;
    }
    if (!_force_terminal &&
        (_result == VK_NOT_READY || _result == VK_TIMEOUT ||
         _result == VK_SUBOPTIMAL_KHR || _result == VK_ERROR_OUT_OF_DATE_KHR ||
         _result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)) {
        return false;
    }
    if (!TryBeginFirstFault(_result)) {
        return false;
    }

    std::unique_lock<std::shared_mutex> gate(native_queue_gate);
    PublishFirstFaultLocked(_context, _result, _injected, _predrained);
    return true;
}

bool VulkanDevice::IsFaulted() const {
    return device_lost_observed.load(std::memory_order_acquire) ||
           fault_state.load(std::memory_order_acquire) != EVulkanFaultPublishState::Healthy;
}

bool VulkanDevice::IsDeviceLost() const {
    return device_lost_observed.load(std::memory_order_acquire);
}

[[noreturn]] void VulkanDevice::EmergencyExitWithoutVulkanCleanup(
    const VulkanOperationContext& _context, VkResult _result
) {
    bool expected = false;
    if (emergency_exit_started.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
        LOG_CRITICAL(
            "[VulkanFault][CompletionUnknownFatal] operation={} result={} result_code={} "
            "queue={} queue_handle={:#x} timeline={} work_serial={} thread={} "
            "action=exit-without-vulkan-cleanup",
            VulkanFaultOperationName(_context.operation),
            VulkanResultName(_result),
            int(_result),
            VulkanQueueName(_context.queue_type),
            uint64(_context.queue),
            _context.timeline,
            _context.work_serial,
            Platform::GetCurrentThreadID()
        );
        if (auto logger = spdlog::default_logger()) {
            logger->flush();
        }
    }
    std::_Exit(EXIT_FAILURE);
}

VkResult VulkanDevice::GetFirstFaultResult() const {
    EVulkanFaultPublishState state = fault_state.load(std::memory_order_acquire);
    while (state == EVulkanFaultPublishState::Publishing) {
        std::this_thread::yield();
        state = fault_state.load(std::memory_order_acquire);
    }
    if (state != EVulkanFaultPublishState::Faulted) {
        return VK_ERROR_DEVICE_LOST;
    }
    return first_fault.result;
}

bool VulkanDevice::ShouldInjectPresentSubmit() {
    if (present_submit_fault_trigger == 0) {
        return false;
    }
    return present_submit_attempts.fetch_add(1, std::memory_order_relaxed) + 1 ==
           present_submit_fault_trigger;
}

VulkanOperationResult VulkanDevice::InjectPresentSubmitFault(
    const VulkanOperationContext& _context
) {
    std::unique_lock<std::shared_mutex> gate(native_queue_gate);
    if (IsFaulted()) {
        gate.unlock();
        RecordRejectedSubmit();
        return {EVulkanOperationStatus::Rejected, GetFirstFaultResult()};
    }

    Array<std::unique_lock<std::mutex>> queue_locks;
    const auto                         queue_mutexes = GetUniqueQueueHostMutexes();
    queue_locks.reserve(queue_mutexes.size());
    for (auto* mutex : queue_mutexes) {
        queue_locks.emplace_back(*mutex);
    }

    const VkResult predrain_result = vkDeviceWaitIdle(m_device);
    const bool     predrained      = predrain_result == VK_SUCCESS;
    const VkResult result          = predrained ? VK_ERROR_DEVICE_LOST : predrain_result;
    if (TryBeginFirstFault(result)) {
        PublishFirstFaultLocked(_context, result, true, predrained);
    }
    if (!predrained && result != VK_ERROR_DEVICE_LOST) {
        EmergencyExitWithoutVulkanCleanup(_context, result);
    }
    return {EVulkanOperationStatus::Faulted, result, true, predrained};
}

void VulkanDevice::RecordRejectedSubmit() {
    rejected_submit_count.fetch_add(1, std::memory_order_relaxed);
}

void VulkanDevice::RecordRejectedPresent() {
    rejected_present_count.fetch_add(1, std::memory_order_relaxed);
}

void VulkanDevice::RecordAllocatorQuarantine() {
    allocator_quarantine_count.fetch_add(1, std::memory_order_relaxed);
}

void VulkanDevice::RecordSkippedCommandPoolReset() {
    skipped_command_pool_reset_count.fetch_add(1, std::memory_order_relaxed);
}

void VulkanDevice::RecordQueueSyncComplete() {
    queue_sync_complete_count.fetch_add(1, std::memory_order_relaxed);
}

void VulkanDevice::LogFaultSummary() const {
    if (fault_state.load(std::memory_order_acquire) != EVulkanFaultPublishState::Faulted) {
        return;
    }
    const uint64 native_submit_after_fault =
        native_submit_call_count.load(std::memory_order_relaxed) -
        fault_submit_call_snapshot.load(std::memory_order_relaxed);
    const uint64 native_present_after_fault =
        native_present_call_count.load(std::memory_order_relaxed) -
        fault_present_call_snapshot.load(std::memory_order_relaxed);
    LOG_INFO(
        "[VulkanFault][Summary] first_fault_count={} native_submit_after_fault={} "
        "native_present_after_fault={} rejected_submit={} rejected_present={} "
        "device_lost={} skipped_command_pool_reset={} allocator_quarantined={} sync_completed={} "
        "quarantine_count={} queue_sync_count={}",
        first_fault_count.load(std::memory_order_relaxed),
        native_submit_after_fault,
        native_present_after_fault,
        rejected_submit_count.load(std::memory_order_relaxed),
        rejected_present_count.load(std::memory_order_relaxed),
        IsDeviceLost(),
        skipped_command_pool_reset_count.load(std::memory_order_relaxed) > 0,
        allocator_quarantine_count.load(std::memory_order_relaxed) > 0,
        shutdown_sync_completed.load(std::memory_order_acquire),
        allocator_quarantine_count.load(std::memory_order_relaxed),
        queue_sync_complete_count.load(std::memory_order_relaxed)
    );
}

void VulkanDevice::SetupDebugUtilsMessengerEXT() {
    VkDebugUtilsMessengerCreateInfoEXT debug_utils_messenger_create_info{};
    PopulateDebugMessengerCreateInfo(debug_utils_messenger_create_info);
    VK_CHECK_RESULT(vkCreateDebugUtilsMessengerEXT(
        m_instance, &debug_utils_messenger_create_info, nullptr, &m_debug_utils_messenger
    ));
}

static VkPipelineStageFlags2 VkShaderStage2PipelineStage(VkShaderStageFlagBits _stage) {
    switch (_stage) {

        case VK_SHADER_STAGE_VERTEX_BIT:
            return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            return VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            return VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
        case VK_SHADER_STAGE_GEOMETRY_BIT:
            return VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case VK_SHADER_STAGE_COMPUTE_BIT:
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case VK_SHADER_STAGE_ALL_GRAPHICS:
            return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        case VK_SHADER_STAGE_ALL:
            return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            return VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_NV;
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            return VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_NV;
        default:
            assert(false && "Invalid Shader Stage.");
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

// Merge Shader Reflection Info with Cpp End Definitions, thus we can get binding relations between shader and cpp.
static void MergeReflectInfo(
    const VulkanDevice&     _device,
    const SingleShaderInfo& _info,
    // target shader compiled result
    const PipelineShaderInfo& _shader_info,
    // all shader compiled result in this pso
    VkShaderStageFlagBits _stage,
    // target shader stage
    UnorderedMap<uint64, uint>& _out_hash_2_idx,
    // hash to index
    Moer::Array<ParamInfoFlags>& _out_reflect_flags,
    // cpp param name hash to shader binding index
    UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>& _out_descriptor_bindings,
    // set to binding to binding info, actual vk pipeline layout
    VkPushConstantRange& _out_push_constant_ranges,
    // push constant range
    int&    _out_constant_idx,
    uint64& _out_valid_bits,
    // push constant index in cpp param
    uint& _max_set // max set index, calculate descriptor set count
) {
    auto set_valid_bits = [&](uint _idx, bool _b_valid) {
        if (_b_valid) {
            _out_valid_bits |= 1ull << _idx;
        }
    };
    for (const auto& hash : _shader_info.layout_hash) {
        uint idx                         = uint(&hash - _shader_info.layout_hash.data());
        _out_hash_2_idx[GetHash(hash)]   = idx;
        const ShaderArgCppInfo& arg_info = _shader_info.arg_cpp_info[idx];

        const UnorderedMap<std::string, ReflectParamInfo>& reflect_map  = _info.shader_param_map->reflect_map;
        const auto                                         binding_iter = reflect_map.find(hash.data());
        bool                                               b_found      = binding_iter != reflect_map.end();
        if (arg_info.type != SDA_BindlessArray && !b_found) {
            continue;
        }
        switch (arg_info.type) {
            case SDA_BindlessArray: {
                auto bdls_iter = reflect_map.find(ReflectParamInfo::bdls_name.data());
                if (bdls_iter == reflect_map.end()) {
                    // no bindless array found, skip
                    break;
                }
                const ReflectParamInfo&                binding_info = bdls_iter->second;
                const ReflectParamInfo::BindlessArray& bdls_array   = binding_info.spirv.bindless;
                bool b_found_bindless_buffer                        = bdls_array.buffer.has_value();
                bool b_found_bindless_texture                       = bdls_array.image.has_value();
                bool b_found_bindless_sampler                       = bdls_array.sampler.has_value();
                bool b_found_bindless_indirect                      = bdls_array.array.has_value();
                uint texture_set                                    = 0;
                uint buffer_set                                     = 0;
                if (b_found_bindless_indirect) {
                    const ReflectParamInfo::Bindless& indirect_binding_info = bdls_array.array.value();

                    auto&                 array_set     = _out_descriptor_bindings[indirect_binding_info.set];
                    static constexpr uint indirect_slot = 0;
                    static constexpr uint sampler_slot  = 0;
                    static constexpr uint texture_slot  = 0;
                    static constexpr uint buffer_slot   = 1;
                    assert(indirect_binding_info.binding == 0 && "Indirect Binding Slot Must be 0.");
                    auto& vk_binding           = array_set[indirect_slot];
                    vk_binding.binding         = indirect_slot;
                    vk_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    vk_binding.descriptorCount = 1;
                    vk_binding.stageFlags |= _stage;
                    vk_binding.pImmutableSamplers = nullptr;
                    _max_set   = uint(std::max(int(_max_set), int(indirect_binding_info.set)));
                    buffer_set = indirect_binding_info.set;
                    array_set.bindings[indirect_slot].param_idx = idx;
                    array_set.is_bindless                       = true;
                    set_valid_bits(idx, bdls_array.array.value().custom_flag.active);
                    if (b_found_bindless_buffer) {
                        array_set.bindings[buffer_slot].param_idx = idx;
                        auto& temp_binding                        = array_set[buffer_slot];
                        temp_binding.binding                      = buffer_slot;
                        temp_binding.descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        temp_binding.descriptorCount              = 5000;
                        temp_binding.stageFlags |= _stage;
                        temp_binding.pImmutableSamplers = nullptr;
                        set_valid_bits(idx, bdls_array.buffer.value().custom_flag.active);
                    }

                    if (b_found_bindless_texture) {
                        auto& texture_set = _out_descriptor_bindings[bdls_array.image.value().set];
                        texture_set.bindings[texture_slot].param_idx = idx;

                        auto& temp_binding           = texture_set[texture_slot];
                        texture_set.is_bindless      = true;
                        temp_binding.binding         = texture_slot;
                        temp_binding.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                        temp_binding.descriptorCount = 5000;
                        temp_binding.stageFlags |= _stage;
                        temp_binding.pImmutableSamplers = nullptr;

                        _max_set = uint(std::max(int(_max_set), int(bdls_array.image.value().set)));
                        set_valid_bits(idx, bdls_array.image.value().custom_flag.active);
                    }

                    if (b_found_bindless_sampler) {
                        auto& sampler_set  = _out_descriptor_bindings[bdls_array.sampler.value().set];
                        auto& temp_binding = sampler_set[sampler_slot];
                        sampler_set.bindings[sampler_slot].param_idx = idx;
                        sampler_set.is_bindless                      = true;
                        temp_binding.binding                         = sampler_slot;
                        temp_binding.descriptorType                  = VK_DESCRIPTOR_TYPE_SAMPLER;
                        temp_binding.descriptorCount                 = 256;
                        temp_binding.stageFlags |= _stage;
                        temp_binding.pImmutableSamplers = nullptr;
                        _max_set = uint(std::max(int(_max_set), int(bdls_array.sampler.value().set)));
                        set_valid_bits(idx, bdls_array.sampler.value().custom_flag.active);
                    }
                }
                _out_reflect_flags[idx].pipeline_flags |= VkShaderStage2PipelineStage(_stage);

                break;
            }
            case SDA_Constant: {
                const ReflectParamInfo&           binding_info = binding_iter->second;
                const ReflectParamInfo::Constant& constant =
                    std::get<ReflectParamInfo::Constant>(binding_info.spirv.resources.data);
                _out_constant_idx              = idx;
                _out_push_constant_ranges.size = std::max(_out_push_constant_ranges.size, constant.size);
                _out_push_constant_ranges.stageFlags |= _stage;
                // _out_reflect_flags[idx] = EncodeReflectInfo(0, constant.size, _out_push_constant_ranges.stageFlags);
                set_valid_bits(idx, constant.custom_flag.active);
                break;
            }
            case SDA_Buffer:
            case SDA_Texture:
            case SDA_TLAS:
            case SDA_Sampler: {
                // _out_valid_bits |= 1 << idx;
                const ReflectParamInfo&           binding_info = binding_iter->second;
                const ReflectParamInfo::Resource& resource =
                    std::get<ReflectParamInfo::Resource>(binding_info.spirv.resources.data);

                // auto  rel_desc_type        = std::get<SpvReflectDescriptorType>(desc_type);
                // auto  rel_res_type         = std::get<SpvReflectResourceType>(resource_type);
                auto  desc_type            = METoVkDescriptorType(resource.desc_type);
                auto& set                  = _out_descriptor_bindings[resource.set];
                auto& vk_binding           = set[resource.binding];
                vk_binding.binding         = resource.binding;
                vk_binding.descriptorType  = desc_type;
                vk_binding.descriptorCount = std::max(resource.count, arg_info.array_size);
                vk_binding.stageFlags |= _stage;
                vk_binding.pImmutableSamplers            = nullptr;
                set.bindings[resource.binding].param_idx = idx;
                // _out_reflect_flags[idx]       = EncodeReflectInfo(resource.set, resource.binding, vk_binding.stageFlags);
                _max_set = uint(std::max(int(_max_set), int(resource.set)));
                VulkanShaderResourceState state =
                    VulkanShaderResourceState(resource.desc_type, resource.resource_type, resource.format);
                if (arg_info.type == SDA_Buffer) {
                } else if (arg_info.type == SDA_Texture) {
                    state.b_sampled = resource.sampled;
                }
                _out_reflect_flags[idx].state_flags = state();
                _out_reflect_flags[idx].pipeline_flags |= VkShaderStage2PipelineStage(_stage);
                set_valid_bits(idx, resource.custom_flag.active);
                break;
            }
            default:
                assert(false && "Unknown shader arg type.");
        }
    }
    //finalize
    for (auto& create_info : _out_descriptor_bindings) {
        //fill missing bindings with empty
        uint max_binding = 0;
        for (auto& binding : create_info.second.bindings) {
            max_binding = std::max(max_binding, binding.first);
        }
        for (uint i = 0; i <= max_binding; ++i) {
            if (create_info.second.bindings.find(i) == create_info.second.bindings.end()) {
                auto& vk_binding              = create_info.second[i];
                vk_binding.binding            = i;
                vk_binding.descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER;
                vk_binding.descriptorCount    = 0;
                vk_binding.stageFlags         = 0;
                vk_binding.pImmutableSamplers = nullptr;
            }
        }
    }
}

PipelineHandle
VulkanDevice::CreatePipeline(GfxPsoCreateInfo&& _create_info, PipelineShaderInfo&& _shader_info) {
    const bool uses_tessellation = std::holds_alternative<ShaderVsHsDsPs>(_shader_info.shader_group);
    if (uses_tessellation) {
        const uint32_t patch_control_points = _create_info.patch_control_points;
        const uint32_t max_patch_size = m_device_info.core_properties.core_1_0.limits.maxTessellationPatchSize;
        if (!SupportsTessellation()) {
            LOG_ERROR("Cannot create tessellation pipeline: the Vulkan device does not support tessellation shaders.");
            return {};
        }
        if (_create_info.primitive_topology != EPrimitiveTopology::PATCH_LIST) {
            LOG_ERROR("Cannot create tessellation pipeline: primitive topology must be PATCH_LIST.");
            return {};
        }
        if (patch_control_points == 0 || patch_control_points > max_patch_size) {
            LOG_ERROR(
                "Cannot create tessellation pipeline: patch control point count {} is outside [1, {}].",
                patch_control_points,
                max_patch_size
            );
            return {};
        }
    } else if (_create_info.primitive_topology == EPrimitiveTopology::PATCH_LIST) {
        LOG_ERROR("Cannot create PATCH_LIST pipeline without Hull and Domain shaders.");
        return {};
    }

    VulkanPipelineState* vk_pso = MoerNew(VulkanPipelineState)(this, VulkanPipelineState::GFX);

    uint32_t attachment_count = _create_info.color_attachment_count;

    Moer::Array<VkFormat> color_attachment_formats(attachment_count);

    for (int i = 0; i < attachment_count; ++i) {
        color_attachment_formats[i] = VkFormat(_create_info.color_attachments_info[i].pixel_format);
    }
    VkPipelineRenderingCreateInfo rendering_create_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering_create_info.pNext                   = nullptr;
    rendering_create_info.viewMask                = 0;
    rendering_create_info.colorAttachmentCount    = attachment_count;
    rendering_create_info.pColorAttachmentFormats = color_attachment_formats.data();
    rendering_create_info.depthAttachmentFormat =
        VulkanEnumTranslator::METoVKFormat(_create_info.depth_stencil_format);

    // Only declare a stencil attachment when the PSO actually enables stencil tests.
    bool depth_has_stencil =
        (_create_info.depth_stencil_format == PF_D32_SFLOAT_S8_UINT ||
         _create_info.depth_stencil_format == PF_D24_UNORM_S8_UINT ||
         _create_info.depth_stencil_format == PF_D16_UNORM_S8_UINT ||
         _create_info.depth_stencil_format == PF_S8_UINT);
    bool stencil_enabled = _create_info.depth_stencil_info.b_enable_front_face_stencil ||
                           _create_info.depth_stencil_info.b_enable_back_face_stencil;
    vk_pso->b_uses_stencil_attachment = depth_has_stencil && stencil_enabled;
    rendering_create_info.stencilAttachmentFormat =
        vk_pso->b_uses_stencil_attachment ?
            VulkanEnumTranslator::METoVKFormat(_create_info.depth_stencil_format) :
            VK_FORMAT_UNDEFINED;

    auto to_vk_blend_attachment = [](const RHIBlendAttachmentInfo& _info) {
        VkPipelineColorBlendAttachmentState state{};
        state.blendEnable =
            (_info.color_blend_op != BO_ADD || _info.color_dst_blend_factor != BF_ZERO ||
             _info.color_src_blend_factor != BF_ONE || _info.alpha_blend_op != BO_ADD ||
             _info.alpha_dst_blend_factor != BF_ZERO || _info.alpha_src_blend_factor != BF_ONE) ?
                VK_TRUE :
                VK_FALSE;
        state.srcColorBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(_info.color_src_blend_factor);
        state.dstColorBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(_info.color_dst_blend_factor);
        state.colorBlendOp        = VulkanEnumTranslator::METoVKBlendOp(_info.color_blend_op);
        state.srcAlphaBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(_info.alpha_src_blend_factor);
        state.dstAlphaBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(_info.alpha_dst_blend_factor);
        state.alphaBlendOp        = VulkanEnumTranslator::METoVKBlendOp(_info.alpha_blend_op);
        state.colorWriteMask      = (_info.color_write_mask & CW_RED) ? VK_COLOR_COMPONENT_R_BIT : 0;
        state.colorWriteMask |= (_info.color_write_mask & CW_GREEN) ? VK_COLOR_COMPONENT_G_BIT : 0;
        state.colorWriteMask |= (_info.color_write_mask & CW_BLUE) ? VK_COLOR_COMPONENT_B_BIT : 0;
        state.colorWriteMask |= (_info.color_write_mask & CW_ALPHA) ? VK_COLOR_COMPONENT_A_BIT : 0;
        return std::move(state);
    };
    Moer::Array<VkPipelineColorBlendAttachmentState> color_blend_attachments(attachment_count);
    for (int i = 0; i < attachment_count; ++i) {
        color_blend_attachments[i] =
            to_vk_blend_attachment(_create_info.color_attachments_info[i].blend_state_info);
    }
    // color blend state
    VkPipelineColorBlendStateCreateInfo color_blend_state{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };

    color_blend_state.logicOp         = VK_LOGIC_OP_COPY;
    color_blend_state.logicOpEnable   = VK_FALSE;
    color_blend_state.attachmentCount = attachment_count;
    color_blend_state.pAttachments    = color_blend_attachments.data();

    Moer::Array<VkPipelineShaderStageCreateInfo> shader_stages;

    auto emplace_shader = [&](const SingleShaderInfo& _info, VkShaderStageFlagBits _stage) {
        VkShaderModuleCreateInfo shader_module_create_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_module_create_info.codeSize = _info.shader_data.size();
        shader_module_create_info.pCode    = reinterpret_cast<const uint32_t*>(_info.shader_data.data());
        shader_module_create_info.flags    = 0;
        VkShaderModule shader_module;
        VK_CHECK_RESULT(vkCreateShaderModule(m_device, &shader_module_create_info, nullptr, &shader_module));
        shader_stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO});
        auto& shader_stage_info = shader_stages.back();

        shader_stage_info.stage  = _stage;
        shader_stage_info.module = shader_module;
        shader_stage_info.pName  = _info.entry_point.data();
    };
    using TPipelineSets = UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>;
    TPipelineSets       descriptor_bindings;
    VkPushConstantRange push_constant_ranges{.offset = 0, .size = 0};
    uint                max_set = 0;

    Moer::Array<ParamInfoFlags> reflect_flags(_shader_info.layout_hash.size());
    uint64                      valid_bits = 0;
    UnorderedMap<uint64, uint>  hash_2_idx;
    int                         constant_idx = -1;

    auto merge_reflect_info = [&](const SingleShaderInfo& _info, VkShaderStageFlagBits _stage) {
        MergeReflectInfo(
            *this,
            _info,
            _shader_info,
            _stage,
            hash_2_idx,
            reflect_flags,
            descriptor_bindings,
            push_constant_ranges,
            constant_idx,
            valid_bits,
            max_set
        );
    };

    // shader stage
    std::visit(
        Overload{
            [&](const ShaderVsPs& _shader_info_group) {
                shader_stages.reserve(2);
                emplace_shader(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                merge_reflect_info(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
            },
            [&](const ShaderVsGsPs& _shader_info_group) {
                shader_stages.reserve(3);
                emplace_shader(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                emplace_shader(_shader_info_group.gs, VK_SHADER_STAGE_GEOMETRY_BIT);
                emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                merge_reflect_info(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                merge_reflect_info(_shader_info_group.gs, VK_SHADER_STAGE_GEOMETRY_BIT);
                merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
            },
            [&](const ShaderVsHsDsPs& _shader_info_group) {
                shader_stages.reserve(4);
                emplace_shader(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                emplace_shader(_shader_info_group.hs, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
                emplace_shader(_shader_info_group.ds, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
                emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                merge_reflect_info(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                merge_reflect_info(_shader_info_group.hs, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
                merge_reflect_info(_shader_info_group.ds, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
                merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
            },
            [&](const ShaderMsPs& _shader_info_group) {
                shader_stages.reserve(2);
                emplace_shader(_shader_info_group.ms, VK_SHADER_STAGE_MESH_BIT_NV);
                emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                merge_reflect_info(_shader_info_group.ms, VK_SHADER_STAGE_MESH_BIT_NV);
                merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
            },
            [&](const ShaderTsMsPs& _shader_info_group) {
                shader_stages.reserve(3);
                emplace_shader(_shader_info_group.ts, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
                emplace_shader(_shader_info_group.ms, VK_SHADER_STAGE_MESH_BIT_NV);
                emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                merge_reflect_info(_shader_info_group.ts, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
                merge_reflect_info(_shader_info_group.ms, VK_SHADER_STAGE_MESH_BIT_NV);
                merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
            },
            [&](const ShaderRT& _shader_info_group) {
                assert(false && "Should Use RT PSO.");
            },
            [&](const ShaderCs& _shader_info_group) {
                assert(false && "Should Use Compute PSO.");
            },
        },
        _shader_info.shader_group
    );

    VkPipelineVertexInputStateCreateInfo vertex_input_state{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    Moer::Array<VkVertexInputBindingDescription>   binding_descs;
    Moer::Array<VkVertexInputAttributeDescription> attribute_descs;

    auto to_vk_vertex_stream = [&]() {
        uint binding_offset = 0;
        binding_descs.reserve(_create_info.vertex_stream.bindings.size());
        auto attribute_cnt = [&]() {
            uint cnt = 0;
            for (const auto& binding : _create_info.vertex_stream.bindings) {
                cnt += binding.vertex_elements.size();
            }
            return cnt;
        }();
        uint attrib_location = 0;
        attribute_descs.reserve(attribute_cnt);
        for (const VertexBinding& binding : _create_info.vertex_stream.bindings) {
            uint binding_stride = 0;
            uint attrib_offset  = 0;
            for (const VertexElement& attribute : binding.vertex_elements) {
                const FormatInfo& format_info = g_platform_pixel_formats[uint(attribute.format)];
                attribute_descs.emplace_back(
                    attrib_location++, binding_offset, format_info.format, attrib_offset
                );
                attrib_offset += format_info.stride;
                binding_stride = attrib_offset;
            }
            binding_descs.emplace_back(
                binding_offset,
                binding_stride,
                VulkanEnumTranslator::METoVKVertexInputRate(binding.input_rate)
            );
            ++binding_offset;
        }
        vertex_input_state.vertexBindingDescriptionCount   = uint(binding_descs.size());
        vertex_input_state.pVertexBindingDescriptions      = binding_descs.data();
        vertex_input_state.vertexAttributeDescriptionCount = uint(attribute_descs.size());
        vertex_input_state.pVertexAttributeDescriptions    = attribute_descs.data();
    };
    to_vk_vertex_stream();
    // input assembly
    VkPipelineInputAssemblyStateCreateInfo input_assembly_state{};
    input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.pNext = nullptr;
    input_assembly_state.flags = 0;
    input_assembly_state.topology =
        VulkanEnumTranslator::METoVKPrimitiveTopology(_create_info.primitive_topology);
    input_assembly_state.primitiveRestartEnable = VK_FALSE;

    // viewport state
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.pNext         = nullptr;
    viewport_state.flags         = 0;
    viewport_state.viewportCount = _create_info.multi_view_count;
    viewport_state.scissorCount  = _create_info.multi_view_count;
    // rasterization state
    VkPipelineRasterizationStateCreateInfo vk_rasterization_state{};

    auto to_rasterize_state = [](const RHIRasterizeInfo& _info) {
        VkPipelineRasterizationStateCreateInfo state{};
        state.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        state.pNext                   = nullptr;
        state.flags                   = 0;
        state.depthClampEnable        = _info.b_depth_clamp_enable ? VK_TRUE : VK_FALSE;
        state.rasterizerDiscardEnable = VK_FALSE; // MARK...
        state.polygonMode             = VulkanEnumTranslator::METoVKPolygonMode(_info.fill_mode);
        state.cullMode                = VulkanEnumTranslator::METoVKCullModeFlags(_info.cull_mode);
        state.frontFace               = _info.b_front_counter_clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE :
                                                                          VK_FRONT_FACE_CLOCKWISE; // MARK...
        state.depthBiasEnable         = _info.b_depth_bias ? VK_TRUE : VK_FALSE;
        state.depthBiasConstantFactor = _info.depth_bias;
        state.depthBiasClamp          = _info.depth_bias_clamp;
        state.depthBiasSlopeFactor    = _info.depth_bias_slop_factor;
        state.lineWidth               = 1.0f;
        return std::move(state);
    };

    vk_rasterization_state = to_rasterize_state(_create_info.rasterizer_info);
    // multisample state
    auto to_multi_sample_state = [](const RHIMultisampleStateInfo& info) {
        VkPipelineMultisampleStateCreateInfo state{};
        state.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        state.pNext                 = nullptr;
        state.flags                 = 0;
        state.rasterizationSamples  = VulkanEnumTranslator::METoVKSampleCountFlagBits(info.sample_count);
        state.sampleShadingEnable   = VK_FALSE;
        state.minSampleShading      = 1.0f;
        state.pSampleMask           = nullptr;
        state.alphaToCoverageEnable = VK_FALSE;
        state.alphaToOneEnable      = VK_FALSE;
        return std::move(state);
    };
    auto vk_multisample_state = to_multi_sample_state(_create_info.multisample_info);

    VkPipelineTessellationStateCreateInfo tessellation_state{
        VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO
    };
    tessellation_state.patchControlPoints = _create_info.patch_control_points;

    auto to_depth_stencil_state = [](const RHIDepthStencilStateInfo& info) {
        VkPipelineDepthStencilStateCreateInfo state{};
        state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        state.pNext = nullptr;
        state.flags = 0;
        state.depthTestEnable =
            (info.b_enable_depth_write || info.depth_test_op != ECompareOption::CO_NEVER) ? VK_TRUE :
                                                                                            VK_FALSE;
        state.depthWriteEnable      = info.b_enable_depth_write;
        state.depthCompareOp        = VulkanEnumTranslator::METoVKCompareOp(info.depth_test_op);
        state.depthBoundsTestEnable = VK_FALSE; // MARK...
        state.minDepthBounds        = 0.0f;
        state.maxDepthBounds        = 1.0f;

        state.stencilTestEnable =
            (info.b_enable_front_face_stencil || info.b_enable_back_face_stencil) ? VK_TRUE : VK_FALSE;

        if (info.b_enable_front_face_stencil) {
            state.front.failOp =
                VulkanEnumTranslator::METoVKStencilOp(info.front_face_stencil_fail_stencil_op);
            state.front.passOp = VulkanEnumTranslator::METoVKStencilOp(info.front_face_pass_stencil_op);
            state.front.depthFailOp =
                VulkanEnumTranslator::METoVKStencilOp(info.front_face_depth_fail_stencil_op);
            state.front.compareOp   = VulkanEnumTranslator::METoVKCompareOp(info.front_face_stencil_test);
            state.front.compareMask = info.stencil_readmask;
            state.front.writeMask   = info.stencil_writemask;
            state.front.reference   = 0;
        } else {
            state.front.failOp      = VK_STENCIL_OP_KEEP;
            state.front.passOp      = VK_STENCIL_OP_KEEP;
            state.front.depthFailOp = VK_STENCIL_OP_KEEP;
            state.front.compareOp   = VK_COMPARE_OP_ALWAYS;
            state.front.compareMask = 0;
            state.front.writeMask   = 0;
            state.front.reference   = 0;
        }

        if (info.b_enable_back_face_stencil) {
            state.back.failOp = VulkanEnumTranslator::METoVKStencilOp(info.back_face_stencil_fail_stencil_op);
            state.back.passOp = VulkanEnumTranslator::METoVKStencilOp(info.back_face_pass_stencil_op);
            state.back.depthFailOp =
                VulkanEnumTranslator::METoVKStencilOp(info.back_face_depth_fail_stencil_op);
            state.back.compareOp   = VulkanEnumTranslator::METoVKCompareOp(info.back_face_stencil_test);
            state.back.compareMask = info.stencil_readmask;
            state.back.writeMask   = info.stencil_writemask;
            state.back.reference   = 0;
        } else {
            state.back.failOp      = VK_STENCIL_OP_KEEP;
            state.back.passOp      = VK_STENCIL_OP_KEEP;
            state.back.depthFailOp = VK_STENCIL_OP_KEEP;
            state.back.compareOp   = VK_COMPARE_OP_ALWAYS;
            state.back.compareMask = 0;
            state.back.writeMask   = 0;
            state.back.reference   = 0;
        }
        return std::move(state);
    };
    auto vk_depth_stencil_state = to_depth_stencil_state(_create_info.depth_stencil_info);

    // dynamic state
    Moer::StaticArray<VkDynamicState, 2> states = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
        //VK_DYNAMIC_STATE_STENCIL_REFERENCE //TODO：用于动态设置模板Ref值
    };
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.pNext             = nullptr;
    dynamic_state.flags             = 0;
    dynamic_state.dynamicStateCount = states.size();
    dynamic_state.pDynamicStates    = states.data();

    // init descriptor set layouts and pipeline resource cache
    // Moer::Array<TDescriptorSetLayoutBindingArray> desc_sets_array(max_set + 1u);
    // for (const auto& [set_idx, desc_set] : descriptor_bindings) {
    //     TDescriptorSetLayoutBindingArray desc_set_layouts;
    //     for (const auto& [binding_idx, binding] : desc_set) { desc_set_layouts.push_back(binding); }
    //     desc_sets_array[set_idx] = std::move(desc_set_layouts);
    // }
    // vk_pso->InitDescriptorSetLayouts(desc_sets_array);
    // vk_pso->InitPipelineResourceCache(desc_sets_array);
    if (push_constant_ranges.size != 0) {
        vk_pso->InitPipelineLayout(std::move(descriptor_bindings), std::move(push_constant_ranges));

    } else {
        vk_pso->InitPipelineLayout(std::move(descriptor_bindings));
    }

    // const auto& layouts = vk_pso->GetDescriptorSetsLayout()->GetLayouts();
    // // create pipeline layout
    // VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
    // pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    // pipeline_layout_create_info.pNext                  = nullptr;
    // pipeline_layout_create_info.flags                  = 0;
    // pipeline_layout_create_info.setLayoutCount         = layouts.size();
    // pipeline_layout_create_info.pSetLayouts            = layouts.data();
    // pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size == 0 ? 0 : 1;
    // pipeline_layout_create_info.pPushConstantRanges    = &push_constant_ranges;

    // vk_pso->CreatePipelineLayout(pipeline_layout_create_info);
    VkGraphicsPipelineCreateInfo pipeline_create_info{};
    pipeline_create_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_create_info.pNext               = &rendering_create_info;
    pipeline_create_info.flags               = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    pipeline_create_info.stageCount          = shader_stages.size();
    pipeline_create_info.pStages             = shader_stages.data();
    pipeline_create_info.pVertexInputState   = &vertex_input_state;
    pipeline_create_info.pInputAssemblyState = &input_assembly_state;
    pipeline_create_info.pTessellationState  = uses_tessellation ? &tessellation_state : nullptr;
    pipeline_create_info.pViewportState      = &viewport_state;
    pipeline_create_info.pRasterizationState = &vk_rasterization_state;
    pipeline_create_info.pMultisampleState   = &vk_multisample_state;
    pipeline_create_info.pDepthStencilState  = &vk_depth_stencil_state;
    pipeline_create_info.pColorBlendState    = &color_blend_state;
    pipeline_create_info.pDynamicState       = &dynamic_state;
    pipeline_create_info.layout              = vk_pso->GetPipelineLayout();
    pipeline_create_info.renderPass          = nullptr;
    pipeline_create_info.subpass             = 0;
    pipeline_create_info.basePipelineHandle  = nullptr; // MARK...
    pipeline_create_info.basePipelineIndex   = -1;

    // vk_pso->CreateGraphicsPipeline(pipeline_create_info);
    VK_CHECK_RESULT(vkCreateGraphicsPipelines(
        m_device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline
    ));

    //destroy shader modules
    for (auto& shader_stage : shader_stages) {
        vkDestroyShaderModule(m_device, shader_stage.module, nullptr);
    }
    return PipelineHandle{
        .handle            = reinterpret_cast<uint64>(vk_pso),
        .binding_infos     = std::move(reflect_flags),
        .hash_2_info_index = std::move(hash_2_idx),
        .valid_bits        = valid_bits,
        .constant_idx      = constant_idx
    };
}

PipelineHandle VulkanDevice::CreatePipeline(PipelineShaderInfo&& _shader_info) {

    auto* vk_pso = MoerNew(VulkanPipelineState)(this, VulkanPipelineState::Compute);

    using TPipelineSets = UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>;

    VkComputePipelineCreateInfo pipeline_create_info{};
    pipeline_create_info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_create_info.pNext              = nullptr;
    pipeline_create_info.flags              = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    pipeline_create_info.stage              = {};
    pipeline_create_info.layout             = nullptr;
    pipeline_create_info.basePipelineHandle = nullptr;

    VkPipelineShaderStageCreateInfo& shader_stage = pipeline_create_info.stage;

    TPipelineSets              descriptor_bindings;
    VkPushConstantRange        push_constant_ranges{.offset = 0, .size = 0};
    uint                       max_set = 0;
    Array<ParamInfoFlags>      reflect_flags(_shader_info.layout_hash.size());
    UnorderedMap<uint64, uint> hash_2_idx;
    int                        constant_idx = -1;
    uint64                     valid_bits   = 0;
    auto merge_reflect_info = [&](const SingleShaderInfo& _info, VkShaderStageFlagBits _stage) {
        MergeReflectInfo(
            *this,
            _info,
            _shader_info,
            _stage,
            hash_2_idx,
            reflect_flags,
            descriptor_bindings,
            push_constant_ranges,
            constant_idx,
            valid_bits,
            max_set
        );
    };

    auto emplace_shader = [&](SingleShaderInfo& _info, VkShaderStageFlagBits _stage) {
        VkShaderModuleCreateInfo shader_module_create_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_module_create_info.codeSize = _info.shader_data.size();
        shader_module_create_info.pCode    = reinterpret_cast<const uint32_t*>(_info.shader_data.data());
        shader_module_create_info.flags    = 0;
        VkShaderModule shader_module;
        VK_CHECK_RESULT(vkCreateShaderModule(m_device, &shader_module_create_info, nullptr, &shader_module));
        shader_stage        = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shader_stage.stage  = _stage;
        shader_stage.module = shader_module;
        shader_stage.pName  = _info.entry_point.data();
    };

    std::visit(
        [&](auto&& _shader_info_group) {
            using T = std::decay_t<decltype(_shader_info_group)>;
            if constexpr (std::is_same_v<T, ShaderCs>) {
                merge_reflect_info(_shader_info_group.cs, VK_SHADER_STAGE_COMPUTE_BIT);
                emplace_shader(_shader_info_group.cs, VK_SHADER_STAGE_COMPUTE_BIT);
            } else {
                LOG_ERROR("Unsupported shader group type: {}", typeid(T).name());
            }
        },
        _shader_info.shader_group
    );

    // init descriptor set layouts and pipeline resource cache
    // Moer::Array<TDescriptorSetLayoutBindingArray> desc_sets_array(max_set + 1u);
    // for (const auto& [set_idx, desc_set] : descriptor_bindings) {
    //     TDescriptorSetLayoutBindingArray desc_set_layouts;
    //     for (const auto& [binding_idx, binding] : desc_set) { desc_set_layouts.push_back(binding); }
    //     desc_sets_array[set_idx] = std::move(desc_set_layouts);
    // }
    // vk_pso->InitDescriptorSetLayouts(desc_sets_array);
    // vk_pso->InitPipelineResourceCache(desc_sets_array);

    // const auto& layouts = vk_pso->GetDescriptorSetsLayout()->GetLayouts();
    if (push_constant_ranges.size != 0) {
        vk_pso->InitPipelineLayout(std::move(descriptor_bindings), std::move(push_constant_ranges));

    } else {
        vk_pso->InitPipelineLayout(std::move(descriptor_bindings));
    }
    // create pipeline layout
    // VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
    // pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    // pipeline_layout_create_info.pNext                  = nullptr;
    // pipeline_layout_create_info.flags                  = 0;
    // pipeline_layout_create_info.setLayoutCount         = layouts.size();
    // pipeline_layout_create_info.pSetLayouts            = layouts.data();
    // pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size == 0 ? 0 : 1;
    // pipeline_layout_create_info.pPushConstantRanges    = &push_constant_ranges;

    // vk_pso->CreatePipelineLayout(pipeline_layout_create_info);

    pipeline_create_info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_create_info.pNext              = nullptr;
    pipeline_create_info.flags              = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    pipeline_create_info.stage              = shader_stage;
    pipeline_create_info.layout             = vk_pso->GetPipelineLayout();
    pipeline_create_info.basePipelineHandle = nullptr;
    pipeline_create_info.basePipelineIndex  = -1;

    VK_CHECK_RESULT(vkCreateComputePipelines(
        m_device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline
    ));

    //destroy shader module
    vkDestroyShaderModule(m_device, shader_stage.module, nullptr);

    return PipelineHandle{
        .handle            = reinterpret_cast<uint64>(vk_pso),
        .binding_infos     = std::move(reflect_flags),
        .hash_2_info_index = std::move(hash_2_idx),
        .valid_bits        = valid_bits,
        .constant_idx      = constant_idx
    };
}

CommandQueue& VulkanDevice::GetCommandQueue(EQueueType _type) {

    switch (_type) {
        case EQueueType::Graphics:
            return *gfx_queue;
        case EQueueType::Compute:
            return *compute_queue;
        case EQueueType::Copy:
        default:
            assert(false && "Unknown queue type.");
    }
    return *gfx_queue;
}

CopyQueue& VulkanDevice::GetCopyQueue() {
    return *copy_queue;
}

TextureRef VulkanDevice::CreateTexture(
    std::string_view   _name,
    ETextureDimension  _dimension,
    Extent3D           _size,
    EPixelFormat       _format,
    ETextureUsageFlags _usage,
    uint32_t           _mip_cnt,
    uint32_t           _array_size
) {

    bool        b_depth = uint(ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT & _usage) != 0;
    TextureInfo info{
        _dimension,
        _usage,
        _format,
        b_depth ? EClearAttachment::DEPTH_STENCIL : EClearAttachment::COLOR,
        _size,
        uint8(_mip_cnt),
        uint8((_dimension == ETextureDimension::TEX_CUBE ? 6 : 1) * _array_size),
        1
    };
    info.aspect_flags = b_depth ? ETextureAspectFlags::DEPTH_SLICE : ETextureAspectFlags::COLOR;
    info.debug_name   = _name;

    return TextureRef{MoerNew(VulkanTexture)(info, this)};
}

BufferRef VulkanDevice::CreateBuffer(
    std::string_view  _name,
    uint              _element_cnt,
    uint              _byte_stride,
    EBufferUsageFlags _usage,
    EPixelFormat      _format
) {
    BufferInfo info{_element_cnt, _byte_stride, _usage, _format};
    return BufferRef{MoerNew(VulkanBuffer)(_name, info, *this)};
}

BindlessArrayRef VulkanDevice::CreateBindlessArray(uint _max_size) {
    return MoerNew(VulkanBindlessArray)(this, _max_size);
}

FenceRef VulkanDevice::CreateFence() {
    return FenceRef{MoerNew(VulkanFence)(*this)};
}

#pragma region[ raytracing ]

RaytracingGeometryRef VulkanDevice::CreateRaytracingGeometry(const RaytracingGeometryInfo& _info) {
    return RaytracingGeometryRef{MoerNew(VulkanRaytracingGeometry)(_info, this)};
}

RaytracingSceneRef VulkanDevice::CreateRaytracingScene() {
    return RaytracingSceneRef{MoerNew(VulkanRaytracingScene)(this)};
}

#pragma endregion

SwapchainRef VulkanDevice::CreateSwapchain(const SwapchainCreateInfo& _info) {
    return SwapchainRef{MoerNew(VkSwapchain)(*this, _info)};
}

IOInterfaceRef VulkanDevice::CreateIOInterface(CopyQueue& _copy_queue) {
    VkCopyQueue* copy_queue_vk = static_cast<VkCopyQueue*>(&_copy_queue);
    return MakeShared<VulkanIOInterface>(*this, *copy_queue_vk);
}
void VulkanDevice::EnqueueDeferredRelease(RHIResource* _object) {
    deferred_release_queue.Push(_object);
}

void VulkanDevice::FlushDeferredReleases() {
    Array<RHIResource*> objects;
    deferred_release_queue.PopAll(objects);
    for (auto* object : objects) {
        MoerDelete(object);
    }
}

const VkSampler VulkanDevice::GetSampler(Sampler _sampler) const {
    uint filter  = uint(_sampler.filter);
    uint address = uint(_sampler.address_mode);
    uint compare = uint(_sampler.compare_function);

    uint idx = (uint(SF_Num) * uint(SAM_Num)) * compare + (uint(SF_Num)) * address + filter;
    return immutable_samplers[idx];
}

void VulkanDevice::SetResourceName(uint64 _handle, VkObjectType _type, const std::string_view _name) {

    VkDebugUtilsObjectNameInfoEXT name_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    name_info.objectType   = _type;
    name_info.objectHandle = _handle;
    name_info.pObjectName  = _name.data();
    vkSetDebugUtilsObjectNameEXT(m_device, &name_info);
}

RuntimePlugin* VulkanDevice::LoadPlugin(std::string_view _name) {
    auto ite = exts.find(_name.data());
    if (ite == exts.end())
        return nullptr;
    auto& v = ite->second;
    {
        std::lock_guard lck{ext_mutex};
        if (v.ext == nullptr) {
            v.ext = v.ctor(this);
        }
    }
    return v.ext;
}

void VulkanDevice::CopyData(const BufferView& _dst, const void* _data, uint64 _size) {
    auto* buffer = ResourceCast(_dst.buffer);
    VK_CHECK_RESULT(
        vmaCopyMemoryToAllocation(m_allocator, _data, buffer->GetAllocation(), _dst.GetByteOffset(), _size)
    );
}
void VulkanDevice::CopyData(void* _dst, const BufferView& _src, uint64 _size) {
    auto* buffer = ResourceCast(_src.buffer);
    VK_CHECK_RESULT(
        vmaCopyAllocationToMemory(m_allocator, buffer->GetAllocation(), _src.GetByteOffset(), _dst, _size)
    );
}

void VulkanDevice::LoadDefaultExtensions() {

#if WITH_NRD
    exts.try_emplace(
        Moer::Render::Ext::NRDPlugin::name.data(),
        [](VulkanDevice* _device) -> RuntimePlugin* {
            return MoerNew(Moer::Render::Ext::VkNRDPlugin(_device));
        },
        [](RuntimePlugin* _ext) {
            MoerDelete(static_cast<Moer::Render::Ext::VkNRDPlugin*>(_ext));
        }
    );
#endif
}

// RHIViewportRef VulkanDevice::CreateViewport(const RHIViewportInitializer& _init) {
//     VulkanSwapChain* swapchain = MoerNew(VulkanSwapChain)();
//     uint32_t         width, height;
//     VkSurfaceKHR     surface;
//     Moer::WindowContext::CreateVulkanSurface(m_instance, _init.window_handle, nullptr, &surface);
//     swapchain->Connect(m_instance, surface, this);
//     swapchain->Init(&width, &height, _init.b_vsync);

//     Moer::Render::VulkanRHIViewport* viewport = MoerNew(Moer::Render::VulkanRHIViewport)(swapchain, 2);

//     return viewport;
// }
}; // namespace Moer::Render
