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
#include "string/Format.h"
#include "string/StringConvert.h"
#include "vulkanextension/VulkanExtension.h"

#include "log/LogSystem.h"
// #include "misc/MacroUtils.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"

#include "Core.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/ThreadManager.h"
#include "vulkan/vulkan_core.h"

// #include <vk_mem_alloc.h>
#include "VulkanMemoryAllocator.h"

#if PLATFORM_WINDOWS
#include <vulkan/vulkan_win32.h>
#endif

#include "VulkanIOService.h"
#include <algorithm>
#include <config.h>
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

namespace {

struct DescriptorHeapCommandCheck {
    const char* name;
    PFN_vkVoidFunction address;
};

} // namespace

VulkanDevice::VulkanDevice(const VulkanRHIConfig&& _config) : RenderDevice::Impl() {
    InitVulkanInstance(_config.api_version);

    m_gpu = SelectGpu(_config.api_version);
    InitGpu(_config.api_version);

    CreateDevice(_config.api_version);
    CreateMemoryAllocator(m_instance, _config.api_version);

    CreateInternalResources();

    LoadDefaultExtensions();
}

void VulkanDevice::PostInit() {
    if (!HasDescriptorHeapRuntime()) {
        return;
    }
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

#ifdef MOER_VK_LAYER_PATH
    constexpr std::string_view vk_layer_path = MOER_XSTR(MOER_VK_LAYER_PATH);
    std::filesystem::path      layer_path(vk_layer_path);
    if (std::filesystem::exists(layer_path)) {
        Platform::SetEnv("VK_LAYER_PATH", MOER_XSTR(MOER_VK_LAYER_PATH));
        LOG_INFO(MOER_TEXT("Set VK_LAYER_PATH to {}"), MOER_XSTR(MOER_VK_LAYER_PATH));
    }
#endif

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
            LOG_WARNING(MOER_TEXT("Layer '{}' is not supported."), _view.data());
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
                b_validation_layer_enabled = true;
            }
            instance_layers_loaded.emplace_back(layer.data());
        }
        // other layers, not fully implemented
    }
    m_validation_layer_enabled = b_validation_layer_enabled;
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
            LOG_ERROR(MOER_TEXT("Reqired instance extension '{}' is not supported"), _ext);
            return false;
        }
        return true;
    };

    const auto instance_extensions_required = VulkanInstanceExtension::GetMERequiredInstanceExtensions();
    static constexpr std::string_view optional_instance_extensions[] = {
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME
    };

    Array<const char*> instance_extensions_loaded;
    bool               b_instance_extensions_fully_supported = true;
    for (const auto& ext : instance_extensions_required) {
        if (is_extension_supported(ext.data()))
            instance_extensions_loaded.emplace_back(ext.data());
        else
            b_instance_extensions_fully_supported = false;
    }
    for (const auto optional_ext : optional_instance_extensions) {
        if (instance_extensions.contains(optional_ext.data())) {
            instance_extensions_loaded.emplace_back(optional_ext.data());
        } else {
            LOG_WARNING(MOER_TEXT("Optional instance extension '{}' is not supported"), optional_ext);
        }
    }
    CHECK_ASSERT(
        b_instance_extensions_fully_supported, "Not all required instance extensions are supported."
    );

    const bool b_debug_utils_extension_enabled = std::find_if(
        instance_extensions_loaded.begin(),
        instance_extensions_loaded.end(),
        [](const char* ext_name) {
            return ext_name != nullptr &&
                   std::string_view(ext_name) == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        }
    ) != instance_extensions_loaded.end();

    if (b_validation_layer_enabled && b_debug_utils_extension_enabled) {
        PopulateDebugMessengerCreateInfo(debug_create_info);
        debug_create_info.pNext    = instance_create_info.pNext;
        instance_create_info.pNext = &debug_create_info;
    }

    instance_create_info.enabledExtensionCount   = instance_extensions_loaded.size();
    instance_create_info.ppEnabledExtensionNames = instance_extensions_loaded.data();
    m_device_info.has_surface_maintenance1_instance =
        std::find_if(
            instance_extensions_loaded.begin(),
            instance_extensions_loaded.end(),
            [](const char* ext_name) {
                return ext_name != nullptr &&
                       std::string_view(ext_name) == VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
            }
        ) != instance_extensions_loaded.end();

    if (m_device_info.has_surface_maintenance1_instance) {
        LOG_INFO(MOER_TEXT("Loading VulkanInstanceExtension: {}"), VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }

    VK_CHECK_RESULT(vkCreateInstance(&instance_create_info, nullptr, &m_instance))
    volkLoadInstance(m_instance);

    if (b_validation_layer_enabled && b_debug_utils_extension_enabled)
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
            LOG_ERROR(MOER_TEXT("GPU does NOT satisfy required core Vulkan features."));
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
                LOG_ERROR(MOER_TEXT("GPU Device type is invalid!"));
        }

        auto indices = QueryQueueFamilyIndices(gpu);
        if (indices.IsComplete() && is_extensions_required_supported(gpu) &&
            is_core_features_supported(gpu)) {
            priority += 100;
            gpu_candidates.push_back(SelectGpuCandidate{gpu, priority});
        }
    }

    if (gpu_candidates.empty()) {
        LOG_ERROR(MOER_TEXT("No available GPU. Details:"));

        // 仅在失败分支里输出调试信息：按 GPU 输出缺失的必需扩展
        for (const auto& gpu : gpu_list) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(gpu, &props);

            auto gpu_extensions = VulkanDevice::GetGpuExtensions(gpu);

            Utf8String log_msg = Utf8Printf("GPU '{}':\n", static_cast<const char*>(props.deviceName));

            bool has_missing = false;
            for (const auto& extension : extensions_required) {
                const auto& required_name = extension->GetExtensionName();
                const bool  is_optional   = extension->IsOptional();

                if (!gpu_extensions.contains(required_name.data()) && !is_optional) {
                    has_missing = true;
                    log_msg += Utf8StringView(required_name.data(), required_name.size());
                    log_msg += Utf8StringView("\n");
                }
            }

            if (has_missing) {
                LOG_ERROR(MOER_TEXT("{}"), log_msg);
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

    LOG_INFO(MOER_TEXT("VulkanRHI: GPU initialized."));
    LOG_INFO(
        MOER_TEXT("\n- DeviceName: {}.")
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
        LOG_INFO(MOER_TEXT("Loading VulkanDeviceExtension: {}"), extension->GetExtensionName());
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
    FinalizeDescriptorHeapRuntimeCapability();

    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.graphics.value(), 0, &m_graphics_queue);

    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.present.value(), 0, &m_present_queue);
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.compute.value(), 0, &m_compute_queue);
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.transfer.value(), 0, &m_transfer_queue);
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.raytracing.value(), 0, &m_raytracing_queue);

    gfx_queue = MakeUnique<VkCommandQueue>(*this, EQueueType::Graphics);
    SetResourceName(uint64(m_graphics_queue), VK_OBJECT_TYPE_QUEUE, MOER_TEXT("GraphicsQueue"));
    compute_queue = MakeUnique<VkCommandQueue>(*this, EQueueType::Compute);
    SetResourceName(uint64(m_compute_queue), VK_OBJECT_TYPE_QUEUE, MOER_TEXT("ComputeQueue"));
    // transfer_queue = MakeUnique<VkCommandQueue>(*this, EQueueType::Copy);
    copy_queue = MakeUnique<VkCopyQueue>(*this);
    SetResourceName(uint64(m_transfer_queue), VK_OBJECT_TYPE_QUEUE, MOER_TEXT("TransferQueue"));

    if (m_graphics_queue == m_transfer_queue) {
        LOG_WARNING(
            MOER_TEXT("gfx and transfer share the same VkQueue handle. ")
            "Installing shared submit mutex to avoid concurrent vkQueueSubmit2."
        );
        gfx_queue->SetQueueSubmitMutex(&m_shared_queue_submit_mutex);
        copy_queue->SetQueueSubmitMutex(&m_shared_queue_submit_mutex);
    }
    if (m_compute_queue == m_graphics_queue) {
        LOG_WARNING(
            MOER_TEXT("compute and gfx share the same VkQueue handle. ")
            "Installing shared submit mutex to avoid concurrent vkQueueSubmit2."
        );
        gfx_queue->SetQueueSubmitMutex(&m_shared_queue_submit_mutex);
        compute_queue->SetQueueSubmitMutex(&m_shared_queue_submit_mutex);
    }
}

