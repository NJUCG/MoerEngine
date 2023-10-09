#include "config.h"
#include "misc/MacroUtils.h"
#include "misc/VulkanMacroUtils.h"

#include "rhi/vulkan/VulkanRHI.h"

#include "VulkanDebug.h"
#include "VulkanUtil.h"

#include <string>

const std::string vk_layer = MACRO_STR(__ENGINE_NAME__) MACRO_STR(_VK_LAYER_PATH);

namespace VUtil       = MoerEngine::RHI::Vulkan::Util;
namespace VDebug      = MoerEngine::RHI::Vulkan::Debug;
namespace VDebugUtils = MoerEngine::RHI::Vulkan::DebugUtils;

VulkanRHIImpl::VulkanRHIImpl() : m_instance(VK_NULL_HANDLE), m_device(nullptr), m_current_viewport(nullptr), settings({true}) {
    MOER_LOG_INFO("Built with Vulkan header version %u.%u.%u", VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));

    CreateInstance(settings.validation);
}

void VulkanRHIImpl::Initialize() {
    InitWindow();
    InitVulkan();
}

void VulkanRHIImpl::ShutDown() {}

#pragma region           resources creation
RHISamplerRef            VulkanRHIImpl::RHICreateSampler(const RHISamplerInitializer& _initializer) { return RHISamplerRef{}; }
RHIRasterizationStateRef VulkanRHIImpl::RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) { return RHIRasterizationStateRef{}; }
RHIDepthStencilStateRef  VulkanRHIImpl::RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) { return RHIDepthStencilStateRef{}; }
RHIMultisampleStateRef   VulkanRHIImpl::RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) { return RHIMultisampleStateRef{}; }
RHIBlendStateRef         VulkanRHIImpl::RHICreateBlendState(const RHIBlendStateInitializer& _init) { return RHIBlendStateRef{}; }
RHIVertexInputStateRef   VulkanRHIImpl::RHICreateVertexInputState(const VertexInputStateInitializerList& _init) { return RHIVertexInputStateRef{}; }

RHIVertexShaderRef   VulkanRHIImpl::RHICreateVertexShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIVertexShaderRef{}; }
RHIFragmentShaderRef VulkanRHIImpl::RHICreateFragmentShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIFragmentShaderRef{}; }
RHIGeometryShaderRef VulkanRHIImpl::RHICreateGeometryShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIGeometryShaderRef{}; }

RHIMeshShaderRef          VulkanRHIImpl::RHICreateMeshShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIMeshShaderRef{}; }
RHIAmplificationShaderRef VulkanRHIImpl::RHICreateAmplificationShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIAmplificationShaderRef{}; }

RHIComputeShaderRef VulkanRHIImpl::RHICreateComputeShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIComputeShaderRef{}; }

RHIShaderLibraryRef VulkanRHIImpl::RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) { return RHIShaderLibraryRef{}; }

RHIFenceRef VulkanRHIImpl::RHICreateFence(const std::string& name) { return RHIFenceRef{}; }

/* create cpu visible buffer for direct data transfer */
RHIStagingBufferRef VulkanRHIImpl::RHICreateStagingBuffer() { return RHIStagingBufferRef{}; }

RHIShaderBoundStateRef VulkanRHIImpl::RHICreateShaderBoundStage(
    RHIVertexInputState* _vertex_input,
    RHIVertexShader*     _vertex_shader,
    RHIFragmentShader*   _fragment_shader,
    RHIGeometryShader*   _geometry_shader) { return RHIShaderBoundStateRef{}; }

RHIGraphicsPipelineStateRef VulkanRHIImpl::RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) { return RHIGraphicsPipelineStateRef{}; }

RHIComputePipelineStateRef VulkanRHIImpl::RHICreateComputePipelineState(RHIComputeShader* _compute_shader) { return RHIComputePipelineStateRef{}; }

RHIUniformBufferRef VulkanRHIImpl::RHICreateUniformBuffer(const void* data, const RHIUniformBufferLayout* layout, EBufferUsageFlags _usage) { return RHIUniformBufferRef{}; }

void VulkanRHIImpl::RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) {}

RHIBufferRef VulkanRHIImpl::RHICreateBuffer(const RHIBufferCreateInfo& info) { return RHIBufferRef{}; }

