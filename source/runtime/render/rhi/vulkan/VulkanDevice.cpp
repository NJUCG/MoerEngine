//
// Created by 74535 on 2023/10/2.
//

#include "VulkanEnumTranslator.h"
#include "VulkanBindlessArray.h"
#include "VulkanRaytracingResource.h"
#include "VulkanTimeline.h"
#include "VulkanDevice.h"
#include "PixelFormat.h"
#include "VulkanCommandList.h"
#include "VulkanDebugCallback.h"
#include "VulkanMacroUtils.h"
#include "VulkanPlatform.h"
#include "VulkanQueue.h"
#include "VulkanRHIResource.h"
#include "VulkanSwapChain.h"
#include "diagnostics/VulkanSubmissionDiagnosticsInternal.h"
#include "plugin/VulkanCooperativeSupport.h"
#include "plugin/VulkanNrdPlugin.h"
#include "vulkanextension/VulkanExtension.h"
#include <string_view>

#include "log/LogSystem.h"
// #include "misc/MacroUtils.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIMultiview.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/RHIThreadHeartbeat.h"

#include "Core.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/ThreadManager.h"
#include "vulkan/vk_enum_string_helper.h"

// #include <vk_mem_alloc.h>
#include "VulkanMemoryAllocator.h"

#include <algorithm>
#include <array>
#include <config.h>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <platform/Platform.h>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <variant>

#ifndef MOER_STR
#define MOER_STR(x)  #x
#define MOER_XSTR(x) MOER_STR(x)
#endif