void VulkanDevice::CreateMemoryAllocator(VkInstance _instance, uint32 _api_version) {
    VmaAllocatorCreateInfo alloc_create_info{};

    alloc_create_info.vulkanApiVersion = _api_version;

    alloc_create_info.instance         = _instance;
    alloc_create_info.physicalDevice   = m_gpu;
    alloc_create_info.device           = m_device;

    //capable of using buffer via device address(64bit) passed to shader.
    alloc_create_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (m_device_info.optional_extensions.m_has_memory_priority) {
        alloc_create_info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    }

#if WITH_CUDA
    alloc_create_info.flags |= VMA_ALLOCATOR_CREATE_KHR_EXTERNAL_MEMORY_WIN32_BIT;

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

    VmaVulkanFunctions vma_functions{};
    VK_CHECK_RESULT(vmaImportVulkanFunctionsFromVolk(&alloc_create_info, &vma_functions));
    alloc_create_info.pVulkanFunctions = &vma_functions;

    VK_CHECK_RESULT(vmaCreateAllocator(&alloc_create_info, &m_allocator));

    LOG_INFO(MOER_TEXT("Vulkan Memory Allocator initialized with api version: {}."), alloc_create_info.vulkanApiVersion);
}

bool VulkanDevice::ValidateDescriptorHeapRuntimeCommands() const {
    const DescriptorHeapCommandCheck required_commands[] = {
        {"vkCmdBindResourceHeapEXT", reinterpret_cast<PFN_vkVoidFunction>(m_vk_cmd_bind_resource_heap)},
        {"vkCmdBindSamplerHeapEXT", reinterpret_cast<PFN_vkVoidFunction>(m_vk_cmd_bind_sampler_heap)},
        {"vkWriteResourceDescriptorsEXT", reinterpret_cast<PFN_vkVoidFunction>(m_vk_write_resource_descriptors)},
        {"vkWriteSamplerDescriptorsEXT", reinterpret_cast<PFN_vkVoidFunction>(m_vk_write_sampler_descriptors)},
        {"vkCmdPushDataEXT", reinterpret_cast<PFN_vkVoidFunction>(m_vk_cmd_push_data)},
    };

    bool all_loaded = true;
    for (const auto& command : required_commands) {
        if (command.address != nullptr) {
            continue;
        }
        LOG_ERROR(
            MOER_TEXT("VulkanRHI: descriptor heap disabled: required command {} is unavailable after volkLoadDevice."),
            command.name
        );
        all_loaded = false;
    }
    return all_loaded;
}

