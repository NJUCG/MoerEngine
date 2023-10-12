#include "config.h"

#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "misc/MacroUtils.h"

#include "rhi/vulkan/VulkanRHI.h"
#include "VulkanExtension.h"

#include "VulkanDebug.h"
#include "VulkanUtil.h"

#include "VulkanDevice.h"
#include "VulkanSwapChain.h"

#include <GLFW/glfw3.h>

#include <string>

const char* vk_layer = MACRO_STR(VK_LAYER_PATH);

namespace VkUtil = MoerEngine::RHI::Vulkan::Util;

VulkanRHIImpl::VulkanRHIImpl(GLFWwindow* _window) : m_instance(VK_NULL_HANDLE), m_device(nullptr), m_current_viewport(nullptr) {
    LOG_INFO("Built with Vulkan header version {0:d}.{1:d}.{2:d}", VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));
    // SetEnvironmentVariableA("VK_LAYER_PATH", vk_layer);

    CreateInstance();
    InitSurface(_window);
}

void VulkanRHIImpl::Initialize() {
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

void VulkanRHIImpl::RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) {}

RHIBufferRef VulkanRHIImpl::RHICreateBuffer(const RHIBufferCreateInfo& info) { return RHIBufferRef{}; }
void*        VulkanRHIImpl::RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) { return nullptr; }
void         VulkanRHIImpl::RHIUnmapBuffer(RHIBuffer* _buffer) {}

RHITextureRef VulkanRHIImpl::RHICreateTexture(const RHITextureCreateInfo& info) { return RHITextureRef{}; };

RHIShaderResourceViewRef  VulkanRHIImpl::RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) { return RHIShaderResourceViewRef{}; }
RHIUnorderedAccessViewRef VulkanRHIImpl::RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) { return RHIUnorderedAccessViewRef{}; }
#pragma endregion

void VulkanRHIImpl::InitSurface(GLFWwindow* _window) {
    VK_CHECK_RESULT(glfwCreateWindowSurface(m_instance, _window, nullptr, &m_surface));
}

void VulkanRHIImpl::InitVulkan() {
    DeviceInitializer initializer;
    initializer.instance           = m_instance;
    initializer.surface            = m_surface;
    initializer.enabled_features   = {};
    initializer.enabled_extensions = VulkanDeviceExtension::GetMESupportedDeviceExtensions();

    m_device = std::make_shared<VulkanDevice>();
    m_device->Init(initializer);

    m_swap_chain = std::make_shared<VulkanSwapChain>();
    m_swap_chain->Connect(m_instance, m_surface, m_device);
    uint32_t width, height;
    // glfwGetFramebufferSize(m_window, &width, &height);
    m_swap_chain->Init(&width, &height, true);
}

#pragma region vulkan functions

void VulkanRHIImpl::CreateInstance() {
    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // application_info.pApplicationName = MACRO_STR(__ENGINE_NAME__);
    // application_info.applicationVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    application_info.pEngineName   = MACRO_STR(__ENGINE_NAME__);
    application_info.engineVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    application_info.apiVersion    = VK_API_VERSION_1_3;

    m_instance_extensions         = VulkanInstanceExtension::GetDriverSupportedInstanceExtensionNames();
    m_enabled_instance_extensions = VulkanInstanceExtension::GetMESupportedInstanceExtensions();

    bool extension_supported   = CheckEnabledExtensions();
    bool debug_utils_available = std::find(m_enabled_instance_extensions.begin(), m_enabled_instance_extensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != m_enabled_instance_extensions.end();

    VkInstanceCreateInfo instance_create_info{};
    instance_create_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pNext            = nullptr;
    instance_create_info.flags            = 0;
    instance_create_info.pApplicationInfo = &application_info;

    auto n = m_enabled_instance_extensions.size();

    std::vector<const char*> r_extensions(n, nullptr);
    for (size_t i = 0; i < n; ++i) {
        r_extensions[i] = m_enabled_instance_extensions[i].c_str();
    }
    instance_create_info.enabledExtensionCount   = n;
    instance_create_info.ppEnabledExtensionNames = r_extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};

    const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";
    if (CheckValidationLayer(validation_layer_name)) {
        instance_create_info.enabledLayerCount   = 1;
        instance_create_info.ppEnabledLayerNames = &validation_layer_name;
        MoerEngine::RHI::Vulkan::Debug::PopulateDebugMessengerCreateInfo(debug_create_info);
        instance_create_info.pNext = &debug_create_info;
    } else {
        instance_create_info.enabledLayerCount   = 0;
        instance_create_info.ppEnabledLayerNames = nullptr;
    }

    VK_CHECK_RESULT(vkCreateInstance(&instance_create_info, nullptr, &m_instance))

    MoerEngine::RHI::Vulkan::Debug::SetupDebugUtilsMessengerEXT(m_instance);

    if (debug_utils_available) {
        MoerEngine::RHI::Vulkan::DebugUtils::Setup(m_instance);
    }
}

#pragma endregion

#pragma region helper functions

bool VulkanRHIImpl::CheckValidationLayer(const std::string& layer_name) {
    uint32_t instance_layer_count = 0;
    vkEnumerateInstanceLayerProperties(&instance_layer_count, nullptr);
    std::vector<VkLayerProperties> instance_layer_properties(instance_layer_count);
    vkEnumerateInstanceLayerProperties(&instance_layer_count, instance_layer_properties.data());
    bool validation_layer_present = false;

    for (auto layer_property : instance_layer_properties) {
        if (layer_name == layer_property.layerName) {
            validation_layer_present = true;
            break;
        }
    }

    return validation_layer_present;
}
bool VulkanRHIImpl::CheckEnabledExtensions() {
    if (!m_enabled_instance_extensions.empty()) {
        for (const auto& extension : m_enabled_instance_extensions) {
            if (std::find(m_instance_extensions.begin(), m_instance_extensions.end(), extension) == m_instance_extensions.end()) {
                VkUtil::ExitFatal("Enabled instance extension '" + std::string(extension) + "' is not supported!", -1);
                return false;
            }
        }
    }
    return true;
}

#pragma endregion