namespace Moer::Render {

VulkanDevice::VulkanDevice(const VulkanRHIConfig& _config) : RenderDevice::Impl() {
    rhi_thread_enabled            = _config.rhi_thread && !_config.rhi_bypass;
    thread_profile_logging        = _config.thread_profile_logging;
    parallel_recording            = _config.parallel_recording;
    parallel_record_workers       = _config.parallel_record_workers;
    parallel_record_verify        = _config.parallel_record_verify;
    parallel_record_profile       = _config.parallel_record_profile;
    parallel_record_min_work_units_per_job =
        std::max(1u, _config.parallel_record_min_work_units_per_job);
    parallel_record_worker_throw_trigger =
        _config.parallel_record_worker_throw_trigger;
    present_submit_fault_trigger = _config.present_submit_fault_trigger;
    if (parallel_record_worker_throw_trigger != 0) {
        LOG_INFO(
            "[ParallelRecord][FaultConfig] point=worker-throw trigger={}",
            parallel_record_worker_throw_trigger
        );
    }
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

    try {
        InitVulkanInstance(_config.api_version);

        m_gpu = SelectGpu(_config.api_version);
        InitGpu(_config.api_version);

        CreateDevice();
        CreateMemoryAllocator(m_instance, _config.api_version);

        CreateInternalResources();

        RegisterDefaultPlugins();
        LogGpuEnvironment(_config.api_version);
    } catch (...) {
        Destroy();
        throw;
    }
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

    validation_layer_requested = std::ranges::any_of(
        instance_layers_required,
        [](std::string_view layer) { return layer == "VK_LAYER_KHRONOS_validation"; }
    );
    validation_layer_available =
        instance_layers.contains("VK_LAYER_KHRONOS_validation");

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
    const auto instance_extensions_optional =
        VulkanInstanceExtension::GetMEOptionalInstanceExtensions();
    Array<const char*> instance_extensions_loaded;
    bool               b_instance_extensions_fully_supported = true;
    for (const auto& ext : instance_extensions_required) {
        if (is_extension_supported(ext.data()))
            instance_extensions_loaded.emplace_back(ext.data());
        else
            b_instance_extensions_fully_supported = false;
    }
    for (const auto& ext : instance_extensions_optional) {
        if (!instance_extensions.contains(ext.data())) {
            continue;
        }
        instance_extensions_loaded.emplace_back(ext.data());
    }
    CHECK_ASSERT(
        b_instance_extensions_fully_supported, "Not all required instance extensions are supported."
    );

    instance_create_info.enabledExtensionCount   = instance_extensions_loaded.size();
    instance_create_info.ppEnabledExtensionNames = instance_extensions_loaded.data();

    VK_CHECK_RESULT(vkCreateInstance(&instance_create_info, nullptr, &m_instance));
    volkLoadInstance(m_instance);

    if (b_validation_layer_enabled) {
        SetupDebugUtilsMessengerEXT();
    }
    validation_layer_enabled =
        b_validation_layer_enabled && m_debug_utils_messenger != VK_NULL_HANDLE;
}

/**
    * @brief Select gpu, check extension support, etc.
    * Only check core extensions and core features support.
    * @param _api_version
    * @return selected gpu
    */
VkPhysicalDevice VulkanDevice::SelectGpu(uint32 _api_version) {
    uint32 gpu_count = 0;
    VK_CHECK_RESULT(vkEnumeratePhysicalDevices(m_instance, &gpu_count, nullptr));
    CHECK_ASSERT(gpu_count, "No GPU with Vulkan support found!");

    Array<VkPhysicalDevice> gpu_list(gpu_count);
    VK_CHECK_RESULT(vkEnumeratePhysicalDevices(m_instance, &gpu_count, gpu_list.data()));

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

    const auto& features_required          = VulkanDeviceFeatures::BuildRequiredFeatures(_api_version);
    const auto& is_core_features_supported = [&](VkPhysicalDevice _gpu) {
        auto gpu_features = VulkanDeviceFeatures::QuerySupportedFeatures(_gpu, _api_version);
        bool ok           = gpu_features.SupportsAll(features_required);
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

        auto indices = SelectQueueFamilyIndices(gpu);
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
    m_device_info.core_features = VulkanDeviceFeatures::QuerySupportedFeatures(m_gpu, _api_version);
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
    m_device_info.core_properties = VulkanCoreDeviceProperties::QueryGpuProperties(m_gpu, _api_version);
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
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &m_device_info.memory_properties);

    // Select queue families, then retain their hardware properties.
    m_device_info.queue_family_indices = SelectQueueFamilyIndices(m_gpu);
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

void VulkanDevice::LogGpuEnvironment(uint32 _requested_api_version) const {
    const auto& properties = m_device_info.core_properties.core_1_0;
    const char* device_type = "unknown";
    switch (properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            device_type = "other";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            device_type = "integrated_gpu";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            device_type = "discrete_gpu";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            device_type = "virtual_gpu";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            device_type = "cpu";
            break;
        default:
            break;
    }

    constexpr char hex_digits[] = "0123456789abcdef";
    std::string    device_uuid;
    device_uuid.reserve(VK_UUID_SIZE * 2);
    for (const uint8_t byte : m_device_info.core_properties.core_1_1.deviceUUID) {
        device_uuid.push_back(hex_digits[byte >> 4]);
        device_uuid.push_back(hex_digits[byte & 0x0f]);
    }

    LOG_INFO(
        "[Vulkan][GPU_ENV] schema=1 backend=vulkan "
        "validation_requested={} validation_layer_available={} validation_enabled={} "
        "device_type={} vendor_id=0x{:08x} device_id=0x{:08x} device_uuid={} "
        "requested_api={}.{}.{} device_api={}.{}.{} api_variant={} "
        "device_api_raw=0x{:08x} driver_id={} driver_version_raw=0x{:08x}",
        validation_layer_requested,
        validation_layer_available,
        validation_layer_enabled,
        device_type,
        properties.vendorID,
        properties.deviceID,
        device_uuid,
        VK_API_VERSION_MAJOR(_requested_api_version),
        VK_API_VERSION_MINOR(_requested_api_version),
        VK_API_VERSION_PATCH(_requested_api_version),
        VK_API_VERSION_MAJOR(properties.apiVersion),
        VK_API_VERSION_MINOR(properties.apiVersion),
        VK_API_VERSION_PATCH(properties.apiVersion),
        VK_API_VERSION_VARIANT(properties.apiVersion),
        properties.apiVersion,
        static_cast<uint32>(m_device_info.core_properties.core_1_2.driverID),
        properties.driverVersion
    );
}

void VulkanDevice::CreateDevice() {
    const uint32_t graphics_family =
        m_device_info.queue_family_indices.graphics.value();
    const uint32_t compute_family =
        m_device_info.queue_family_indices.compute.value();
    const bool can_split_same_family_compute =
        compute_family == graphics_family &&
        compute_family < m_device_info.queue_family_props.size() &&
        m_device_info.queue_family_props[compute_family].queueCount >= 2;
    const uint32_t compute_queue_index =
        can_split_same_family_compute ? 1u : 0u;

    std::set<uint32> unique_family_indices = {
        graphics_family,
        m_device_info.queue_family_indices.present.value(),
        compute_family,
        m_device_info.queue_family_indices.transfer.value()
    };

    // setup queue info
    Moer::Array<VkDeviceQueueCreateInfo> queue_create_infos;

    const std::array queue_priorities{1.0f, 1.0f};
    for (const auto& queue_family_index : unique_family_indices) {
        VkDeviceQueueCreateInfo queue_create_info{};
        queue_create_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info.queueFamilyIndex = queue_family_index;
        queue_create_info.queueCount =
            queue_family_index == compute_family ?
                compute_queue_index + 1 :
                1;
        queue_create_info.pQueuePriorities = queue_priorities.data();

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

    VulkanDeviceFeatures enabled_features = m_device_info.core_features;
    enabled_features.AttachToDeviceCreateInfo(device_create_info);
    VK_CHECK_RESULT(vkCreateDevice(m_gpu, &device_create_info, nullptr, &m_device));
    volkLoadDevice(m_device);

    vkGetDeviceQueue(m_device, graphics_family, 0, &m_graphics_queue_handle);

    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.present.value(), 0, &m_present_queue_handle);
    vkGetDeviceQueue(
        m_device,
        compute_family,
        compute_queue_index,
        &m_compute_queue_handle
    );
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.transfer.value(), 0, &m_transfer_queue_handle);
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.raytracing.value(), 0, &m_raytracing_queue_handle);

