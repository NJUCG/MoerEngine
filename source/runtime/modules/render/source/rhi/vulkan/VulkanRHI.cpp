#include "config.h"

#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "misc/MacroUtils.h"

#include "rhi/vulkan/VulkanRHI.h"
#include "VulkanRHIResource.h"
#include "VulkanRHIInitializer.h"
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
    InitVulkanMemoryAllocator();
}

void VulkanRHIImpl::ShutDown() {
    delete m_swap_chain;
    delete m_device;
}

#pragma region resources creation
RHISamplerRef  VulkanRHIImpl::RHICreateSampler(const RHISamplerInitializer& _initializer) {
    VulkanRHISampler* vk_sampler = new VulkanRHISampler();

    auto sampler_create_info = VulkanRHISamplerInitializer::FromRHISamplerInitializer(m_device, _initializer);

    VK_CHECK_RESULT(vkCreateSampler(*m_device, &sampler_create_info, nullptr, &vk_sampler->m_sampler));

    return RHISamplerRef(vk_sampler);
}

RHIRasterizationStateRef VulkanRHIImpl::RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) {
    VkPipelineRasterizationStateCreateInfo rasterization_state_create_info{};
    rasterization_state_create_info.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state_create_info.pNext                   = nullptr;
    rasterization_state_create_info.flags                   = 0;
    rasterization_state_create_info.depthClampEnable        = _init.b_depth_clamp_enable ? VK_TRUE : VK_FALSE;
    rasterization_state_create_info.rasterizerDiscardEnable = VK_FALSE;// MARK...
    rasterization_state_create_info.polygonMode             = VulkanRHIRasterizationState::METoVKPolygonMode(_init.fill_mode);
    rasterization_state_create_info.cullMode                = VulkanRHIRasterizationState::METoVKCullModeFlags(_init.cull_mode);
    rasterization_state_create_info.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;// MARK...
    rasterization_state_create_info.depthBiasEnable         = _init.b_depth_bias ? VK_TRUE : VK_FALSE;
    rasterization_state_create_info.depthBiasConstantFactor = _init.depth_bias;
    rasterization_state_create_info.depthBiasClamp          = _init.depth_bias_clamp;
    rasterization_state_create_info.depthBiasSlopeFactor    = _init.depth_bias_slop_factor;
    rasterization_state_create_info.lineWidth               = 1.0f;

    VulkanRHIRasterizationState* vk_rasterization_state = new VulkanRHIRasterizationState(rasterization_state_create_info);

    return RHIRasterizationStateRef(vk_rasterization_state);
}

