//
// Created by 74535 on 2023/10/2.
//

#include "VulkanRHIResource.h"
#include "log/LogSystem.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "VulkanDescriptor.h"
#include "VulkanExtension.h"
#include "VulkanDevice.h"
#include "VulkanUtil.h"
#include "VulkanCommand.h"
#include "Core.h"
#include "taskgraph/ThreadManager.h"
#include "vk_mem_alloc.h"
#include "vulkan/vulkan_core.h"
#include <algorithm>
#include <shared_mutex>
#include <spirv_reflect.h>
#include <variant>
#include <config.h>

namespace Moer::Render {
    namespace VkUtil = Moer::RHI::Vulkan::Util;

    std::tuple<SpvReflectResourceType, SpvReflectDescriptorType> DecodeReflectType(uint32 _value) { return std::make_tuple(SpvReflectResourceType(_value), SpvReflectDescriptorType(_value >> 4)); }

    std::tuple<uint32, uint32> DecodeConstant(uint32 _flag) { return std::make_tuple(_flag >> 20, (_flag & 0xFFFFF)); }
    VulkanDevice::~VulkanDevice() { Destroy(); }

    void VulkanDevice::Init(const DeviceInitializer& _initializer) {
        m_gpu = SelectGpu(_initializer);
        if (m_gpu == VK_NULL_HANDLE) { VkUtil::ExitFatal("No available GPU found.", -1); }

        InitGpu(_initializer);

        LOG_INFO("\n- DeviceName: {}."
                 "\n- API={}.{}.{} (0x{:x}) Driver=0x{:x} VendorId=0x{:x}."
                 "\n- DeviceID=0x{:x} Type={}."
                 "\n- Max Descriptor Sets Bound {}, Timestamps {}.",
                 m_core_properties.core_1_0.deviceName,
                 VK_API_VERSION_MAJOR(m_core_properties.core_1_0.apiVersion),
                 VK_API_VERSION_MINOR(m_core_properties.core_1_0.apiVersion),
                 VK_API_VERSION_PATCH(m_core_properties.core_1_0.apiVersion),
                 m_core_properties.core_1_0.apiVersion,
                 m_core_properties.core_1_0.driverVersion,
                 m_core_properties.core_1_0.vendorID,
                 m_core_properties.core_1_0.deviceID,
                 std::to_string(m_core_properties.core_1_0.deviceType),
                 m_core_properties.core_1_0.limits.maxBoundDescriptorSets,
                 m_core_properties.core_1_0.limits.timestampComputeAndGraphics);

        CreateDevice(_initializer.api_version);
        CreateDescriptorAllocator();

        VkSamplerCreateInfo sampler_create_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_create_info.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        sampler_create_info.unnormalizedCoordinates = VK_FALSE;
        sampler_create_info.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_create_info.mipLodBias              = 0.0f;
        sampler_create_info.minLod                  = 0.0f;
        sampler_create_info.maxLod                  = 0.0f;
        for (uint i = 0; i < immutable_sampler_count; ++i) {
            ESamplerFilter          filter           = ESamplerFilter(i % SF_Num);
            ESamplerAddressMode     address_mode     = ESamplerAddressMode((i / SF_Num) % SAM_Num);
            ESamplerCompareFunction compare_function = ESamplerCompareFunction(i / (SAM_Num * uint(SF_Num)));

            sampler_create_info.minFilter    = sampler_create_info.magFilter    = VulkanEnumTranslator::METoVKMinMagFilterMode(filter);
            sampler_create_info.addressModeU = sampler_create_info.addressModeV = sampler_create_info.addressModeW = VulkanEnumTranslator::METoVKWrapMode(address_mode);

            sampler_create_info.compareOp     = VulkanEnumTranslator::METoVKCompareOp(ECompareOption(compare_function));
            sampler_create_info.compareEnable = compare_function != SCF_NEVER;

            vkCreateSampler(m_device, &sampler_create_info, VK_NULL_HANDLE, &immutable_samplers[i]);
        }
        InitMemoryAllocator(m_instance);
        new(&m_global_descriptor_heap) VulkanDescriptorHeap(*this);
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
        VkDebugUtilsMessageTypeFlagsEXT             message_type,
        const VkDebugUtilsMessengerCallbackDataEXT* p_callback_data,
        void*                                       p_user_data) {

        std::stringstream stream;
        stream << "[" << p_callback_data->messageIdNumber << "][" << p_callback_data->pMessageIdName << "]: " << p_callback_data->pMessage << std::endl;

        if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) { LOG_DEBUG(stream.str()); } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) { LOG_INFO(stream.str()); } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) { LOG_WARNING(stream.str()); } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { LOG_ERROR(stream.str()); }

        return VK_FALSE;
    }

    void VulkanDevice::CreateInstance() {
        VkApplicationInfo application_info{};
        application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        // application_info.pApplicationName = MACRO_STR(__ENGINE_NAME__);
        // application_info.applicationVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
        application_info.pEngineName   = MACRO_STR(__ENGINE_NAME__);
        application_info.engineVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
        application_info.apiVersion    = VK_API_VERSION_1_3;

        m_instance_extensions         = VulkanInstanceExtension::GetDriverSupportedInstanceExtensionNames();
        m_enabled_instance_extensions = VulkanInstanceExtension::GetMESupportedInstanceExtensions();

        bool extension_supported = [&]() {
            if (!m_enabled_instance_extensions.empty()) {
                for (const auto& extension : m_enabled_instance_extensions) {
                    if (std::find(m_instance_extensions.begin(), m_instance_extensions.end(), extension) == m_instance_extensions.end()) {
                        VkUtil::ExitFatal("Enabled instance extension '" + std::string(extension) + "' is not supported!", -1);
                        return false;
                    }
                }
            }
            return true;
        }();
        bool debug_utils_available = std::find(m_enabled_instance_extensions.begin(), m_enabled_instance_extensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != m_enabled_instance_extensions.end();

        VkInstanceCreateInfo instance_create_info{};
        instance_create_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_create_info.pNext            = nullptr;
        instance_create_info.flags            = 0;
        instance_create_info.pApplicationInfo = &application_info;

        auto n = m_enabled_instance_extensions.size();

        Moer::Array<const char*> r_extensions(n, nullptr);
        for (size_t i = 0; i < n; ++i) { r_extensions[i] = m_enabled_instance_extensions[i].c_str(); }
        instance_create_info.enabledExtensionCount   = n;
        instance_create_info.ppEnabledExtensionNames = r_extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};

        const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";

        if ([&](std::string_view _view) {
            uint32_t instance_layer_count = 0;
            vkEnumerateInstanceLayerProperties(&instance_layer_count, nullptr);
            Moer::Array<VkLayerProperties> instance_layer_properties(instance_layer_count);
            vkEnumerateInstanceLayerProperties(&instance_layer_count, instance_layer_properties.data());
            bool validation_layer_present = false;

            for (auto layer_property : instance_layer_properties) {
                if (_view.data() == layer_property.layerName) {
                    validation_layer_present = true;
                    break;
                }
            }
            return validation_layer_present;
        }(validation_layer_name)) {
            instance_create_info.enabledLayerCount   = 1;
            instance_create_info.ppEnabledLayerNames = &validation_layer_name;
            PopulateDebugMessengerCreateInfo(debug_create_info);
            instance_create_info.pNext = &debug_create_info;
        } else {
            instance_create_info.enabledLayerCount   = 0;
            instance_create_info.ppEnabledLayerNames = nullptr;
        }

        VK_CHECK_RESULT(vkCreateInstance(&instance_create_info, nullptr, &m_instance))

        SetupDebugUtilsMessengerEXT();

        if (debug_utils_available) { SetupDebugProcs(); }
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
        if (m_optional_extensions.m_has_memory_priority) { alloc_create_info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT; }

        VK_CHECK_RESULT(vmaCreateAllocator(&alloc_create_info, &m_allocator));

        LOG_INFO("Vulkan Memory Allocator initialized with api version: {}.", alloc_create_info.vulkanApiVersion);
    }

    void VulkanDevice::Destroy() {
        // for (auto& cmd_allocator : m_command_allocators) {
        //     CHECK_AND_DELETE(cmd_allocator);
        // }
        compute_queue.reset();
        transfer_queue.reset();
        gfx_queue.reset();
        m_command_allocators.clear();
        CHECK_AND_DELETE(m_descriptor_allocator);
        FlushDeferredReleases();
        vmaDestroyAllocator(m_allocator);
        // Assertion failed: m_pMetadata->IsEmpty() && "Some allocations were not freed before destruction of this memory block!"
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
        Moer::Array<VkPhysicalDevice> gpu_list(gpu_count);
        VK_CHECK_RESULT(vkEnumeratePhysicalDevices(_init.instance, &gpu_count, gpu_list.data()))

        for (const auto& gpu : gpu_list) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(gpu, &props);
            bool type_matched            = props.deviceType == _init.gpu_type;
            bool extensions_supported    = CheckEnabledExtensionsSupported(gpu, _init.enabled_extensions);
            bool core_features_supported = CheckEnabledFeaturesSupported(gpu, _init.enabled_features, _init.api_version);
            // bool swap_chain_adequate     = false;
            // if (extensions_supported) {
            //     auto details        = VkUtil::QuerySwapChainSupport(gpu, _init.surface);
            //     swap_chain_adequate = !details.formats.empty() && !details.present_modes.empty();
            // }

            auto indices = QueryQueueFamilyIndices(gpu);

            if (indices.IsComplete() && core_features_supported && extensions_supported && type_matched) { return gpu; }
        }

        LOG_ERROR("No available target type (discrete, etc.) GPU found!");

        return VK_NULL_HANDLE;
    }

    void VulkanDevice::InitGpu(const DeviceInitializer& _initializer) {
        // Query core features
        m_core_features.Query(m_gpu, _initializer.api_version);
        // Query advanced features, use advanced features as GPU supported, and developers cannot specify them.
        {
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            for (const auto& extension : _initializer.enabled_extensions) { extension->PreGpuFeatures(features2); }
            vkGetPhysicalDeviceFeatures2(m_gpu, &features2);
            for (const auto& extension : _initializer.enabled_extensions) { extension->PostGpuFeatures(m_optional_extensions); }
        }

        // Query core properties
        m_core_properties.Query(m_gpu, _initializer.api_version);
        // Query advanced properties.
        {
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            for (const auto& extension : _initializer.enabled_extensions) { extension->PreGpuProperties(this, props2); }
            vkGetPhysicalDeviceProperties2(m_gpu, &props2);
        }

        // Query supported extensions
        m_enabled_extensions.Init(_initializer.enabled_extensions);

        // Query core memory properties
        vkGetPhysicalDeviceMemoryProperties(m_gpu, &m_memery_properties);

        // Query queue family info
        m_queue_family_indices = QueryQueueFamilyIndices(m_gpu);
        m_queue_family_props   = GetQueueFamilyProperties(m_gpu);

        LOG_INFO("VulkanRHI: GPU initialized.");
    }

    void VulkanDevice::CreateDevice(uint32_t _api_version) {
        std::set<uint32_t> unique_family_indices = {m_queue_family_indices.graphics.value(), m_queue_family_indices.present.value(), m_queue_family_indices.compute.value(), m_queue_family_indices.transfer.value()};

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
        device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
        device_create_info.pQueueCreateInfos    = queue_create_infos.data();

        // setup extension and feature info
        Moer::Array<const char*> enabled_extensions;
        for (const auto& extension : m_enabled_extensions.m_enabled_extensions) {
            enabled_extensions.emplace_back(extension->GetExtensionName().data());
            extension->PreCreateDevice(device_create_info);
        }

        if (!enabled_extensions.empty()) {
            device_create_info.enabledExtensionCount   = static_cast<uint32_t>(enabled_extensions.size());
            device_create_info.ppEnabledExtensionNames = enabled_extensions.data();
        }
        VkPhysicalDeviceFeatures2 enabled_features;

        if (_api_version > VK_API_VERSION_1_0) {
            enabled_features.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            enabled_features.features = m_core_features.core_1_0;
            enabled_features.pNext    = &m_core_features.core_1_1;
            m_core_features.PreCreateDevice(device_create_info, _api_version);
            device_create_info.pNext = &enabled_features;
        } else { device_create_info.pEnabledFeatures = &m_core_features.core_1_0; }
        VK_CHECK_RESULT(vkCreateDevice(m_gpu, &device_create_info, nullptr, &m_device));

        vkGetDeviceQueue(m_device, m_queue_family_indices.graphics.value(), 0, &m_graphics_queue);
        vkGetDeviceQueue(m_device, m_queue_family_indices.present.value(), 0, &m_present_queue);
        vkGetDeviceQueue(m_device, m_queue_family_indices.compute.value(), 0, &m_compute_queue);
        vkGetDeviceQueue(m_device, m_queue_family_indices.transfer.value(), 0, &m_transfer_queue);
        vkGetDeviceQueue(m_device, m_queue_family_indices.raytracing.value(), 0, &m_raytracing_queue);
        gfx_queue      = MakeUnique<VkCommandQueue>(*this, EQueueType::Graphics);
        compute_queue  = MakeUnique<VkCommandQueue>(*this, EQueueType::Compute);
        transfer_queue = MakeUnique<VkCommandQueue>(*this, EQueueType::Copy);
    }

    void VulkanDevice::CreateDescriptorAllocator() {
        m_descriptor_allocator = MoerNew(VulkanDescriptorSetAllocator)(this);
        LOG_INFO("VulkanRHI: Descriptor set allocator is created.");
    }

    TExtensionArray VulkanDevice::GetGpuExtensions(VkPhysicalDevice _gpu) const {
        uint32_t gpu_extension_count;
        // check extensions
        vkEnumerateDeviceExtensionProperties(_gpu, nullptr, &gpu_extension_count, nullptr);
        Moer::Array<VkExtensionProperties> gpu_extensions(gpu_extension_count);
        vkEnumerateDeviceExtensionProperties(_gpu, nullptr, &gpu_extension_count, gpu_extensions.data());

        TExtensionArray ret;
        for (const auto& extension : gpu_extensions) { ret.push_back(extension.extensionName); }

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

    VulkanCommandAllocator& VulkanDevice::GetCurrentCommandAllocator() {
        auto thread_id = ThreadManager::Instance().GetCurrentThreadIndex();
        return m_command_allocators[thread_id];
    }

    //uint32_t VulkanDevice::GetMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties, VkBool32* mem_type_found) const {
    //    return 0;
    //}
    static uint32_t GetQueueFamilyIndice(std::span<const VkQueueFamilyProperties> _queue_family_props, VkQueueFlags _target_queue_flags, VkQueueFlags _exclude_queue_flags) {
        for (uint32_t i = 0; i < _queue_family_props.size(); ++i) { if (_queue_family_props[i].queueFlags & _target_queue_flags && !(_queue_family_props[i].queueFlags & _exclude_queue_flags)) { return i; } }
        return -1;
    }

    int32_t VulkanDevice::GetQueueFamilyIndex(const Moer::Array<VkQueueFamilyProperties>& queue_family_props, VkQueueFlags _queue_flags) const {
        // Dedicated queue for transfer
        if ((_queue_flags & VK_QUEUE_TRANSFER_BIT) == _queue_flags) { for (uint32_t i = 0; i < queue_family_props.size(); ++i) { if ((queue_family_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && (queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) { return i; } } }

        // Dedicated queue for compute
        if ((_queue_flags & VK_QUEUE_COMPUTE_BIT) == _queue_flags) { for (uint32_t i = 0; i < queue_family_props.size(); ++i) { if ((queue_family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 && (queue_family_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) == 0) { return i; } } }

        // Default queue
        for (uint32_t i = 0; i < queue_family_props.size(); ++i) { if ((queue_family_props[i].queueFlags & _queue_flags) == _queue_flags) { return i; } }

        return -1;
        // CRITICAL_AND_THROW("No suitable queue family found for " + std::to_string(_queue_flags));
    }

    QueueFamilyIndices VulkanDevice::QueryQueueFamilyIndices(VkPhysicalDevice _gpu) const {
        QueueFamilyIndices indices;

        auto queue_family_props = GetQueueFamilyProperties(_gpu);
        //todo:what's the best queue for ray tracing operation?how to tell a queue support raytracing operation?
        auto graphics = GetQueueFamilyIndice(queue_family_props, VK_QUEUE_GRAPHICS_BIT, VkQueueFlagBits(0));
        if (graphics >= 0) {
            indices.graphics   = graphics;
            indices.raytracing = graphics;
            indices.present    = graphics;
        }
        auto transfer = GetQueueFamilyIndice(queue_family_props, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        if (transfer < 0) { transfer = GetQueueFamilyIndice(queue_family_props, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT); }
        if (transfer < 0) { transfer = indices.graphics.value(); }
        indices.transfer = transfer;

        auto compute = GetQueueFamilyIndice(queue_family_props, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
        if (compute >= 0) { indices.compute = compute; } else { indices.compute = indices.transfer; }

        return indices;
    }

    bool VulkanDevice::CheckEnabledExtensionsSupported(VkPhysicalDevice _gpu, const TVulkanDeviceExtensionArray& _enabled_extensions) const {
        // only check whether the extension is included by the GPU, not check corresponding features
        auto gpu_extensions = GetGpuExtensions(_gpu);

        for (const auto& extension : _enabled_extensions) {
            if (std::find(gpu_extensions.begin(), gpu_extensions.end(), extension->GetExtensionName()) == gpu_extensions.end()) {
                LOG_WARNING("Disable the extension '{}' is not supported", extension->GetExtensionName());
                if (!extension->IsOptional()) { return false; }
                extension->Disable();
            }
        }

        return true;
    }

    bool VulkanDevice::CheckEnabledFeaturesSupported(VkPhysicalDevice _gpu, const VulkanPhysicalDeviceFeatures& _enabled_features, uint32_t _api_version) {
        m_core_features.Query(_gpu, _api_version);

        return m_core_features.Contains(_enabled_features);
    }

    VkDescriptorType METoVkDescriptorType(SpvReflectDescriptorType _desc, SpvReflectResourceType _res) {
        switch (_desc) {
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
            case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
        }
    }

    void VulkanDevice::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& _create_info) {
        _create_info = {};

        _create_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        _create_info.pNext           = nullptr;
        _create_info.flags           = 0;
        _create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        _create_info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        _create_info.pfnUserCallback = DebugCallback;
        _create_info.pUserData       = nullptr;
    }

    void VulkanDevice::SetupDebugUtilsMessengerEXT() {
        vk_create_debug_utils_messenger_ext  = PFN_vkCreateDebugUtilsMessengerEXT(vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        vk_destroy_debug_utils_messenger_ext = PFN_vkDestroyDebugUtilsMessengerEXT(vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));

        if (vk_create_debug_utils_messenger_ext == nullptr || vk_destroy_debug_utils_messenger_ext == nullptr) { assert(false); }

        VkDebugUtilsMessengerCreateInfoEXT debug_utils_messenger_create_info{};
        PopulateDebugMessengerCreateInfo(debug_utils_messenger_create_info);

        assert(vk_create_debug_utils_messenger_ext(m_instance, &debug_utils_messenger_create_info, nullptr, &debug_utils_messenger) == VK_SUCCESS);
    }

    void VulkanDevice::SetupDebugProcs() {
        vk_cmd_begin_debug_utils_label_ext  = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetInstanceProcAddr(m_instance, "vkCmdBeginDebugUtilsLabelEXT"));
        vk_cmd_end_debug_utils_label_ext    = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetInstanceProcAddr(m_instance, "vkCmdEndDebugUtilsLabelEXT"));
        vk_cmd_insert_debug_utils_label_ext = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(vkGetInstanceProcAddr(m_instance, "vkCmdInsertDebugUtilsLabelEXT"));
        vk_set_debug_utils_object_name_ext  = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetInstanceProcAddr(m_instance, "vkSetDebugUtilsObjectNameEXT"));
    }

    void VulkanDevice::SetupProcs() {
        vk_get_descriptor_ext                           = reinterpret_cast<PFN_vkGetDescriptorEXT>(vkGetDeviceProcAddr(m_device, "vkGetDescriptorEXT"));
        vk_get_descriptor_set_layout_binding_offset_ext = reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(vkGetDeviceProcAddr(m_device, "vkGetDescriptorSetLayoutBindingOffsetEXT"));
        vk_get_descriptor_set_layout_size_ext           = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(vkGetDeviceProcAddr(m_device, "vkGetDescriptorSetLayoutSizeEXT"));
    }

    VulkanDescriptorHeap& VulkanDevice::GetGlobalDescriptorHeap() { return m_global_descriptor_heap; }

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
        Moer::Array<uint64>& _out_reflect_flags,
        // cpp param name hash to shader binding index
        UnorderedMap<uint, UnorderedMap<uint, VkDescriptorSetLayoutBinding>>& _out_descriptor_bindings,
        // set to binding to binding info, actual vk pipeline layout
        VkPushConstantRange& _out_push_constant_ranges,
        // push constant range
        int& _out_constant_idx,
        uint64& _out_valid_bits,
        // push constant index in cpp param
        uint& _max_set// max set index, calculate descriptor set count
        ) {
        for (const auto& hash : _shader_info.layout_hash) {
            uint idx                       = uint(&hash - _shader_info.layout_hash.data());
            _out_hash_2_idx[GetHash(hash)] = idx;
            EShaderArgType arg_type        = _shader_info.arg_types[idx];

            const UnorderedMap<std::string, ReflectParamInfo>& reflect_map  = _info.shader_param_map.reflect_map;
            const auto                                         binding_iter = reflect_map.find(hash.data());
            bool                                               b_found      = binding_iter != reflect_map.end();
            if (arg_type != SDA_BindlessArray && !b_found) { continue; }
            switch (arg_type) {
                case SDA_BindlessArray: {
                    auto bdls_iter = reflect_map.find(ReflectParamInfo::bdls_name.data());
                    if (bdls_iter == reflect_map.end()) {
                        // no bindless array found, skip
                        break;
                    }
                    _out_valid_bits |= 1 << idx;
                    const ReflectParamInfo&                binding_info              = bdls_iter->second;
                    const ReflectParamInfo::BindlessArray& bdls_array                = binding_info.spirv.bindless;
                    bool                                   b_found_bindless_buffer   = bdls_array.buffer.has_value();
                    bool                                   b_found_bindless_texture  = bdls_array.image.has_value();
                    bool                                   b_found_bindless_indirect = bdls_array.array.has_value();
                    uint                                   texture_set               = 0;
                    uint                                   buffer_set                = 0;
                    if (b_found_bindless_indirect) {
                        const ReflectParamInfo::Bindless& indirect_binding_info = bdls_array.array.value();

                        auto&                 set           = _out_descriptor_bindings[indirect_binding_info.set];
                        static constexpr uint indirect_slot = 0;
                        static constexpr uint sampler_slot  = 0;
                        static constexpr uint texture_slot  = 1;
                        static constexpr uint buffer_slot   = 1;
                        assert(indirect_binding_info.binding == 0 && "Indirect Binding Slot Must be 0.");
                        auto& vk_binding           = set[indirect_slot];
                        vk_binding.binding         = indirect_slot;
                        vk_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        vk_binding.descriptorCount = 1;
                        vk_binding.stageFlags |= _stage;
                        vk_binding.pImmutableSamplers = nullptr;
                        _max_set                      = uint(std::max(int(_max_set), int(indirect_binding_info.set)));
                        buffer_set                    = indirect_binding_info.set;
                        if (b_found_bindless_buffer) {
                            auto& temp_binding           = set[buffer_slot];
                            temp_binding.binding         = buffer_slot;
                            temp_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                            temp_binding.descriptorCount = 10000;
                            temp_binding.stageFlags |= _stage;
                            temp_binding.pImmutableSamplers = nullptr;
                        }

                        if (b_found_bindless_texture) {
                            auto& texture_set            = _out_descriptor_bindings[bdls_array.image.value().set];
                            auto& temp_binding           = texture_set[texture_slot];
                            temp_binding.binding         = texture_slot;
                            temp_binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                            temp_binding.descriptorCount = 10000;
                            temp_binding.stageFlags |= _stage;
                            temp_binding.pImmutableSamplers = nullptr;

                            auto& temp_sampler_binding           = texture_set[sampler_slot];
                            temp_sampler_binding.binding         = sampler_slot;
                            temp_sampler_binding.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
                            temp_sampler_binding.descriptorCount = _device.ImmutableSamplerCount();
                            temp_sampler_binding.stageFlags |= _stage;
                            temp_sampler_binding.pImmutableSamplers = _device.GetImmutableSamplers();
                            _max_set                                = uint(std::max(int(_max_set), int(bdls_array.image.value().set)));
                        }

                        _out_reflect_flags[idx] = EncodeBindlessInfo(texture_set, buffer_set, _stage, b_found_bindless_texture, b_found_bindless_buffer);
                    }

                    break;
                }
                case SDA_Constant: {
                    _out_valid_bits |= 1 << idx;
                    const ReflectParamInfo&           binding_info = binding_iter->second;
                    const ReflectParamInfo::Constant& constant     = std::get<ReflectParamInfo::Constant>(binding_info.spirv.resources.data);
                    _out_constant_idx                              = idx;
                    _out_push_constant_ranges.size                 = std::max(_out_push_constant_ranges.size, constant.size);
                    _out_push_constant_ranges.stageFlags |= _stage;
                    _out_reflect_flags[idx] = EncodeReflectInfo(0, constant.size, _out_push_constant_ranges.stageFlags);
                    break;
                }
                case SDA_Buffer:
                case SDA_Texture:
                case SDA_Sampler: {
                    _out_valid_bits |= 1 << idx;
                    const ReflectParamInfo&           binding_info = binding_iter->second;
                    const ReflectParamInfo::Resource& resource     = std::get<ReflectParamInfo::Resource>(binding_info.spirv.resources.data);

                    // auto  rel_desc_type        = std::get<SpvReflectDescriptorType>(desc_type);
                    // auto  rel_res_type         = std::get<SpvReflectResourceType>(resource_type);
                    auto  desc_type            = METoVkDescriptorType(SpvReflectDescriptorType(resource.desc_type), SpvReflectResourceType(resource.resource_type));
                    auto& set                  = _out_descriptor_bindings[resource.set];
                    auto& vk_binding           = set[resource.binding];
                    vk_binding.binding         = resource.binding;
                    vk_binding.descriptorType  = desc_type;
                    vk_binding.descriptorCount = 1;
                    vk_binding.stageFlags |= _stage;
                    vk_binding.pImmutableSamplers = nullptr;
                    _out_reflect_flags[idx]       = EncodeReflectInfo(resource.set, resource.binding, vk_binding.stageFlags);
                    _max_set                      = uint(std::max(int(_max_set), int(resource.set)));
                    break;
                }
                default:
                    assert(false && "Unknown shader arg type.");
            }
        }
    }

    PipelineHandle VulkanDevice::CreatePipeline(GfxPsoCreateInfo&& _create_info, PipelineShaderInfo&& _shader_info) {
        VulkanPipelineState* vk_pso = MoerNew(VulkanPipelineState)(this, VulkanPipelineState::GFX);

        uint32_t attachment_count = _create_info.color_attachment_count;

        Moer::Array<VkFormat> color_attachment_formats(attachment_count);

        for (int i = 0; i < attachment_count; ++i) { color_attachment_formats[i] = VkFormat(_create_info.color_attachments_info[i].pixel_format); }
        VkPipelineRenderingCreateInfo rendering_create_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering_create_info.pNext                   = nullptr;
        rendering_create_info.viewMask                = 0;
        rendering_create_info.colorAttachmentCount    = attachment_count;
        rendering_create_info.pColorAttachmentFormats = color_attachment_formats.data();
        rendering_create_info.depthAttachmentFormat   = VulkanEnumTranslator::METoVKFormat(_create_info.depth_stencil_format);
        rendering_create_info.stencilAttachmentFormat = VulkanEnumTranslator::METoVKFormat(_create_info.depth_stencil_format);

        auto to_vk_blend_attachment = [](const RHIBlendAttachmentInfo& _info) {
            VkPipelineColorBlendAttachmentState state{};
            state.blendEnable =
            (_info.color_blend_op != BO_ADD || _info.color_dst_blend_factor != BF_ZERO || _info.color_src_blend_factor != BF_ONE ||
             _info.alpha_blend_op != BO_ADD || _info.alpha_dst_blend_factor != BF_ZERO || _info.alpha_src_blend_factor != BF_ONE) ?
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
        for (int i = 0; i < attachment_count; ++i) { color_blend_attachments[i] = to_vk_blend_attachment(_create_info.color_attachments_info[i].blend_state_info); }
        // color blend state
        VkPipelineColorBlendStateCreateInfo color_blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};

        color_blend_state.logicOp         = VK_LOGIC_OP_COPY;
        color_blend_state.logicOpEnable   = VK_FALSE;
        color_blend_state.attachmentCount = attachment_count;
        color_blend_state.pAttachments    = color_blend_attachments.data();

        Moer::Array<VkPipelineShaderStageCreateInfo> shader_stages;

        auto emplace_shader = [&](SingleShaderInfo& _info, VkShaderStageFlagBits _stage) {
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
        using TDescSet      = UnorderedMap<uint, VkDescriptorSetLayoutBinding>;
        using TPipelineSets = UnorderedMap<uint, TDescSet>;
        TPipelineSets       descriptor_bindings;
        VkPushConstantRange push_constant_ranges{.offset = 0, .size = 0};
        uint                max_set = 0;

        Array<uint64>              reflect_flags(_shader_info.layout_hash.size(), 0u);
        uint64                     valid_bits = 0;
        UnorderedMap<uint64, uint> hash_2_idx;
        int                        constant_idx = -1;

        auto merge_reflect_info = [&](const SingleShaderInfo& _info, VkShaderStageFlagBits _stage) {
            MergeReflectInfo(*this,
                             _info,
                             _shader_info,
                             _stage,
                             hash_2_idx,
                             reflect_flags,
                             descriptor_bindings,
                             push_constant_ranges,
                             constant_idx,
                             valid_bits,
                             max_set);
        };

        // shader stage
        std::visit([&](auto&& _shader_info_group) {
                       using T = std::decay_t<decltype(_shader_info_group)>;
                       if constexpr (std::is_same_v<T, ShaderVsPs>) {
                           shader_stages.reserve(2);
                           emplace_shader(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                           emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                           merge_reflect_info(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                           merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                       } else if constexpr (std::is_same_v<T, ShaderVsGsPs>) {
                           shader_stages.reserve(3);
                           emplace_shader(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                           emplace_shader(_shader_info_group.gs, VK_SHADER_STAGE_GEOMETRY_BIT);
                           emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                           merge_reflect_info(_shader_info_group.vs, VK_SHADER_STAGE_VERTEX_BIT);
                           merge_reflect_info(_shader_info_group.gs, VK_SHADER_STAGE_GEOMETRY_BIT);
                           merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                       } else if constexpr (std::is_same_v<T, ShaderMsPs>) {
                           shader_stages.reserve(2);
                           emplace_shader(_shader_info_group.ms, VK_SHADER_STAGE_MESH_BIT_NV);
                           emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                           merge_reflect_info(_shader_info_group.ms, VK_SHADER_STAGE_MESH_BIT_NV);
                           merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                       } else if constexpr (std::is_same_v<T, ShaderTsMsPs>) {
                           shader_stages.reserve(3);
                           emplace_shader(_shader_info_group.ts, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
                           emplace_shader(_shader_info_group.ms, VK_SHADER_STAGE_MESH_BIT_NV);
                           emplace_shader(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                           merge_reflect_info(_shader_info_group.ts, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
                           merge_reflect_info(_shader_info_group.ms, VK_SHADER_STAGE_MESH_BIT_NV);
                           merge_reflect_info(_shader_info_group.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
                       }
                   },
                   _shader_info.shader_group);

        VkPipelineVertexInputStateCreateInfo           vertex_input_state{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        Moer::Array<VkVertexInputBindingDescription>   binding_descs;
        Moer::Array<VkVertexInputAttributeDescription> attribute_descs;

        auto to_vk_vertex_stream = [&]() {
            uint binding_offset = 0;
            binding_descs.reserve(_create_info.vertex_stream.bindings.size());
            auto attribute_cnt = [&]() {
                uint cnt = 0;
                for (const auto& binding : _create_info.vertex_stream.bindings) { cnt += binding.vertex_elements.size(); }
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
                        attrib_location++,
                        binding_offset,
                        format_info.format,
                        attrib_offset);
                    attrib_offset += format_info.stride;
                    binding_stride = attrib_offset;
                }
                binding_descs.emplace_back(
                    binding_offset,
                    binding_stride,
                    VulkanEnumTranslator::METoVKVertexInputRate(binding.input_rate));
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
        input_assembly_state.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_state.pNext                  = nullptr;
        input_assembly_state.flags                  = 0;
        input_assembly_state.topology               = VulkanEnumTranslator::METoVKPrimitiveTopology(_create_info.primitive_topology);
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

        auto to_rasterize_state = [](const RHIRasterizeInfo& info) {
            VkPipelineRasterizationStateCreateInfo state{};
            state.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            state.pNext                   = nullptr;
            state.flags                   = 0;
            state.depthClampEnable        = info.b_depth_clamp_enable ? VK_TRUE : VK_FALSE;
            state.rasterizerDiscardEnable = VK_FALSE;// MARK...
            state.polygonMode             = VulkanEnumTranslator::METoVKPolygonMode(info.fill_mode);
            state.cullMode                = VulkanEnumTranslator::METoVKCullModeFlags(info.cull_mode);
            state.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;// MARK...
            state.depthBiasEnable         = info.b_depth_bias ? VK_TRUE : VK_FALSE;
            state.depthBiasConstantFactor = info.depth_bias;
            state.depthBiasClamp          = info.depth_bias_clamp;
            state.depthBiasSlopeFactor    = info.depth_bias_slop_factor;
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
            state.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            state.pNext                 = nullptr;
            state.flags                 = 0;
            state.depthTestEnable       = (info.b_enable_depth_write || info.depth_test_op == ECompareOption::CO_ALWAYS) ? VK_TRUE : VK_FALSE;
            state.depthWriteEnable      = info.b_enable_depth_write;
            state.depthCompareOp        = VulkanEnumTranslator::METoVKCompareOp(info.depth_test_op);
            state.depthBoundsTestEnable = VK_FALSE;// MARK...
            state.minDepthBounds        = 0.0f;
            state.maxDepthBounds        = 1.0f;

            state.stencilTestEnable = (info.b_enable_front_face_stencil || info.b_enable_back_face_stencil) ? VK_TRUE : VK_FALSE;
            state.front.failOp      = VulkanEnumTranslator::METoVKStencilOp(info.front_face_stencil_fail_stencil_op);
            state.front.passOp      = VulkanEnumTranslator::METoVKStencilOp(info.front_face_pass_stencil_op);
            state.front.depthFailOp = VulkanEnumTranslator::METoVKStencilOp(info.front_face_depth_fail_stencil_op);
            state.front.compareOp   = VulkanEnumTranslator::METoVKCompareOp(info.front_face_stencil_test);
            state.front.compareMask = info.stencil_readmask;
            state.front.writeMask   = info.stencil_writemask;
            state.front.reference   = 0;

            if (info.b_enable_back_face_stencil) {
                state.back.failOp      = VulkanEnumTranslator::METoVKStencilOp(info.back_face_stencil_fail_stencil_op);
                state.back.passOp      = VulkanEnumTranslator::METoVKStencilOp(info.back_face_pass_stencil_op);
                state.back.depthFailOp = VulkanEnumTranslator::METoVKStencilOp(info.back_face_depth_fail_stencil_op);
                state.back.compareOp   = VulkanEnumTranslator::METoVKCompareOp(info.back_face_stencil_test);
                state.back.compareMask = info.stencil_readmask;
                state.back.writeMask   = info.stencil_writemask;
                state.back.reference   = 0;
            } else { state.front = state.back; }
            return std::move(state);
        };
        auto vk_depth_stencil_state = to_depth_stencil_state(_create_info.depth_stencil_info);

        // dynamic state
        Moer::StaticArray<VkDynamicState, 2> states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo     dynamic_state{};
        dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.pNext             = nullptr;
        dynamic_state.flags             = 0;
        dynamic_state.dynamicStateCount = states.size();
        dynamic_state.pDynamicStates    = states.data();

        // init descriptor set layouts and pipeline resource cache
        Moer::Array<TDescriptorSetLayoutBindingArray> desc_sets_array(max_set + 1u);
        for (const auto& [set_idx, desc_set] : descriptor_bindings) {
            TDescriptorSetLayoutBindingArray desc_set_layouts;
            for (const auto& [binding_idx, binding] : desc_set) { desc_set_layouts.push_back(binding); }
            desc_sets_array[set_idx] = std::move(desc_set_layouts);
        }
        vk_pso->InitDescriptorSetLayouts(desc_sets_array);
        vk_pso->InitPipelineResourceCache(desc_sets_array);

        const auto& layouts = vk_pso->GetDescriptorSetsLayout()->GetLayouts();
        // create pipeline layout
        VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
        pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.pNext                  = nullptr;
        pipeline_layout_create_info.flags                  = 0;
        pipeline_layout_create_info.setLayoutCount         = layouts.size();
        pipeline_layout_create_info.pSetLayouts            = layouts.data();
        pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size == 0 ? 0 : 1;
        pipeline_layout_create_info.pPushConstantRanges    = &push_constant_ranges;

        vk_pso->CreatePipelineLayout(pipeline_layout_create_info);
        VkGraphicsPipelineCreateInfo pipeline_create_info{};
        pipeline_create_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_create_info.pNext               = &rendering_create_info;
        pipeline_create_info.flags               = 0;
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
        pipeline_create_info.layout              = vk_pso->GetPipelineLayout();
        pipeline_create_info.renderPass          = nullptr;
        pipeline_create_info.subpass             = 0;
        pipeline_create_info.basePipelineHandle  = nullptr;// MARK...
        pipeline_create_info.basePipelineIndex   = -1;

        // vk_pso->CreateGraphicsPipeline(pipeline_create_info);
        VK_CHECK_RESULT(vkCreateGraphicsPipelines(
            m_device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline));

        return PipelineHandle{
            .handle = VkPipelineHandle{reinterpret_cast<uint64>(vk_pso)},
            .binding_infos = std::move(reflect_flags),
            .hash_2_info_index = std::move(hash_2_idx),
            .valid_bits = valid_bits,
            .constant_idx = constant_idx};
    }

    PipelineHandle VulkanDevice::CreatePipeline(PipelineShaderInfo&& _shader_info) {

        auto* vk_pso = MoerNew(VulkanPipelineState)(this, VulkanPipelineState::Compute);

        using TDescSet      = UnorderedMap<uint, VkDescriptorSetLayoutBinding>;
        using TPipelineSets = UnorderedMap<uint, TDescSet>;
        VkComputePipelineCreateInfo pipeline_create_info{};

        VkPipelineShaderStageCreateInfo& shader_stage = pipeline_create_info.stage;

        TPipelineSets              descriptor_bindings;
        VkPushConstantRange        push_constant_ranges{.offset = 0, .size = 0};
        uint                       max_set = 0;
        Array<uint64>              reflect_flags(_shader_info.layout_hash.size(), 0u);
        UnorderedMap<uint64, uint> hash_2_idx;
        int                        constant_idx = -1;
        uint64 valid_bits = 0;
        auto merge_reflect_info = [&](const SingleShaderInfo& _info, VkShaderStageFlagBits _stage) {
            MergeReflectInfo(*this,
                             _info,
                             _shader_info,
                             _stage,
                             hash_2_idx,
                             reflect_flags,
                             descriptor_bindings,
                             push_constant_ranges,
                             constant_idx,
                             valid_bits,
                             max_set);
        };

        std::visit([&](auto&& _shader_info_group) {
                       using T = std::decay_t<decltype(_shader_info_group)>;
                       if constexpr (std::is_same_v<T, ShaderCs>) { merge_reflect_info(_shader_info_group.cs, VK_SHADER_STAGE_COMPUTE_BIT); } else { LOG_ERROR("Unsupported shader group type: {}", typeid(T).name()); }
                   },
                   _shader_info.shader_group);

        // init descriptor set layouts and pipeline resource cache
        Moer::Array<TDescriptorSetLayoutBindingArray> desc_sets_array(max_set + 1u);
        for (const auto& [set_idx, desc_set] : descriptor_bindings) {
            TDescriptorSetLayoutBindingArray desc_set_layouts;
            for (const auto& [binding_idx, binding] : desc_set) { desc_set_layouts.push_back(binding); }
            desc_sets_array[set_idx] = std::move(desc_set_layouts);
        }
        vk_pso->InitDescriptorSetLayouts(desc_sets_array);
        vk_pso->InitPipelineResourceCache(desc_sets_array);

        const auto& layouts = vk_pso->GetDescriptorSetsLayout()->GetLayouts();
        // create pipeline layout
        VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
        pipeline_layout_create_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.pNext                  = nullptr;
        pipeline_layout_create_info.flags                  = 0;
        pipeline_layout_create_info.setLayoutCount         = layouts.size();
        pipeline_layout_create_info.pSetLayouts            = layouts.data();
        pipeline_layout_create_info.pushConstantRangeCount = push_constant_ranges.size == 0 ? 0 : 1;
        pipeline_layout_create_info.pPushConstantRanges    = &push_constant_ranges;

        vk_pso->CreatePipelineLayout(pipeline_layout_create_info);

        pipeline_create_info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_create_info.pNext              = nullptr;
        pipeline_create_info.flags              = 0;
        pipeline_create_info.stage              = shader_stage;
        pipeline_create_info.layout             = vk_pso->GetPipelineLayout();
        pipeline_create_info.basePipelineHandle = nullptr;
        pipeline_create_info.basePipelineIndex  = -1;

        VK_CHECK_RESULT(
            vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &vk_pso->m_pipeline));
        return PipelineHandle{
            .handle = VkPipelineHandle{reinterpret_cast<uint64>(vk_pso)},
            .binding_infos = std::move(reflect_flags),
            .hash_2_info_index = std::move(hash_2_idx),
            .valid_bits = valid_bits,
            .constant_idx = constant_idx};
    }

    CommandQueue& VulkanDevice::GetCommandQueue(EQueueType _type) { return *gfx_queue; }

    TextureRef VulkanDevice::CreateTexture(Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt, uint32_t _array_size) {
        TextureInfo info{
            _size.z == 1 ? ETextureDimension::TEX_2D : ETextureDimension::TEX_3D,
            _usage,
            _format,
            EClearAttachment::COLOR,
            _size,
            uint8(_mip_cnt),
            1};

        return TextureRef{MoerNew(VulkanTexture)(info, this)};
    }

    BufferRef VulkanDevice::CreateBuffer(uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage) {
        BufferInfo info{_element_cnt, _byte_stride, _usage};
        return BufferRef{MoerNew(VulkanBuffer)(info, *this)};
    }

    BufferRef VulkanDevice::CreateStagingBuffer(uint64 _byte_size) {
        BufferInfo info{_byte_size, sizeof(uint8), EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST};
        return BufferRef{MoerNew(VulkanBuffer)(info, *this)};
    }

    BindlessArrayRef VulkanDevice::CreateBindlessArray(uint _max_size) {
        return MoerNew(VulkanBindlessArray)(this, _max_size);
    }

    FenceRef VulkanDevice::CreateFence() { return FenceRef{MoerNew(VulkanFence)(*this)}; }

    SwapchainRef VulkanDevice::CreateSwapchain(const SwapchainCreateInfo& _info) { return SwapchainRef{MoerNew(VkSwapchain)(*this, _info)}; }

    void VulkanDevice::EnqueueDeferredRelease(RHIResource* _object) { deferred_release_queue.Push(_object); }

    void VulkanDevice::FlushDeferredReleases() {
        Array<RHIResource*> objects;
        deferred_release_queue.PopAll(objects);
        for (auto* object : objects) { MoerDelete(object); }
    }

    const VkSampler VulkanDevice::GetSampler(Sampler _sampler) const {
        uint filter  = uint(_sampler.filter);
        uint address = uint(_sampler.address_mode);
        uint compare = uint(_sampler.compare_function);

        uint idx = (uint(SF_Num) * uint(SAM_Num)) * compare + (uint(SF_Num)) * address + filter;
        return immutable_samplers[idx];
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

};// namespace Moer::Render