    m_graphics_command_queue = MakeUnique<VkCommandQueue>(
        *this,
        EQueueType::Graphics,
        rhi_thread_enabled,
        thread_profile_logging,
        parallel_recording,
        parallel_record_workers,
        parallel_record_verify,
        parallel_record_profile,
        parallel_record_min_work_units_per_job,
        parallel_record_worker_throw_trigger
    );
    SetResourceName(uint64(m_graphics_queue_handle), VK_OBJECT_TYPE_QUEUE, "GraphicsQueue");
    m_compute_command_queue = MakeUnique<VkCommandQueue>(*this, EQueueType::Compute, false);
    SetResourceName(uint64(m_compute_queue_handle), VK_OBJECT_TYPE_QUEUE, "ComputeQueue");
    // transfer_queue = MakeUnique<VkCommandQueue>(*this, EQueueType::Copy);
    m_copy_queue = MakeUnique<VkCopyQueue>(*this);
    SetResourceName(uint64(m_transfer_queue_handle), VK_OBJECT_TYPE_QUEUE, "TransferQueue");

    m_graphics_command_queue->SetQueueSubmitMutex(&GetQueueHostMutex(m_graphics_queue_handle));
    m_compute_command_queue->SetQueueSubmitMutex(&GetQueueHostMutex(m_compute_queue_handle));
    m_copy_queue->SetQueueSubmitMutex(&GetQueueHostMutex(m_transfer_queue_handle));

