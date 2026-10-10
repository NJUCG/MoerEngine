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
#include "VulkanSubmissionDiagnostics.h"
#include "VulkanUtil.h"
#include "VulkanVertexFormat.h"
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
#include "rhi/ShaderStageUtils.h"
#include "rhi/ShaderConstantLayout.h"

#include "Core.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/ThreadManager.h"
#include "vulkan/vk_enum_string_helper.h"

// #include <vk_mem_alloc.h>
#include "VulkanMemoryAllocator.h"

#include "VulkanIOService.h"
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
namespace VkUtil = Moer::RHI::Vulkan::Util;

VulkanDevice::VulkanDevice(const VulkanRHIConfig&& _config) : RenderDevice::Impl() {
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

    InitVulkanInstance(_config.api_version);

    m_gpu = SelectGpu(_config.api_version);
    InitGpu(_config.api_version);

    CreateDevice(_config.api_version);
    CreateMemoryAllocator(m_instance, _config.api_version);

    CreateInternalResources();

    LoadDefaultExtensions();
    LogGpuEnvironment(_config.api_version);
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
    const bool can_enable_surface_maintenance1 =
        CanEnableVulkanSurfaceMaintenance1(
            instance_extensions.contains(
                VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME
            ),
            instance_extensions.contains(
                VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME
            )
        );

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
        if (ext == VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME &&
            !can_enable_surface_maintenance1) {
            // VK_EXT_surface_maintenance1 has an explicit instance-extension
            // dependency. Do not enable a capability that the loader cannot
            // legally satisfy on non-Windows platforms.
            continue;
        }
        instance_extensions_loaded.emplace_back(ext.data());
        if (ext == VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) {
            surface_maintenance1_enabled = true;
        }
    }
    CHECK_ASSERT(
        b_instance_extensions_fully_supported, "Not all required instance extensions are supported."
    );

    instance_create_info.enabledExtensionCount   = instance_extensions_loaded.size();
    instance_create_info.ppEnabledExtensionNames = instance_extensions_loaded.data();

    VK_CHECK_RESULT(vkCreateInstance(&instance_create_info, nullptr, &m_instance))
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
    if (!CanEnableVulkanSwapchainMaintenance1(
            surface_maintenance1_enabled,
            gpu_extensions.contains(
                VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
            )
        )) {
        std::erase_if(
            m_device_info.enabled_extensions,
            [](const std::shared_ptr<VulkanDeviceExtension>& extension) {
                return extension &&
                       extension->GetExtensionName() ==
                           VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
            }
        );
    }

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