RHIDepthStencilStateRef VulkanRHIImpl::RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) {
    VkPipelineDepthStencilStateCreateInfo depth_stencil_state_create_info{};
    depth_stencil_state_create_info.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_state_create_info.pNext                 = nullptr;
    depth_stencil_state_create_info.flags                 = 0;
    depth_stencil_state_create_info.depthTestEnable       = (_init.b_enable_depth_write || _init.depth_test_op != ECompareOption::CO_ALWAYS) ? VK_TRUE : VK_FALSE;
    depth_stencil_state_create_info.depthWriteEnable      = _init.b_enable_depth_write;
    depth_stencil_state_create_info.depthCompareOp        = VulkanRHIDepthStencilState::METoVKCompareOp(_init.depth_test_op);
    depth_stencil_state_create_info.depthBoundsTestEnable = VK_FALSE;// MARK...
    depth_stencil_state_create_info.minDepthBounds        = 0.0f;
    depth_stencil_state_create_info.maxDepthBounds        = 1.0f;

    depth_stencil_state_create_info.stencilTestEnable = (_init.b_enable_front_face_stencil || _init.b_enable_back_face_stencil) ? VK_TRUE : VK_FALSE;
    depth_stencil_state_create_info.front.failOp      = VulkanRHIDepthStencilState::METoVKStencilOp(_init.front_face_stencil_fail_stencilOp);
    depth_stencil_state_create_info.front.passOp      = VulkanRHIDepthStencilState::METoVKStencilOp(_init.front_face_pass_stencil_op);
    depth_stencil_state_create_info.front.depthFailOp = VulkanRHIDepthStencilState::METoVKStencilOp(_init.front_face_depth_fail_stencilOp);
    depth_stencil_state_create_info.front.compareOp   = VulkanRHIDepthStencilState::METoVKCompareOp(_init.front_face_stencil_test);
    depth_stencil_state_create_info.front.compareMask = _init.stencil_readmask;
    depth_stencil_state_create_info.front.writeMask   = _init.stencil_writemask;
    depth_stencil_state_create_info.front.reference   = 0;

    if (_init.b_enable_back_face_stencil) {
        depth_stencil_state_create_info.back.failOp      = VulkanRHIDepthStencilState::METoVKStencilOp(_init.back_face_stencil_fail_stencil_op);
        depth_stencil_state_create_info.back.passOp      = VulkanRHIDepthStencilState::METoVKStencilOp(_init.back_face_pass_stencil_op);
        depth_stencil_state_create_info.back.depthFailOp = VulkanRHIDepthStencilState::METoVKStencilOp(_init.back_face_depth_fail_stencil_op);
        depth_stencil_state_create_info.back.compareOp   = VulkanRHIDepthStencilState::METoVKCompareOp(_init.back_face_stencil_test);
        depth_stencil_state_create_info.back.compareMask = _init.stencil_readmask;
        depth_stencil_state_create_info.back.writeMask   = _init.stencil_writemask;
        depth_stencil_state_create_info.back.reference   = 0;
    } else {
        depth_stencil_state_create_info.front = depth_stencil_state_create_info.back;
    }

    VulkanRHIDepthStencilState* vk_depth_stencil_state = new VulkanRHIDepthStencilState(depth_stencil_state_create_info);

    return RHIDepthStencilStateRef(vk_depth_stencil_state);
}

RHIMultisampleStateRef VulkanRHIImpl::RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) {
    VkPipelineMultisampleStateCreateInfo multisample_state_create_info{};
    multisample_state_create_info.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state_create_info.pNext                 = nullptr;
    multisample_state_create_info.flags                 = 0;
    multisample_state_create_info.rasterizationSamples  = VulkanRHIMultisampleState::METoVKSampleCountFlagBits(_init.sample_count);
    multisample_state_create_info.sampleShadingEnable   = _init.b_sample_shading;
    multisample_state_create_info.minSampleShading      = _init.min_sample_shading;
    multisample_state_create_info.pSampleMask           = nullptr;// MARK...
    multisample_state_create_info.alphaToCoverageEnable = _init.b_alpha_to_converge;
    multisample_state_create_info.alphaToOneEnable      = _init.b_alpha_to_one;

    VulkanRHIMultisampleState* vk_multisample_state = new VulkanRHIMultisampleState(multisample_state_create_info);

    return RHIMultisampleStateRef(vk_multisample_state);
}

RHIBlendStateRef VulkanRHIImpl::RHICreateBlendState(const RHIBlendStateInitializer& _init) { return RHIBlendStateRef{}; }

RHIVertexInputStateRef VulkanRHIImpl::RHICreateVertexInputState(const VertexInputStateInitializerList& _init) {
    const uint32_t descriptor_count = _init.size();

    std::vector<VkVertexInputBindingDescription>   bindings(descriptor_count);
    std::vector<VkVertexInputAttributeDescription> attributes(descriptor_count);// MARK...
    for (uint32_t i = 0; i < descriptor_count; i++) {
        bindings[i].binding    = _init[i].binding_index;
        bindings[i].stride     = _init[i].stride;
        bindings[i].inputRate  = VulkanRHIVertexInputState::METoVKVertexInputRate(_init[i].input_rate);
        attributes[i].location = _init[i].attribute_index;
        attributes[i].binding  = _init[i].binding_index;
        attributes[i].format   = VulkanRHIVertexInputState::METoVKFormat(_init[i].type);
        attributes[i].offset   = _init[i].offset;
    }

    VkPipelineVertexInputStateCreateInfo input_state_create_info{};
    input_state_create_info.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input_state_create_info.pNext                           = nullptr;
    input_state_create_info.flags                           = 0;
    input_state_create_info.vertexBindingDescriptionCount   = descriptor_count;
    input_state_create_info.pVertexBindingDescriptions      = bindings.data();
    input_state_create_info.vertexAttributeDescriptionCount = descriptor_count;
    input_state_create_info.pVertexAttributeDescriptions    = attributes.data();

    VulkanRHIVertexInputState* vk_input_state = new VulkanRHIVertexInputState(input_state_create_info);

    return RHIVertexInputStateRef(vk_input_state);
}

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