void VulkanDevice::LoadDescriptorHeapRuntimeCommands() {
    m_vk_cmd_bind_resource_heap = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(
        vkGetDeviceProcAddr(m_device, "vkCmdBindResourceHeapEXT")
    );
    m_vk_cmd_bind_sampler_heap = reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(
        vkGetDeviceProcAddr(m_device, "vkCmdBindSamplerHeapEXT")
    );
    m_vk_write_resource_descriptors = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(
        vkGetDeviceProcAddr(m_device, "vkWriteResourceDescriptorsEXT")
    );
    m_vk_write_sampler_descriptors = reinterpret_cast<PFN_vkWriteSamplerDescriptorsEXT>(
        vkGetDeviceProcAddr(m_device, "vkWriteSamplerDescriptorsEXT")
    );
    m_vk_cmd_push_data = reinterpret_cast<PFN_vkCmdPushDataEXT>(
        vkGetDeviceProcAddr(m_device, "vkCmdPushDataEXT")
    );
}

void VulkanDevice::FinalizeDescriptorHeapRuntimeCapability() {
    auto& optional_extensions = m_device_info.optional_extensions;
    optional_extensions.m_has_descriptor_heap_runtime = false;

    if (!optional_extensions.m_has_ext_descriptor_heap) {
        LOG_WARNING(
            MOER_TEXT("VulkanRHI: descriptor heap disabled: VK_EXT_descriptor_heap unsupported or feature query rejected.")
        );
        return;
    }

    LoadDescriptorHeapRuntimeCommands();

    if (!ValidateDescriptorHeapRuntimeCommands()) {
        LOG_ERROR(
            MOER_TEXT("VulkanRHI: descriptor heap disabled: VK_EXT_descriptor_heap was enabled but runtime command loading is incomplete.")
        );
        return;
    }

    optional_extensions.m_has_descriptor_heap_runtime = true;
    LOG_INFO(
        MOER_TEXT("VulkanRHI: descriptor heap runtime available: maxResourceHeapSize={}, maxSamplerHeapSize={}, maxPushDataSize={}"),
        m_device_info.optional_properties.descriptor_heap_properties.maxResourceHeapSize,
        m_device_info.optional_properties.descriptor_heap_properties.maxSamplerHeapSize,
        m_device_info.optional_properties.descriptor_heap_properties.maxPushDataSize
    );
}