RHIShaderResourceViewRef  VulkanRHIImpl::RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) { return RHIShaderResourceViewRef{}; }
RHIUnorderedAccessViewRef VulkanRHIImpl::RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) { return RHIUnorderedAccessViewRef{}; }
#pragma endregion

void VulkanRHIImpl::InitWindow() {
}

void VulkanRHIImpl::InitVulkan() {
    DeviceInitializer initializer;
    initializer.instance           = m_instance;
    initializer.surface            = m_surface;
    initializer.enabled_features   = GetEnabledDeviceFeatures();
    initializer.enabled_extensions = GetEnabledDeviceExtensions();

    m_device->Init(initializer);

    m_swap_chain->Connect(m_instance, m_device);
    uint32_t width, height;
    // glfwGetFramebufferSize(m_window, &width, &height);
    m_swap_chain->Init(&width, &height, false, true);
}

#pragma region vulkan functions

void VulkanRHIImpl::CreateInstance(bool _enable_validation) {
    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // application_info.pApplicationName = MACRO_STR(__ENGINE_NAME__);
    // application_info.applicationVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    application_info.pEngineName   = MACRO_STR(__ENGINE_NAME__);
    application_info.engineVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    application_info.apiVersion    = VK_API_VERSION_1_3;

    m_instance_extensions    = GetInstanceExtensions();
    auto required_extensions = GetRequiredExtensionsSupported();
    required_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);

    // Enable surface extensions depending on os
#if defined(_WIN32)
    required_extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    required_extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif

    // Enable the debug utils extension if available
    bool debug_utils_available = std::find(m_instance_extensions.begin(), m_instance_extensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != m_instance_extensions.end();
    if (_enable_validation && debug_utils_available) {
        required_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo instance_create_info{};
    instance_create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pNext                   = nullptr;
    instance_create_info.flags                   = 0;
    instance_create_info.pApplicationInfo        = &application_info;
    instance_create_info.enabledExtensionCount   = static_cast<uint32_t>(required_extensions.size());
    instance_create_info.ppEnabledExtensionNames = required_extensions.data();

    const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";
    if (_enable_validation && CheckValidationLayerSupport(validation_layer_name)) {
        instance_create_info.enabledLayerCount   = 1;
        instance_create_info.ppEnabledLayerNames = &validation_layer_name;
    } else {
        instance_create_info.enabledLayerCount   = 0;
        instance_create_info.ppEnabledLayerNames = nullptr;
    }

    VK_CHECK_RESULT(vkCreateInstance(&instance_create_info, nullptr, &m_instance))

    if (debug_utils_available) {
        VDebugUtils::Setup(m_instance);
    }

    if (_enable_validation) {
        VDebug::SetupDebugUtilsMessengerEXT(m_instance);
    }
}

#pragma endregion

#pragma region helper functions

std::vector<const char*> VulkanRHIImpl::GetInstanceExtensions() {
    std::vector<const char*> instance_extensions;

    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    if (ext_count > 0) {
        std::vector<VkExtensionProperties> extensions(ext_count);
        if (vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, extensions.data()) == VK_SUCCESS) {
            for (const auto& extension : extensions) {
                instance_extensions.push_back(extension.extensionName);
            }
        }
    }

    return instance_extensions;
}

std::vector<const char*> VulkanRHIImpl::GetRequiredExtensionsSupported() {
    std::vector<const char*> required_extensions;
    if (!m_enabled_instance_extensions.empty()) {
        for (auto* extension : m_enabled_instance_extensions) {
            if (std::find(m_instance_extensions.begin(), m_instance_extensions.end(), extension) == m_instance_extensions.end()) {
                VUtil::ExitFatal("Enabled instance extension '" + std::string(extension) + "' is not supported!", -1);
            }
            required_extensions.push_back(extension);
        }
    }

    return required_extensions;
}

bool VulkanRHIImpl::CheckValidationLayerSupport(const char* layer_name) {
    uint32_t instance_layer_count = 0;
    vkEnumerateInstanceLayerProperties(&instance_layer_count, nullptr);
    std::vector<VkLayerProperties> instance_layer_properties(instance_layer_count);
    vkEnumerateInstanceLayerProperties(&instance_layer_count, instance_layer_properties.data());
    bool validation_layer_present = false;

    for (auto layer_property : instance_layer_properties) {
        if (strcmp(layer_property.layerName, layer_name) == 0) {
            validation_layer_present = true;
            break;
        }
    }

    return validation_layer_present;
}

#pragma endregion