    if (m_graphics_queue_handle == m_transfer_queue_handle) {
        LOG_WARNING(
            "gfx and transfer share the same VkQueue handle. "
            "Using shared host synchronization for queue operations."
        );
    }
    if (m_compute_queue_handle == m_graphics_queue_handle) {
        LOG_WARNING(
            "compute and gfx share the same VkQueue handle. "
            "Using shared host synchronization for queue operations."
        );
    } else if (compute_family == graphics_family) {
        LOG_INFO(
            "compute and gfx use distinct VkQueue handles in family {} "
            "(graphics_index=0 compute_index={}).",
            graphics_family,
            compute_queue_index
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
        m_device_info.memory_properties.memoryTypeCount
    );
    for (uint i = 0; i < handleTypes.size(); i++) {
        if ((m_device_info.memory_properties.memoryTypes[i].propertyFlags &
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
    const auto& ext   = GetOptionalExtensions();
    const auto& props = GetOptionalProperties();
    descriptor_caps   = {
          .heap   = ext.m_has_ext_descriptor_heap,
          .buffer = ext.m_has_ext_descriptor_buffer,
          .push   = HasDeviceExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME),
          .buffer_push =
            ext.m_has_descriptor_buffer_push && props.descriptor_buffer_properties.bufferlessPushDescriptors,
          .max_push_descriptors = props.push_descriptor_properties.maxPushDescriptors,
          .max_push_data        = static_cast<uint32_t>(props.descriptor_heap_properties.maxPushDataSize),
    };
    if (descriptor_caps.heap) {
        auto& fn           = descriptor_heap_functions;
        fn.write_resources = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(
            vkGetDeviceProcAddr(m_device, "vkWriteResourceDescriptorsEXT")
        );
        fn.write_samplers = reinterpret_cast<PFN_vkWriteSamplerDescriptorsEXT>(
            vkGetDeviceProcAddr(m_device, "vkWriteSamplerDescriptorsEXT")
        );
        fn.bind_resources = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(
            vkGetDeviceProcAddr(m_device, "vkCmdBindResourceHeapEXT")
        );
        fn.bind_samplers = reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(
            vkGetDeviceProcAddr(m_device, "vkCmdBindSamplerHeapEXT")
        );
        fn.push_data =
            reinterpret_cast<PFN_vkCmdPushDataEXT>(vkGetDeviceProcAddr(m_device, "vkCmdPushDataEXT"));
        descriptor_caps.heap =
            fn.write_resources && fn.write_samplers && fn.bind_resources && fn.bind_samplers && fn.push_data;
    }
    descriptor_mode = SelectVulkanDescriptorMode(descriptor_caps);
    if (const char* mode = std::getenv("MOER_VULKAN_DESCRIPTORS")) {
        const std::string_view requested(mode);
        if (requested == "sets")
            descriptor_mode = VulkanDescriptorMode::Sets;
        else if (requested == "buffer" && descriptor_caps.buffer)
            descriptor_mode = VulkanDescriptorMode::Buffer;
        else if (requested == "heap" && descriptor_caps.heap)
            descriptor_mode = VulkanDescriptorMode::Heap;
        else if (requested != "auto")
            throw std::runtime_error("Requested Vulkan descriptor backend is unavailable or invalid");
    }
    if (const char* push = std::getenv("MOER_VULKAN_PUSH_DESCRIPTORS");
        push && std::string_view(push) == "0") {
        descriptor_caps.push = false;
    }
    LOG_INFO(
        "Vulkan descriptors: backend={}, push={}, buffer_push={}",
        VulkanDescriptorModeName(descriptor_mode),
        descriptor_caps.push,
        descriptor_caps.buffer_push
    );
    m_global_descriptor_heap.emplace(*this);
    LOG_INFO("VulkanRHI: Descriptor Heap initialized.");
}

void VulkanDevice::CreateInternalShaders() {
    m_internal_shaders = MakeUnique<DeviceInternalShaders>();
    m_internal_shaders->sd_component_shuffle =
        ShaderManager::Get().Compute<ComponentShuffleShader>("core/utils/ShuffleBufferIndices.hlsl");
}

void VulkanDevice::DestroyInternalShaders() {
    m_internal_shaders.reset();
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

        immutable_sampler_infos[i] = sampler_create_info;
        VK_CHECK_RESULT(vkCreateSampler(m_device, &sampler_create_info, VK_NULL_HANDLE, &immutable_samplers[i]));
    }
}

void VulkanDevice::DestroyImmutableSamplers() {
    for (auto& sampler : immutable_samplers) {
        if (sampler) {
            vkDestroySampler(m_device, sampler, VK_NULL_HANDLE);
            sampler = VK_NULL_HANDLE;
        }
    }
}

void VulkanDevice::DestroyDescriptorHeap() {
    m_global_descriptor_heap.reset();
}

void VulkanDevice::Destroy() {
    // for (auto& cmd_allocator : m_command_allocators) {
    //     CHECK_AND_DELETE(cmd_allocator);
    // }
    m_compute_command_queue.reset();
    m_graphics_command_queue.reset();
    m_copy_queue.reset();
    shutdown_sync_completed.store(
        queue_sync_complete_count.load(std::memory_order_acquire) >= 3,
        std::memory_order_release
    );
    LogFaultSummary();
    DestroyInternalShaders();
    FlushDeferredReleases();
    DestroyInternalResources();
    FlushDeferredReleases();
    if (m_allocator) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
    }
    if (m_device) {
        vkDestroyDevice(m_device, VK_NULL_HANDLE);
    }
    m_device = VK_NULL_HANDLE;

    // The callback coalesces non-error messages. Flush before removing the
    // messenger so short-lived GPU gate processes cannot lose a final warning.
    FlushBufferedDebugMessages();
    if (m_debug_utils_messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(
            m_instance, m_debug_utils_messenger, VK_NULL_HANDLE
        );
        m_debug_utils_messenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, VK_NULL_HANDLE);
        m_instance = VK_NULL_HANDLE;
    }
    // The VkInstanceCreateInfo pNext callback is active during instance
    // destruction even after the explicit messenger is gone.
    FlushBufferedDebugMessages();

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

static int32_t FindQueueFamilyIndex(
    std::span<const VkQueueFamilyProperties> _queue_family_props,
    VkQueueFlags                             _required_queue_flags,
    VkQueueFlags                             _exclude_queue_flags
) {
    for (uint32_t i = 0; i < _queue_family_props.size(); ++i) {
        const VkQueueFlags flags = _queue_family_props[i].queueFlags;
        if ((flags & _required_queue_flags) == _required_queue_flags &&
            (flags & _exclude_queue_flags) == 0) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
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

uint32_t VulkanDevice::GetTimestampValidBits(
    EQueueType _queue_type
) const {
    const uint32_t family_index = GetQueueFamilyIndex(_queue_type);
    return family_index < m_device_info.queue_family_props.size() ?
               m_device_info.queue_family_props[family_index].
                   timestampValidBits :
               0u;
}

VkQueueFlags VulkanDevice::GetQueueFamilyFlags(
    EQueueType _queue_type
) const {
    const uint32_t family_index = GetQueueFamilyIndex(_queue_type);
    return family_index < m_device_info.queue_family_props.size() ?
               m_device_info.queue_family_props[family_index].queueFlags :
               VkQueueFlags{0};
}

EVulkanTimestampQueryResetMode
VulkanDevice::GetTimestampQueryResetMode(
    EQueueType _queue_type,
    uint32_t   _effective_valid_bits
) const {
    if (_effective_valid_bits == 0) {
        return EVulkanTimestampQueryResetMode::Unsupported;
    }
    const VkQueueFlags queue_flags =
        GetQueueFamilyFlags(_queue_type);
    if ((queue_flags &
         (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0) {
        return EVulkanTimestampQueryResetMode::CommandBuffer;
    }
    // VulkanDevice passes the queried Vulkan 1.2 feature chain unchanged to
    // vkCreateDevice. A supported hostQueryReset value is therefore also the
    // enabled value used by this native host-reset path.
    if ((queue_flags & VK_QUEUE_TRANSFER_BIT) != 0 &&
        m_device_info.core_features.GetCore12Features().hostQueryReset ==
            VK_TRUE) {
        return EVulkanTimestampQueryResetMode::Host;
    }
    return EVulkanTimestampQueryResetMode::Unsupported;
}

EVulkanTimestampQueryResetMode
VulkanDevice::GetTimestampQueryResetMode(
    EQueueType _queue_type
) const {
    return GetTimestampQueryResetMode(
        _queue_type, GetTimestampValidBits(_queue_type)
    );
}

bool VulkanDevice::SupportsTimestampQueries(
    EQueueType _queue_type
) const {
    return GetTimestampQueryResetMode(_queue_type) !=
           EVulkanTimestampQueryResetMode::Unsupported;
}

QueueFamilyIndices VulkanDevice::SelectQueueFamilyIndices(VkPhysicalDevice _gpu) const {
    QueueFamilyIndices indices;

    auto queue_family_props = GetQueueFamilyProperties(_gpu);
    // AMD GPU的DMA/Transfer队列功能受限，提交包含Barrier/LayoutTransition的命令会导致DeviceLost
    // 所以如果检测到AMD GPU，就强制transfer队列 = graphics队列
    VkPhysicalDeviceProperties gpu_props{};
    vkGetPhysicalDeviceProperties(_gpu, &gpu_props);
    const bool is_amd = (gpu_props.vendorID == 0x1002);

    const int32_t graphics = FindQueueFamilyIndex(queue_family_props, VK_QUEUE_GRAPHICS_BIT, 0);
    if (graphics < 0) {
        return indices;
    }
    indices.graphics   = graphics;
    indices.raytracing = graphics;
    indices.present    = graphics;
    if (is_amd) {
        indices.transfer = graphics;
        LOG_WARNING(
            "AMD GPU '{}' (vendorID={:#x}) detected. "
            "Forcing transfer queue = graphics to avoid DMA queue VK_ERROR_DEVICE_LOST.",
            gpu_props.deviceName,
            gpu_props.vendorID
        );
    } else {
        int32_t transfer = FindQueueFamilyIndex(
            queue_family_props, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT
        );
        if (transfer < 0) {
            transfer = FindQueueFamilyIndex(queue_family_props, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT);
        }
        if (transfer < 0) {
            transfer = graphics;
        }
        indices.transfer = transfer;
    }

    const int32_t compute = FindQueueFamilyIndex(queue_family_props, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
    if (compute >= 0) {
        indices.compute = compute;
    } else {
        indices.compute = graphics;
    }

    return indices;
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
    NotifyVulkanDeviceIdleWait(context);
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
    if (_queue == m_graphics_queue_handle) {
        return m_graphics_queue_mutex;
    }
    if (_queue == m_present_queue_handle) {
        return m_present_queue_mutex;
    }
    if (_queue == m_compute_queue_handle) {
        return m_compute_queue_mutex;
    }
    if (_queue == m_transfer_queue_handle) {
        return m_transfer_queue_mutex;
    }
    if (_queue == m_raytracing_queue_handle) {
        return m_raytracing_queue_mutex;
    }
    assert(false && "Unknown VkQueue handle");
    return m_graphics_queue_mutex;
}

Array<std::mutex*> VulkanDevice::GetUniqueQueueHostMutexes() {
    Array<std::mutex*> mutexes;
    const VkQueue queues[] = {
        m_graphics_queue_handle,
        m_present_queue_handle,
        m_compute_queue_handle,
        m_transfer_queue_handle,
        m_raytracing_queue_handle,
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
    RHIThreadHeartbeat::Get().PulseCurrent(
        ERHIHeartbeatStage::NativeSubmit
    );
    const VkResult result = vkQueueSubmit2(_queue, 1, &_submit_info, _fence);
    RHIThreadHeartbeat::Get().PulseCurrent(
        ERHIHeartbeatStage::Submit
    );
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
    RHIThreadHeartbeat::Get().PulseCurrent(
        ERHIHeartbeatStage::Present
    );
    const VkResult result = vkQueuePresentKHR(_queue, &_present_info);
    RHIThreadHeartbeat::Get().PulseCurrent(
        ERHIHeartbeatStage::Submit
    );
    if (result == VK_SUCCESS) {
        return {};
    }
    if (result == VK_NOT_READY || result == VK_TIMEOUT) {
        return {EVulkanOperationStatus::Retry, result};
    }
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ||
        result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
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

    const VkResult result =
        vkAcquireNextImageKHR(m_device, _swapchain, _timeout, _semaphore, _fence, _image_index);
    if (result == VK_SUCCESS) {
        return {};
    }
    if (result == VK_NOT_READY || result == VK_TIMEOUT) {
        return {EVulkanOperationStatus::Retry, result};
    }
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ||
        result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
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
    if (_result == VK_ERROR_SURFACE_LOST_KHR) {
        return false;
    }
    if (!_force_terminal &&
        (_result == VK_NOT_READY || _result == VK_TIMEOUT || _result == VK_SUBOPTIMAL_KHR ||
         _result == VK_ERROR_OUT_OF_DATE_KHR || _result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)) {
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
    return {EVulkanOperationStatus::Faulted, result};
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

CommandQueue& VulkanDevice::GetCommandQueue(EQueueType _type) {

    switch (_type) {
        case EQueueType::Graphics:
            return *m_graphics_command_queue;
        case EQueueType::Compute:
            return *m_compute_command_queue;
        case EQueueType::Copy:
        default:
            assert(false && "Unknown queue type.");
    }
    return *m_graphics_command_queue;
}

RHIQueueTopology VulkanDevice::GetQueueTopology() const {
    const uint32_t graphics_native_id = 0;
    const uint32_t compute_native_id =
        m_compute_queue_handle == m_graphics_queue_handle ? graphics_native_id : 1u;
    const uint32_t copy_native_id =
        m_transfer_queue_handle == m_graphics_queue_handle ?
            graphics_native_id :
        m_transfer_queue_handle == m_compute_queue_handle ?
            compute_native_id :
            compute_native_id + 1;

    return RHIQueueTopology{
        .graphics = RHIQueueBinding{
            EQueueType::Graphics,
            graphics_native_id,
            m_device_info.queue_family_indices.graphics.value(),
        },
        .compute = RHIQueueBinding{
            EQueueType::Compute,
            compute_native_id,
            m_device_info.queue_family_indices.compute.value(),
        },
        .copy = RHIQueueBinding{
            EQueueType::Copy,
            copy_native_id,
            m_device_info.queue_family_indices.transfer.value(),
        },
    };
}

CopyQueue& VulkanDevice::GetCopyQueue() {
    return *m_copy_queue;
}

TextureRef VulkanDevice::CreateTexture(
    std::string_view  _name,
    const TextureInfo& _info
) {
    TextureInfo info = _info;
    if (!info.debug_name.has_value()) {
        info.debug_name = std::string(_name);
    }
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
    return FenceRef{MoerNew(VulkanTimeline)(*this)};
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

VkSampler VulkanDevice::GetSampler(Sampler _sampler) const {
    return immutable_samplers[GetSamplerIndex(_sampler)];
}

void VulkanDevice::SetResourceName(uint64 _handle, VkObjectType _type, const std::string_view _name) {

    VkDebugUtilsObjectNameInfoEXT name_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    name_info.objectType   = _type;
    name_info.objectHandle = _handle;
    name_info.pObjectName  = _name.data();
    vkSetDebugUtilsObjectNameEXT(m_device, &name_info);
}

RuntimePlugin* VulkanDevice::LoadPlugin(std::string_view _name) {
    auto plugin_it = plugins.find(_name.data());
    if (plugin_it == plugins.end()) {
        return nullptr;
    }
    auto& plugin = plugin_it->second;
    {
        std::lock_guard lock{plugins_mutex};
        if (plugin.instance == nullptr) {
            plugin.instance = plugin.create(this);
        }
    }
    return plugin.instance;
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

void VulkanDevice::RegisterDefaultPlugins() {

#if WITH_NRD
    plugins.try_emplace(
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
}; // namespace Moer::Render