void VulkanDevice::CreateDescriptorHeap() {
    m_global_descriptor_heap = MakeUnique<VulkanDescriptorHeap>(*this);
    //create empty descriptor set layout
    VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    descriptor_set_layout_create_info.flags = 0;
    vkCreateDescriptorSetLayout(
        m_device, &descriptor_set_layout_create_info, VK_NULL_HANDLE, &empty_descriptor_set_layout
    );

    if (HasDescriptorHeapRuntime()) {
        LOG_INFO(MOER_TEXT("VulkanRHI: Descriptor Heap initialized."));
        return;
    }

    LOG_WARNING(
        MOER_TEXT("VulkanRHI: descriptor heap startup skipped; this runtime requires VK_EXT_descriptor_heap.")
    );
}

VkResult VulkanDevice::WriteResourceDescriptors(
    uint32                             _descriptor_count,
    const VkResourceDescriptorInfoEXT* _descriptor_infos,
    const VkHostAddressRangeEXT*       _dst_ranges
) const {
    assert(m_vk_write_resource_descriptors != nullptr && "vkWriteResourceDescriptorsEXT is unavailable");
    return m_vk_write_resource_descriptors(m_device, _descriptor_count, _descriptor_infos, _dst_ranges);
}

VkResult VulkanDevice::WriteSamplerDescriptors(
    uint32                       _descriptor_count,
    const VkSamplerCreateInfo*   _sampler_infos,
    const VkHostAddressRangeEXT* _dst_ranges
) const {
    assert(m_vk_write_sampler_descriptors != nullptr && "vkWriteSamplerDescriptorsEXT is unavailable");
    return m_vk_write_sampler_descriptors(m_device, _descriptor_count, _sampler_infos, _dst_ranges);
}

void VulkanDevice::CmdBindResourceHeap(VkCommandBuffer _command_buffer, const VkBindHeapInfoEXT* _bind_info) const {
    assert(m_vk_cmd_bind_resource_heap != nullptr && "vkCmdBindResourceHeapEXT is unavailable");
    m_vk_cmd_bind_resource_heap(_command_buffer, _bind_info);
}

void VulkanDevice::CmdBindSamplerHeap(VkCommandBuffer _command_buffer, const VkBindHeapInfoEXT* _bind_info) const {
    assert(m_vk_cmd_bind_sampler_heap != nullptr && "vkCmdBindSamplerHeapEXT is unavailable");
    m_vk_cmd_bind_sampler_heap(_command_buffer, _bind_info);
}

void VulkanDevice::CmdPushData(VkCommandBuffer _command_buffer, const VkPushDataInfoEXT* _push_info) const {
    assert(m_vk_cmd_push_data != nullptr && "vkCmdPushDataEXT is unavailable");
    m_vk_cmd_push_data(_command_buffer, _push_info);
}

uint64 VulkanDevice::GetPhysicalDescriptorSize(VkDescriptorType _type) const {
    const auto& heap_props = m_device_info.optional_properties.descriptor_heap_properties;
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            return heap_props.samplerDescriptorSize;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return heap_props.imageDescriptorSize;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return heap_props.bufferDescriptorSize;
        default:
            LOG_ERROR(MOER_TEXT("Unsupported physical descriptor size query: {}"), VK_TYPE_TO_STRING(VkDescriptorType, _type));
            assert(false && "Unsupported physical descriptor size query");
            return 0;
    }
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