void VulkanDevice::CreateDevice(uint32 _api_version) {
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

    vkGetDeviceQueue(m_device, graphics_family, 0, &m_graphics_queue);

    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.present.value(), 0, &m_present_queue);
    vkGetDeviceQueue(
        m_device,
        compute_family,
        compute_queue_index,
        &m_compute_queue
    );
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.transfer.value(), 0, &m_transfer_queue);
    vkGetDeviceQueue(m_device, m_device_info.queue_family_indices.raytracing.value(), 0, &m_raytracing_queue);

    gfx_queue = MakeUnique<VkCommandQueue>(
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
        m_device_info.core_features.core_1_2.hostQueryReset ==
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

    NotifyVulkanQueueIdleWait(_context);
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

VkResult VulkanDevice::GetFenceStatus(
    VkFence _fence,
    const VulkanOperationContext& _context
) {
    std::shared_lock<std::shared_mutex> gate(native_queue_gate);
    if (IsFaulted()) {
        gate.unlock();
        return GetFirstFaultResult();
    }

    const VkResult result = vkGetFenceStatus(m_device, _fence);
    if (result == VK_SUCCESS || result == VK_NOT_READY) {
        return result;
    }

    const bool publish_fault = TryBeginFirstFault(result);
    gate.unlock();
    if (publish_fault) {
        std::unique_lock<std::shared_mutex> publish_gate(
            native_queue_gate
        );
        PublishFirstFaultLocked(
            _context, result, false, false
        );
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

namespace {

struct VulkanPipelineBindingInfo {
    UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo> descriptor_set_layouts;
    VkPushConstantRange                                     push_constant_range{};
    Array<ParamInfoFlags>                                   argument_flags;
    UnorderedMap<uint64, uint>                              name_hash_to_argument_index;
    uint64                                                  active_argument_bits    = 0;
};

VkPipelineStageFlags2 ToVulkanPipelineStageFlags(VkShaderStageFlagBits shader_stage) {
    switch (shader_stage) {

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

void MarkShaderArgumentActive(uint argument_id, bool is_active, VulkanPipelineBindingInfo& out_binding_info) {
    if (is_active) {
        out_binding_info.active_argument_bits |= uint64(1) << argument_id;
    }
}

void AccumulateDescriptorBinding(
    uint                                set_id,
    uint                                argument_id,
    const VkDescriptorSetLayoutBinding& binding_desc,
    VulkanPipelineBindingInfo&          out_binding_info
) {
    auto& set_layout            = out_binding_info.descriptor_set_layouts.try_emplace(set_id).first->second;
    auto [binding_it, inserted] = set_layout.bindings.try_emplace(
        binding_desc.binding,
        VulkanDescriptorBindingInfo{
            .vk_binding = binding_desc, .argument_index = static_cast<int>(argument_id)
        }
    );
    if (inserted) {
        return;
    }

    auto& binding_info = binding_it->second;
    if (binding_info.argument_index != static_cast<int>(argument_id)) {
        throw std::invalid_argument(
            "Vulkan descriptor binding conflict at set " + std::to_string(set_id) + ", binding " +
            std::to_string(binding_desc.binding) + ": C++ argument indices " +
            std::to_string(binding_info.argument_index) + " and " + std::to_string(argument_id) + " differ."
        );
    }
    auto& vk_binding = binding_info.vk_binding;
    if (vk_binding.descriptorType != binding_desc.descriptorType ||
        vk_binding.descriptorCount != binding_desc.descriptorCount ||
        vk_binding.pImmutableSamplers != binding_desc.pImmutableSamplers) {
        throw std::invalid_argument(
            "Vulkan descriptor binding conflict at set " + std::to_string(set_id) + ", binding " +
            std::to_string(binding_desc.binding) + ": descriptor descriptions differ."
        );
    }
    // Shared stages extend visibility while retaining the original descriptor and argument source.
    vk_binding.stageFlags |= binding_desc.stageFlags;
}

void AccumulateResourceBinding(
    const ReflectParamInfo::Resource& resource,
    const ShaderArgCppInfo&           argument_info,
    uint                              argument_id,
    VkShaderStageFlagBits             shader_stage,
    VulkanPipelineBindingInfo&        out_binding_info
) {
    const VkDescriptorSetLayoutBinding binding{
        .binding         = resource.binding,
        .descriptorType  = METoVkDescriptorType(resource.desc_type),
        .descriptorCount = std::max(resource.count, argument_info.array_size),
        .stageFlags      = static_cast<VkShaderStageFlags>(shader_stage)
    };
    AccumulateDescriptorBinding(resource.set, argument_id, binding, out_binding_info);

    VulkanShaderResourceState resource_state(resource.desc_type, resource.resource_type, resource.format);
    if (argument_info.type == SDA_Texture) {
        resource_state.b_sampled = resource.sampled;
    }
    out_binding_info.argument_flags[argument_id].state_flags = resource_state();
    out_binding_info.argument_flags[argument_id].pipeline_flags |= ToVulkanPipelineStageFlags(shader_stage);
    MarkShaderArgumentActive(argument_id, resource.custom_flag.active, out_binding_info);
}

void AccumulatePushConstantBinding(
    const ReflectParamInfo::Constant& constant,
    uint                              argument_id,
    VkShaderStageFlagBits             shader_stage,
    VulkanPipelineBindingInfo&        out_binding_info
) {
    if (!constant.custom_flag.active) return;
    out_binding_info.push_constant_range.stageFlags |= shader_stage;
    MarkShaderArgumentActive(argument_id, constant.custom_flag.active, out_binding_info);
}

void AccumulateBindlessDescriptorBinding(
    uint                                set_id,
    uint                                argument_id,
    const VkDescriptorSetLayoutBinding& binding_desc,
    VulkanPipelineBindingInfo&          out_binding_info
) {
    AccumulateDescriptorBinding(set_id, argument_id, binding_desc, out_binding_info);
    out_binding_info.descriptor_set_layouts.at(set_id).is_bindless = true;
}

void AccumulateBindlessBindings(
    const ReflectParamInfo::BindlessArray& resources,
    uint                                   argument_id,
    VkShaderStageFlagBits                  shader_stage,
    VulkanPipelineBindingInfo&             out_binding_info
) {
    // The bindless resource tables are paired with the indirect handle table.
    if (resources.array) {
        constexpr uint k_resource_descriptor_count = 5000;
        constexpr uint k_sampler_descriptor_count  = 256;
        assert(resources.array->binding == 0 && "Indirect Binding Slot Must be 0.");
        AccumulateBindlessDescriptorBinding(
            resources.array->set,
            argument_id,
            {.binding         = 0,
             .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount = 1,
             .stageFlags      = static_cast<VkShaderStageFlags>(shader_stage)},
            out_binding_info
        );
        MarkShaderArgumentActive(argument_id, resources.array->custom_flag.active, out_binding_info);
        if (resources.buffer) {
            // Buffer descriptors share the indirect table's set at binding 1.
            AccumulateBindlessDescriptorBinding(
                resources.array->set,
                argument_id,
                {.binding         = 1,
                 .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .descriptorCount = k_resource_descriptor_count,
                 .stageFlags      = static_cast<VkShaderStageFlags>(shader_stage)},
                out_binding_info
            );
            MarkShaderArgumentActive(argument_id, resources.buffer->custom_flag.active, out_binding_info);
        }
        if (resources.image) {
            AccumulateBindlessDescriptorBinding(
                resources.image->set,
                argument_id,
                {.binding         = 0,
                 .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 .descriptorCount = k_resource_descriptor_count,
                 .stageFlags      = static_cast<VkShaderStageFlags>(shader_stage)},
                out_binding_info
            );
            MarkShaderArgumentActive(argument_id, resources.image->custom_flag.active, out_binding_info);
        }
        if (resources.sampler) {
            AccumulateBindlessDescriptorBinding(
                resources.sampler->set,
                argument_id,
                {.binding         = 0,
                 .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
                 .descriptorCount = k_sampler_descriptor_count,
                 .stageFlags      = static_cast<VkShaderStageFlags>(shader_stage)},
                out_binding_info
            );
            MarkShaderArgumentActive(argument_id, resources.sampler->custom_flag.active, out_binding_info);
        }
    }
    out_binding_info.argument_flags[argument_id].pipeline_flags |= ToVulkanPipelineStageFlags(shader_stage);
}

// Match this shader's reflection to C++ arguments and accumulate the pipeline's layout and binding metadata.
void AccumulateShaderPipelineBindings(
    const SingleShaderInfo&               shader,
    std::span<const PipelineArgumentInfo> arguments,
    VkShaderStageFlagBits                 shader_stage,
    VulkanPipelineBindingInfo&            out_binding_info
) {
    for (uint argument_id = 0; argument_id < arguments.size(); ++argument_id) {
        const auto  argument_name = arguments[argument_id].name;
        const auto& argument_info = arguments[argument_id].cpp_info;
        out_binding_info.name_hash_to_argument_index[GetHash(argument_name)] = argument_id;

        const auto& reflection = shader.shader_param_map->reflect_map;
        const auto  reflection_name =
            argument_info.type == SDA_BindlessArray ? ReflectParamInfo::bdls_name : argument_name;
        const auto reflected_parameter = reflection.find(std::string(reflection_name));
        if (reflected_parameter == reflection.end()) {
            continue;
        }

        const auto& parameter = reflected_parameter->second.spirv;
        switch (argument_info.type) {
            case SDA_BindlessArray:
                AccumulateBindlessBindings(parameter.bindless, argument_id, shader_stage, out_binding_info);
                break;
            case SDA_Constant:
                AccumulatePushConstantBinding(
                    std::get<ReflectParamInfo::Constant>(parameter.resources.data),
                    argument_id,
                    shader_stage,
                    out_binding_info
                );
                break;
            case SDA_Buffer:
            case SDA_Texture:
            case SDA_TLAS:
            case SDA_Sampler:
                AccumulateResourceBinding(
                    std::get<ReflectParamInfo::Resource>(parameter.resources.data),
                    argument_info,
                    argument_id,
                    shader_stage,
                    out_binding_info
                );
                break;
            default:
                assert(false && "Unknown shader arg type.");
        }
    }
}

VulkanPipelineBindingInfo BuildVulkanPipelineBindingInfo(
    std::span<const SingleShaderInfo* const> shaders,
    std::span<const PipelineArgumentInfo>    arguments
) {
    VulkanPipelineBindingInfo binding_info;
    binding_info.argument_flags.resize(arguments.size());
    for (const auto* shader : shaders) {
        const auto shader_stage = static_cast<VkShaderStageFlagBits>(
            VulkanEnumTranslator::METoVKShaderStageFlags(shader->shader_type)
        );
        AccumulateShaderPipelineBindings(*shader, arguments, shader_stage, binding_info);
    }
    return binding_info;
}

Array<VkPipelineShaderStageCreateInfo>
CreateVulkanShaderStages(VkDevice device, std::span<const SingleShaderInfo* const> shaders) {
    Array<VkPipelineShaderStageCreateInfo> stages;
    stages.reserve(shaders.size());
    for (const auto* shader : shaders) {
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = shader->shader_data.size();
        module_info.pCode    = reinterpret_cast<const uint32_t*>(shader->shader_data.data());
        VkShaderModule module;
        VK_CHECK_RESULT(vkCreateShaderModule(device, &module_info, nullptr, &module));
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = static_cast<VkShaderStageFlagBits>(
            VulkanEnumTranslator::METoVKShaderStageFlags(shader->shader_type)
        );
        stage.module = module;
        stage.pName  = shader->entry_point.data();
        stages.push_back(stage);
    }
    return stages;
}

void FillMissingDescriptorBindings(UnorderedMap<uint, VulkanDescriptorSetLayoutCreateInfo>& out_set_layouts) {
    for (auto& [set_id, set_layout] : out_set_layouts) {
        uint max_binding = 0;
        for (const auto& [binding_id, binding] : set_layout.bindings) {
            max_binding = std::max(max_binding, binding_id);
        }
        for (uint binding_id = 0; binding_id <= max_binding; ++binding_id) {
            set_layout.bindings.try_emplace(
                binding_id,
                VulkanDescriptorBindingInfo{
                    .vk_binding = {.binding = binding_id, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER}
                }
            );
        }
    }
}

void InitializeVulkanPipelineLayout(VulkanPipelineState& pipeline, VulkanPipelineBindingInfo& binding_info) {
    FillMissingDescriptorBindings(binding_info.descriptor_set_layouts);
    pipeline.InitPipelineLayout(
        std::move(binding_info.descriptor_set_layouts), binding_info.push_constant_range
    );
}

bool ValidateGraphicsPipelineCreateInfo(
    const VulkanDevice&       device,
    const VulkanDeviceInfo&   device_info,
    const GfxPsoCreateInfo&   create_info,
    const PipelineShaderInfo& shader_info
) {
    if (const auto error = ValidateGraphicsShaderStages(shader_info.shaders); !error.empty()) {
        LOG_ERROR("Cannot create Vulkan graphics pipeline: {}", error);
        return false;
    }
    const uint32_t required_view_count = Multiview::RequiredViewCount(create_info.view_mask);
    if (create_info.view_mask != 0 && !device.SupportsMultiview(required_view_count)) {
        LOG_ERROR(
            "Cannot create multiview graphics pipeline: view_mask=0x{:x} requires {} views, "
            "but the selected device supports at most {} (feature enabled={}).",
            create_info.view_mask,
            required_view_count,
            device_info.core_properties.core_1_1.maxMultiviewViewCount,
            device_info.core_features.core_1_1.multiview == VK_TRUE
        );
        return false;
    }

    const bool uses_tessellation = FindShaderStage(shader_info.shaders, ST_HULL) != nullptr;
    if (uses_tessellation) {
        const uint32_t patch_control_points = create_info.patch_control_points;
        const uint32_t max_patch_size = device_info.core_properties.core_1_0.limits.maxTessellationPatchSize;
        if (!device.SupportsTessellation()) {
            LOG_ERROR("Cannot create tessellation pipeline: the Vulkan device does not support tessellation "
                      "shaders.");
            return false;
        }
        if (create_info.primitive_topology != EPrimitiveTopology::PATCH_LIST) {
            LOG_ERROR("Cannot create tessellation pipeline: primitive topology must be PATCH_LIST.");
            return false;
        }
        if (patch_control_points == 0 || patch_control_points > max_patch_size) {
            LOG_ERROR(
                "Cannot create tessellation pipeline: patch control point count {} is outside [1, {}].",
                patch_control_points,
                max_patch_size
            );
            return false;
        }
    } else if (create_info.primitive_topology == EPrimitiveTopology::PATCH_LIST) {
        LOG_ERROR("Cannot create PATCH_LIST pipeline without Hull and Domain shaders.");
        return false;
    }
    return true;
}

VkPipelineColorBlendAttachmentState ToVulkanBlendAttachment(const RHIBlendAttachmentInfo& info) {
    VkPipelineColorBlendAttachmentState state{};
    state.blendEnable         = (info.color_blend_op != BO_ADD || info.color_dst_blend_factor != BF_ZERO ||
                         info.color_src_blend_factor != BF_ONE || info.alpha_blend_op != BO_ADD ||
                         info.alpha_dst_blend_factor != BF_ZERO || info.alpha_src_blend_factor != BF_ONE) ?
                                    VK_TRUE :
                                    VK_FALSE;
    state.srcColorBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(info.color_src_blend_factor);
    state.dstColorBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(info.color_dst_blend_factor);
    state.colorBlendOp        = VulkanEnumTranslator::METoVKBlendOp(info.color_blend_op);
    state.srcAlphaBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(info.alpha_src_blend_factor);
    state.dstAlphaBlendFactor = VulkanEnumTranslator::METoVKBlendFactor(info.alpha_dst_blend_factor);
    state.alphaBlendOp        = VulkanEnumTranslator::METoVKBlendOp(info.alpha_blend_op);
    state.colorWriteMask      = (info.color_write_mask & CW_RED) ? VK_COLOR_COMPONENT_R_BIT : 0;
    state.colorWriteMask |= (info.color_write_mask & CW_GREEN) ? VK_COLOR_COMPONENT_G_BIT : 0;
    state.colorWriteMask |= (info.color_write_mask & CW_BLUE) ? VK_COLOR_COMPONENT_B_BIT : 0;
    state.colorWriteMask |= (info.color_write_mask & CW_ALPHA) ? VK_COLOR_COMPONENT_A_BIT : 0;
    return state;
}

VkPipelineRasterizationStateCreateInfo ToVulkanRasterizationState(const RHIRasterizeInfo& info) {
    VkPipelineRasterizationStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    state.depthClampEnable        = info.b_depth_clamp_enable ? VK_TRUE : VK_FALSE;
    state.rasterizerDiscardEnable = VK_FALSE;
    state.polygonMode             = VulkanEnumTranslator::METoVKPolygonMode(info.fill_mode);
    state.cullMode                = VulkanEnumTranslator::METoVKCullModeFlags(info.cull_mode);
    state.frontFace =
        info.b_front_counter_clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    state.depthBiasEnable         = info.b_depth_bias ? VK_TRUE : VK_FALSE;
    state.depthBiasConstantFactor = info.depth_bias;
    state.depthBiasClamp          = info.depth_bias_clamp;
    state.depthBiasSlopeFactor    = info.depth_bias_slop_factor;
    state.lineWidth               = 1.0f;
    return state;
}

VkPipelineMultisampleStateCreateInfo ToVulkanMultisampleState(const RHIMultisampleStateInfo& info) {
    VkPipelineMultisampleStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    state.rasterizationSamples  = VulkanEnumTranslator::METoVKSampleCountFlagBits(info.sample_count);
    state.sampleShadingEnable   = VK_FALSE;
    state.minSampleShading      = 1.0f;
    state.pSampleMask           = nullptr;
    state.alphaToCoverageEnable = VK_FALSE;
    state.alphaToOneEnable      = VK_FALSE;
    return state;
}

VkPipelineDepthStencilStateCreateInfo ToVulkanDepthStencilState(const RHIDepthStencilStateInfo& info) {
    VkPipelineDepthStencilStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    state.depthTestEnable =
        (info.b_enable_depth_write || info.depth_test_op != ECompareOption::CO_NEVER) ? VK_TRUE : VK_FALSE;
    state.depthWriteEnable      = info.b_enable_depth_write;
    state.depthCompareOp        = VulkanEnumTranslator::METoVKCompareOp(info.depth_test_op);
    state.depthBoundsTestEnable = VK_FALSE;
    state.minDepthBounds        = 0.0f;
    state.maxDepthBounds        = 1.0f;

    state.stencilTestEnable =
        (info.b_enable_front_face_stencil || info.b_enable_back_face_stencil) ? VK_TRUE : VK_FALSE;

    if (info.b_enable_front_face_stencil) {
        state.front.failOp = VulkanEnumTranslator::METoVKStencilOp(info.front_face_stencil_fail_stencil_op);
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
        state.back.depthFailOp = VulkanEnumTranslator::METoVKStencilOp(info.back_face_depth_fail_stencil_op);
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
    return state;
}

// Owns every array referenced by the Vulkan state structures until pipeline creation completes.
struct VulkanGraphicsPipelineState {
    Array<VkFormat>                            color_attachment_formats;
    Array<VkPipelineColorBlendAttachmentState> color_blend_attachments;
    Array<VkVertexInputBindingDescription>     vertex_bindings;
    Array<VkVertexInputAttributeDescription>   vertex_attributes;
    StaticArray<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineRenderingCreateInfo        rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    VkPipelineViewportStateCreateInfo      viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    VkPipelineMultisampleStateCreateInfo  multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    VkPipelineTessellationStateCreateInfo tessellation{
        VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO
    };
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo    dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    bool                                uses_stencil_attachment = false;

    VulkanGraphicsPipelineState() = default;
    // Internal pointers refer to this object's storage, so its address must remain stable.
    VulkanGraphicsPipelineState(const VulkanGraphicsPipelineState&)            = delete;
    VulkanGraphicsPipelineState& operator=(const VulkanGraphicsPipelineState&) = delete;
    VulkanGraphicsPipelineState(VulkanGraphicsPipelineState&&)                 = delete;
    VulkanGraphicsPipelineState& operator=(VulkanGraphicsPipelineState&&)      = delete;
};

void InitializeGraphicsAttachmentState(
    const GfxPsoCreateInfo&      create_info,
    VulkanGraphicsPipelineState& states
) {
    const uint32_t attachment_count = create_info.color_attachment_count;
    states.color_attachment_formats.resize(attachment_count);
    states.color_blend_attachments.resize(attachment_count);
    for (uint32_t index = 0; index < attachment_count; ++index) {
        const auto& attachment                 = create_info.color_attachments_info[index];
        states.color_attachment_formats[index] = VulkanEnumTranslator::METoVKFormat(attachment.pixel_format);
        states.color_blend_attachments[index]  = ToVulkanBlendAttachment(attachment.blend_state_info);
    }

    states.rendering.viewMask                = create_info.view_mask;
    states.rendering.colorAttachmentCount    = attachment_count;
    states.rendering.pColorAttachmentFormats = states.color_attachment_formats.data();
    states.rendering.depthAttachmentFormat =
        VulkanEnumTranslator::METoVKFormat(create_info.depth_stencil_format);

    // Only declare a stencil attachment when the PSO actually enables stencil tests.
    const bool depth_has_stencil = create_info.depth_stencil_format == PF_D32_SFLOAT_S8_UINT ||
                                   create_info.depth_stencil_format == PF_D24_UNORM_S8_UINT ||
                                   create_info.depth_stencil_format == PF_D16_UNORM_S8_UINT ||
                                   create_info.depth_stencil_format == PF_S8_UINT;
    const bool stencil_enabled = create_info.depth_stencil_info.b_enable_front_face_stencil ||
                                 create_info.depth_stencil_info.b_enable_back_face_stencil;
    states.uses_stencil_attachment = depth_has_stencil && stencil_enabled;
    states.rendering.stencilAttachmentFormat =
        states.uses_stencil_attachment ? states.rendering.depthAttachmentFormat : VK_FORMAT_UNDEFINED;

    states.color_blend.logicOp         = VK_LOGIC_OP_COPY;
    states.color_blend.logicOpEnable   = VK_FALSE;
    states.color_blend.attachmentCount = attachment_count;
    states.color_blend.pAttachments    = states.color_blend_attachments.data();
}

void InitializeGraphicsVertexInputState(
    const GfxPsoCreateInfo&      create_info,
    VulkanGraphicsPipelineState& states
) {
    uint binding_index = 0;
    states.vertex_bindings.reserve(create_info.vertex_stream.bindings.size());
    uint attribute_count = 0;
    for (const auto& binding : create_info.vertex_stream.bindings) {
        attribute_count += binding.vertex_elements.size();
    }
    uint attribute_location = 0;
    states.vertex_attributes.reserve(attribute_count);
    for (const VertexBinding& binding : create_info.vertex_stream.bindings) {
        uint binding_stride   = 0;
        uint attribute_offset = 0;
        for (const VertexElement& attribute : binding.vertex_elements) {
            const VkFormat format = ToVulkanVertexFormat(attribute.format);
            if (format == VK_FORMAT_UNDEFINED) {
                throw std::invalid_argument("Invalid Vulkan vertex attribute format");
            }
            states.vertex_attributes.emplace_back(
                attribute_location++, binding_index, format, attribute_offset
            );
            attribute_offset += GetVertexFormatByteSize(attribute.format);
            binding_stride = attribute_offset;
        }
        states.vertex_bindings.emplace_back(
            binding_index, binding_stride, VulkanEnumTranslator::METoVKVertexInputRate(binding.input_rate)
        );
        ++binding_index;
    }
    states.vertex_input.vertexBindingDescriptionCount   = uint(states.vertex_bindings.size());
    states.vertex_input.pVertexBindingDescriptions      = states.vertex_bindings.data();
    states.vertex_input.vertexAttributeDescriptionCount = uint(states.vertex_attributes.size());
    states.vertex_input.pVertexAttributeDescriptions    = states.vertex_attributes.data();
}

void InitializeGraphicsPipelineState(
    const GfxPsoCreateInfo&      create_info,
    VulkanGraphicsPipelineState& states
) {
    InitializeGraphicsAttachmentState(create_info, states);
    InitializeGraphicsVertexInputState(create_info, states);

    states.input_assembly.topology =
        VulkanEnumTranslator::METoVKPrimitiveTopology(create_info.primitive_topology);
    states.input_assembly.primitiveRestartEnable = VK_FALSE;
    states.viewport.viewportCount                = create_info.multi_view_count;
    states.viewport.scissorCount                 = create_info.multi_view_count;
    states.rasterization                         = ToVulkanRasterizationState(create_info.rasterizer_info);
    states.multisample                           = ToVulkanMultisampleState(create_info.multisample_info);
    states.tessellation.patchControlPoints       = create_info.patch_control_points;
    states.depth_stencil                         = ToVulkanDepthStencilState(create_info.depth_stencil_info);
    states.dynamic.dynamicStateCount             = states.dynamic_states.size();
    states.dynamic.pDynamicStates                = states.dynamic_states.data();
}

VkGraphicsPipelineCreateInfo MakeGraphicsPipelineCreateInfo(
    const VulkanGraphicsPipelineState&               states,
    std::span<const VkPipelineShaderStageCreateInfo> shader_stages,
    VkPipelineLayout                                 layout,
    bool                                             uses_tessellation
) {
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext               = &states.rendering;
    info.flags               = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    info.stageCount          = static_cast<uint32_t>(shader_stages.size());
    info.pStages             = shader_stages.data();
    info.pVertexInputState   = &states.vertex_input;
    info.pInputAssemblyState = &states.input_assembly;
    info.pTessellationState  = uses_tessellation ? &states.tessellation : nullptr;
    info.pViewportState      = &states.viewport;
    info.pRasterizationState = &states.rasterization;
    info.pMultisampleState   = &states.multisample;
    info.pDepthStencilState  = &states.depth_stencil;
    info.pColorBlendState    = &states.color_blend;
    info.pDynamicState       = &states.dynamic;
    info.layout              = layout;
    info.basePipelineIndex   = -1;
    return info;
}

} // namespace

PipelineHandle
VulkanDevice::CreatePipeline(GfxPsoCreateInfo&& _create_info, PipelineShaderInfo&& _shader_info) {
    const uint constant_byte_size = ValidatePipelineConstants(
        _shader_info, m_device_info.core_properties.core_1_0.limits.maxPushConstantsSize
    );
    if (!ValidateGraphicsPipelineCreateInfo(*this, m_device_info, _create_info, _shader_info)) {
        return {};
    }

    const auto ordered_shaders = GetGraphicsShadersInStageOrder(_shader_info.shaders);
    auto       binding_info =
        BuildVulkanPipelineBindingInfo(ordered_shaders, _shader_info.argument_metadata.GetArguments());
    binding_info.push_constant_range.size = constant_byte_size;

    auto*                       vk_pso = MoerNew(VulkanPipelineState)(this, VulkanPipelineState::GFX);
    VulkanGraphicsPipelineState states;
    InitializeGraphicsPipelineState(_create_info, states);
    vk_pso->b_uses_stencil_attachment = states.uses_stencil_attachment;
    vk_pso->view_mask                 = _create_info.view_mask;

    auto shader_stages = CreateVulkanShaderStages(m_device, ordered_shaders);
    InitializeVulkanPipelineLayout(*vk_pso, binding_info);
    const bool uses_tessellation = FindShaderStage(_shader_info.shaders, ST_HULL) != nullptr;
    const auto pipeline_create_info =
        MakeGraphicsPipelineCreateInfo(states, shader_stages, vk_pso->GetPipelineLayout(), uses_tessellation);
    VK_CHECK_RESULT(vkCreateGraphicsPipelines(
        m_device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline
    ));

    for (const auto& shader_stage : shader_stages) {
        vkDestroyShaderModule(m_device, shader_stage.module, nullptr);
    }
    return PipelineHandle{
        .handle            = reinterpret_cast<uint64>(vk_pso),
        .binding_infos     = std::move(binding_info.argument_flags),
        .hash_2_info_index = std::move(binding_info.name_hash_to_argument_index),
        .valid_bits        = binding_info.active_argument_bits,
    };
}

PipelineHandle VulkanDevice::CreatePipeline(PipelineShaderInfo&& shader_info) {
    const uint constant_byte_size = ValidatePipelineConstants(
        shader_info, m_device_info.core_properties.core_1_0.limits.maxPushConstantsSize);
    if (const auto error = ValidateComputeShaderStages(shader_info.shaders); !error.empty()) {
        LOG_ERROR("Cannot create Vulkan compute pipeline: {}", error);
        return {};
    }
    const SingleShaderInfo* shader = &shader_info.shaders.front();
    const std::span<const SingleShaderInfo* const> stages(&shader, 1);
    auto binding_info = BuildVulkanPipelineBindingInfo(stages, shader_info.argument_metadata.GetArguments());
    binding_info.push_constant_range.size = constant_byte_size;
    auto* vk_pso = MoerNew(VulkanPipelineState)(this, VulkanPipelineState::Compute);
    auto  shader_stages = CreateVulkanShaderStages(m_device, stages);
    InitializeVulkanPipelineLayout(*vk_pso, binding_info);

    VkComputePipelineCreateInfo pipeline_create_info{};
    pipeline_create_info.sType             = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_create_info.flags             = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    pipeline_create_info.stage             = shader_stages.front();
    pipeline_create_info.layout            = vk_pso->GetPipelineLayout();
    pipeline_create_info.basePipelineIndex = -1;
    VK_CHECK_RESULT(vkCreateComputePipelines(
        m_device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline
    ));

    vkDestroyShaderModule(m_device, shader_stages.front().module, nullptr);
    return PipelineHandle{
        .handle            = reinterpret_cast<uint64>(vk_pso),
        .binding_infos     = std::move(binding_info.argument_flags),
        .hash_2_info_index = std::move(binding_info.name_hash_to_argument_index),
        .valid_bits        = binding_info.active_argument_bits,
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

RHIQueueTopology VulkanDevice::GetQueueTopology() const {
    const uint32_t graphics_native_id = 0;
    const uint32_t compute_native_id =
        m_compute_queue == m_graphics_queue ? graphics_native_id : 1u;
    const uint32_t copy_native_id =
        m_transfer_queue == m_graphics_queue ?
            graphics_native_id :
        m_transfer_queue == m_compute_queue ?
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
    return *copy_queue;
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
}; // namespace Moer::Render