RHIGraphicsPipelineStateRef VulkanRHIImpl::RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) {
    VulkanRHIGraphicsPipelineState* vk_pipeline = new VulkanRHIGraphicsPipelineState();

    return RHIGraphicsPipelineStateRef(vk_pipeline);
}

RHIComputePipelineStateRef VulkanRHIImpl::RHICreateComputePipelineState(RHIComputeShader* _compute_shader) { return RHIComputePipelineStateRef{}; }

RHIGlobalBufferRef VulkanRHIImpl::RHICreateUniformBuffer(const void* data, const RHIGlobalBufferLayout* layout, EBufferUsageFlags _usage) { return RHIGlobalBufferRef{}; }

void VulkanRHIImpl::RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) {
}

RHIBufferRef VulkanRHIImpl::RHICreateBuffer(const RHIBufferCreateInfo& info) {
    RHIBufferInfo buffer_info{};
    buffer_info.size   = info.size;
    buffer_info.stride = info.stride;
    buffer_info.usage  = info.usage;

    VulkanRHIBuffer* vk_buffer = new VulkanRHIBuffer(buffer_info);

    auto indices = m_device->GetQueueFamilyIndices();

    std::array<uint32_t, 2> allowed_queue_indices = {indices.graphics.value(), indices.transfer.value()};
    VkBufferCreateInfo      buffer_create_info{};
    buffer_create_info.sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.pNext                 = nullptr;
    buffer_create_info.flags                 = 0;
    buffer_create_info.size                  = info.size;
    buffer_create_info.usage                 = VulkanRHIBuffer::METoVKBufferUsageFlags(m_device, info.usage);
    buffer_create_info.sharingMode           = VK_SHARING_MODE_CONCURRENT;
    buffer_create_info.queueFamilyIndexCount = allowed_queue_indices.size();
    buffer_create_info.pQueueFamilyIndices   = allowed_queue_indices.data();

    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage(info.usage);

    VK_CHECK_RESULT(vmaCreateBuffer(m_allocator, &buffer_create_info, &alloc_create_info, &vk_buffer->m_alloc.buffer, &vk_buffer->m_alloc.alloc, nullptr));

    return RHIBufferRef(vk_buffer);
}

void* VulkanRHIImpl::RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) { return nullptr; }
void  VulkanRHIImpl::RHIUnmapBuffer(RHIBuffer* _buffer) {}

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

    m_device = new VulkanDevice();
    m_device->Init(initializer);

    m_swap_chain = new VulkanSwapChain();
    m_swap_chain->Connect(m_instance, m_surface, m_device);
    uint32_t width, height;
    // glfwGetFramebufferSize(m_window, &width, &height);
    m_swap_chain->Init(&width, &height, true);
}

void VulkanRHIImpl::InitVulkanMemoryAllocator() {
    VmaAllocatorCreateInfo alloc_create_info{};

    alloc_create_info.vulkanApiVersion = VK_API_VERSION_1_3;
    alloc_create_info.instance         = m_instance;
    alloc_create_info.physicalDevice   = m_device->GetGpu();
    alloc_create_info.device           = m_device->GetDevice();

    VK_CHECK_RESULT(vmaCreateAllocator(&alloc_create_info, &m_allocator));
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