VkFence VulkanDevice::AcquireHostFence() {
    HostFenceSlot* slot = host_fence_pool.Pop();
    if (slot != nullptr) {
        VkFence fence = slot->handle;
        delete slot;
        return fence;
    }

    VkFenceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkFence created_fence = VK_NULL_HANDLE;
    VK_CHECK_RESULT(vkCreateFence(m_device, &create_info, nullptr, &created_fence));
    return created_fence;
}

void VulkanDevice::RecycleHostFence(VkFence fence) {
    if (fence == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE) {
        return;
    }
    VK_CHECK_RESULT(vkResetFences(m_device, 1, &fence));
    auto* slot   = new HostFenceSlot();
    slot->handle = fence;
    host_fence_pool.Push(slot);
}

void VulkanDevice::DestroyHostFencePool() {
    while (HostFenceSlot* slot = host_fence_pool.Pop()) {
        vkDestroyFence(m_device, slot->handle, VK_NULL_HANDLE);
        delete slot;
    }
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
    m_global_descriptor_heap.reset();
    if (empty_descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, empty_descriptor_set_layout, VK_NULL_HANDLE);
        empty_descriptor_set_layout = VK_NULL_HANDLE;
    }
}

void VulkanDevice::Destroy() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    WaitIdle();

    // for (auto& cmd_allocator : m_command_allocators) {
    //     CHECK_AND_DELETE(cmd_allocator);
    // }
    compute_queue.reset();
    gfx_queue.reset();
    copy_queue.reset();
    DestroyInternalShaders();
    FlushDeferredReleases();
    DestroyInternalResources();
    FlushDeferredReleases();
    DestroyHostFencePool();
    if (m_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
    }
    vkDestroyDevice(m_device, VK_NULL_HANDLE);
    m_device = VK_NULL_HANDLE;

    LOG_INFO(MOER_TEXT("VulkanRHI: Device destroyed."));

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
                (queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
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

bool VulkanDevice::SupportsTimestampQueries(EQueueType _type) const {
    if (_type == EQueueType::Ignore || _type == EQueueType::Num) {
        return false;
    }

    const uint32 family_index = GetQueueFamilyIndex(_type);
    if (family_index >= m_device_info.queue_family_props.size()) {
        return false;
    }

    const VkQueueFamilyProperties& family_props = m_device_info.queue_family_props[family_index];
    if (family_props.timestampValidBits == 0) {
        return false;
    }

    if (_type == EQueueType::Graphics || _type == EQueueType::Compute) {
        return m_device_info.core_properties.core_1_0.limits.timestampComputeAndGraphics == VK_TRUE;
    }

    return true;
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
            MOER_TEXT("AMD GPU '{}' (vendorID={:#x}) detected. ")
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
        LOG_ERROR(MOER_TEXT("VulkanRHI: TryConvertCooperativeVectorMatrix failed: {}"), _message);
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
        log_error(VK_TYPE_TO_STRING(VkResult, result));
        return false;
    }
    if (required_dst_size > _dst_data.size_bytes()) {
        LOG_ERROR(
            MOER_TEXT("VulkanRHI: TryConvertCooperativeVectorMatrix failed: destination buffer is too small ")
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
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    vkDeviceWaitIdle(m_device);

    if (gfx_queue) {
        gfx_queue->Sync();
    }
    if (compute_queue) {
        compute_queue->Sync();
    }
    if (copy_queue) {
        auto* copy_timeline = ResourceCast(copy_queue->GetFenceHandle().Get());
        if (copy_timeline != nullptr) {
            copy_queue->Sync(copy_timeline->GetDeviceValue());
        }
    }
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

static void MergeDirectHeapBuiltin(
    const std::optional<ReflectParamInfo::Bindless>& _src,
    std::optional<ReflectParamInfo::Bindless>&       _dst
) {
    if (!_src.has_value()) {
        return;
    }
    if (!_dst.has_value()) {
        _dst = _src;
        return;
    }

    assert(_dst->set == _src->set && "Descriptor heap builtin set mismatch.");
    assert(_dst->binding == _src->binding && "Descriptor heap builtin binding mismatch.");
    assert(_dst->count == _src->count && "Descriptor heap builtin count mismatch.");
    _dst->stage_bits |= _src->stage_bits;
    _dst->custom_flag.active |= _src->custom_flag.active;
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
    VulkanDirectHeapBuiltins& _out_direct_heap_builtins,
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
                bool b_found_bindless_indirect                      = bdls_array.array.has_value();
                bool b_found_resource_heap                         = bdls_array.resource_heap.has_value();
                bool b_found_sampler_heap                          = bdls_array.sampler_heap.has_value();
                if (b_found_bindless_indirect) {
                    const ReflectParamInfo::Bindless& indirect_binding_info = bdls_array.array.value();
                    const uint indirect_binding = indirect_binding_info.binding;

                    auto& array_set            = _out_descriptor_bindings[indirect_binding_info.set];
                    auto& vk_binding           = array_set[indirect_binding];
                    vk_binding.binding         = indirect_binding;
                    vk_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    vk_binding.descriptorCount = 1;
                    vk_binding.stageFlags |= _stage;
                    vk_binding.pImmutableSamplers = nullptr;
                    _max_set   = uint(std::max(int(_max_set), int(indirect_binding_info.set)));
                    array_set.bindings[indirect_binding].param_idx = idx;
                    array_set.is_bindless                       = true;
                }
                MergeDirectHeapBuiltin(bdls_array.resource_heap, _out_direct_heap_builtins.resource_heap);
                MergeDirectHeapBuiltin(bdls_array.sampler_heap, _out_direct_heap_builtins.sampler_heap);
                set_valid_bits(
                    idx,
                    (b_found_bindless_indirect && bdls_array.array->custom_flag.active) ||
                        (b_found_resource_heap && bdls_array.resource_heap->custom_flag.active) ||
                        (b_found_sampler_heap && bdls_array.sampler_heap->custom_flag.active)
                );
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
    VulkanPipelineState* vk_pso = MoerNew(VulkanPipelineState)(this, VulkanPipelineState::GFX);

    uint32_t attachment_count = _create_info.color_attachment_count;

    Moer::Array<VkFormat> color_attachment_formats(attachment_count);

    for (int i = 0; i < attachment_count; ++i) {
        color_attachment_formats[i] =
            VulkanEnumTranslator::METoVKFormat(_create_info.color_attachments_info[i].pixel_format);
    }
    VkPipelineRenderingCreateInfo rendering_create_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering_create_info.pNext                   = nullptr;
    rendering_create_info.viewMask                = 0;
    rendering_create_info.colorAttachmentCount    = attachment_count;
    rendering_create_info.pColorAttachmentFormats = color_attachment_formats.data();
    rendering_create_info.depthAttachmentFormat =
        VulkanEnumTranslator::METoVKFormat(_create_info.depth_stencil_format);

    // Only set stencil format if the depth format actually has a stencil aspect
    bool depth_has_stencil =
        (_create_info.depth_stencil_format == PF_D32_SFLOAT_S8_UINT ||
         _create_info.depth_stencil_format == PF_D24_UNORM_S8_UINT ||
         _create_info.depth_stencil_format == PF_D16_UNORM_S8_UINT ||
         _create_info.depth_stencil_format == PF_S8_UINT);
    rendering_create_info.stencilAttachmentFormat =
        depth_has_stencil ? VulkanEnumTranslator::METoVKFormat(_create_info.depth_stencil_format) :
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
    TPipelineSets             descriptor_bindings;
    VulkanDirectHeapBuiltins  direct_heap_builtins;
    VkPushConstantRange       push_constant_ranges{.offset = 0, .size = 0};
    uint                      max_set = 0;

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
            direct_heap_builtins,
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
        vk_pso->InitPipelineLayout(
            std::move(descriptor_bindings),
            std::move(push_constant_ranges),
            std::move(direct_heap_builtins)
        );

    } else {
        vk_pso->InitPipelineLayout(std::move(descriptor_bindings), std::nullopt, std::move(direct_heap_builtins));
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
    VkPipelineCreateFlags2CreateInfo pipeline_flags2_create_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &rendering_create_info,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT
    };
    pipeline_create_info.pNext               = &pipeline_flags2_create_info;
    pipeline_create_info.flags               = 0;

    Array<VkShaderDescriptorSetAndBindingMappingInfoEXT> stage_mapping_infos(shader_stages.size());
    for (uint32_t stage_index = 0; stage_index < shader_stages.size(); ++stage_index) {
        stage_mapping_infos[stage_index] = {
            .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
            .pNext = shader_stages[stage_index].pNext,
            .mappingCount = uint32_t(vk_pso->bind_template->descriptor_mappings.size()),
            .pMappings = vk_pso->bind_template->descriptor_mappings.data()
        };
        shader_stages[stage_index].pNext = &stage_mapping_infos[stage_index];
    }

    pipeline_create_info.stageCount          = shader_stages.size();
    pipeline_create_info.pStages             = shader_stages.data();
    pipeline_create_info.pVertexInputState   = &vertex_input_state;
    pipeline_create_info.pInputAssemblyState = &input_assembly_state;
    pipeline_create_info.pTessellationState  = nullptr;
    pipeline_create_info.pViewportState      = &viewport_state;
    pipeline_create_info.pRasterizationState = &vk_rasterization_state;
    pipeline_create_info.pMultisampleState   = &vk_multisample_state;
    pipeline_create_info.pDepthStencilState  = &vk_depth_stencil_state;
    pipeline_create_info.pColorBlendState    = &color_blend_state;
    pipeline_create_info.pDynamicState       = &dynamic_state;
    pipeline_create_info.layout              = VK_NULL_HANDLE;
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
    pipeline_create_info.flags              = 0;
    pipeline_create_info.stage              = {};
    pipeline_create_info.layout             = nullptr;
    pipeline_create_info.basePipelineHandle = nullptr;

    VkPipelineShaderStageCreateInfo& shader_stage = pipeline_create_info.stage;

    TPipelineSets              descriptor_bindings;
    VulkanDirectHeapBuiltins   direct_heap_builtins;
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
            direct_heap_builtins,
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
                LOG_ERROR(MOER_TEXT("Unsupported shader group type: {}"), typeid(T).name());
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
        vk_pso->InitPipelineLayout(
            std::move(descriptor_bindings),
            std::move(push_constant_ranges),
            std::move(direct_heap_builtins)
        );

    } else {
        vk_pso->InitPipelineLayout(std::move(descriptor_bindings), std::nullopt, std::move(direct_heap_builtins));
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
    VkPipelineCreateFlags2CreateInfo pipeline_flags2_create_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT
    };
    pipeline_create_info.pNext              = &pipeline_flags2_create_info;
    VkShaderDescriptorSetAndBindingMappingInfoEXT stage_mapping_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
        .pNext = shader_stage.pNext,
        .mappingCount = uint32_t(vk_pso->bind_template->descriptor_mappings.size()),
        .pMappings = vk_pso->bind_template->descriptor_mappings.data()
    };
    shader_stage.pNext = &stage_mapping_info;
    pipeline_create_info.flags              = 0;
    pipeline_create_info.stage              = shader_stage;
    pipeline_create_info.layout             = VK_NULL_HANDLE;
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

TextureRef VulkanDevice::CreateTexture(StringView _name, const TextureInfo& _info) {
    TextureInfo info = _info;
    if (!info.debug_name.has_value()) {
        info.debug_name = String(_name);
    }
    return TextureRef{MoerNew(VulkanTexture)(info, this)};
}

BufferRef VulkanDevice::CreateBuffer(StringView _name, const BufferInfo& _info) {
    BufferInfo info = _info;
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

void VulkanDevice::SetResourceName(uint64 _handle, VkObjectType _type, StringView _name) {

    VkDebugUtilsObjectNameInfoEXT name_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    Utf8String utf8_name = PlatformToUtf8(_name);
    name_info.objectType   = _type;
    name_info.objectHandle = _handle;
    name_info.pObjectName  = utf8_name.c_str();
    vkSetDebugUtilsObjectNameEXT(m_device, &name_info);
}

RuntimePlugin* VulkanDevice::LoadPlugin(StringView _name) {
    auto ite = exts.find(String(_name));
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
        Moer::Render::Ext::NRDPlugin::name,
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
//     swapchain->Connect(m_instance, surface, this);
//     swapchain->Init(&width, &height, _init.b_vsync);

//     Moer::Render::VulkanRHIViewport* viewport = MoerNew(Moer::Render::VulkanRHIViewport)(swapchain, 2);

//     return viewport;
// }
}; // namespace Moer::Render
