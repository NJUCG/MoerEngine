#include "config.h"

#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "misc/MacroUtils.h"

#include "rhi/vulkan/VulkanRHI.h"
#include "rhi/vulkan/VulkanCommandList.h"

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
    vk_sampler->GenerateSamplerFromInitializer(m_device, _initializer);

    return RHISamplerRef(vk_sampler);
}

RHIRasterizationStateRef VulkanRHIImpl::RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) {
    VulkanRHIRasterizationState* vk_rasterization_state = new VulkanRHIRasterizationState();
    vk_rasterization_state->GenerateRasterizationStateFromInitializer(_init);

    return RHIRasterizationStateRef(vk_rasterization_state);
}

RHIDepthStencilStateRef VulkanRHIImpl::RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) {
    VulkanRHIDepthStencilState* vk_depth_stencil_state = new VulkanRHIDepthStencilState();
    vk_depth_stencil_state->GenerateDepthStencilStateFromInitializer(_init);

    return RHIDepthStencilStateRef(vk_depth_stencil_state);
}

RHIMultisampleStateRef VulkanRHIImpl::RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) {
    VulkanRHIMultisampleState* vk_multisample_state = new VulkanRHIMultisampleState();
    vk_multisample_state->GenerateMultisampleStateFromInitializer(_init);

    return RHIMultisampleStateRef(vk_multisample_state);
}

RHIBlendStateRef VulkanRHIImpl::RHICreateBlendState(const RHIBlendStateInitializer& _init) {
    VulkanRHIBlendState* vk_blend_state = new VulkanRHIBlendState();
    vk_blend_state->GenerateBlendStateFromInitializer(_init);

    return RHIBlendStateRef(vk_blend_state);
}

RHIVertexInputStateRef VulkanRHIImpl::RHICreateVertexInputState(const VertexInputStateInitializerList& _init) {
    VulkanRHIVertexInputState* vk_input_state = new VulkanRHIVertexInputState();
    vk_input_state->GenerateVertexInputStateFromInitializer(_init);

    return RHIVertexInputStateRef(vk_input_state);
}

RHIVertexShaderRef   VulkanRHIImpl::RHICreateVertexShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIVertexShaderRef{}; }
RHIFragmentShaderRef VulkanRHIImpl::RHICreateFragmentShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIFragmentShaderRef{}; }
RHIGeometryShaderRef VulkanRHIImpl::RHICreateGeometryShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIGeometryShaderRef{}; }

RHIMeshShaderRef          VulkanRHIImpl::RHICreateMeshShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIMeshShaderRef{}; }
RHIAmplificationShaderRef VulkanRHIImpl::RHICreateAmplificationShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIAmplificationShaderRef{}; }

RHIComputeShaderRef VulkanRHIImpl::RHICreateComputeShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) { return RHIComputeShaderRef{}; }

RHIShaderLibraryRef VulkanRHIImpl::RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) { return RHIShaderLibraryRef{}; }

RHIFenceRef VulkanRHIImpl::RHICreateFence(const std::string& name) {
    VulkanRHIFence* vk_fence = new VulkanRHIFence(name, m_device);

    return RHIFenceRef(vk_fence);
}

/* create cpu visible buffer for direct data transfer */
RHIStagingBufferRef VulkanRHIImpl::RHICreateStagingBuffer() {
    // MARK...
    uint64_t byte_size = 64 * 1024;

    VulkanRHIStagingBuffer* vk_staging_buffer = new VulkanRHIStagingBuffer(byte_size);

    VkBufferCreateInfo buffer_create_info{};
    buffer_create_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.pNext       = nullptr;
    buffer_create_info.flags       = 0;
    buffer_create_info.size        = byte_size;
    buffer_create_info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    vmaCreateBuffer(m_allocator, &buffer_create_info, &alloc_create_info, &vk_staging_buffer->m_alloc.buffer, &vk_staging_buffer->m_alloc.alloc, nullptr);

    void* p_data;
    vmaMapMemory(m_allocator, vk_staging_buffer->m_alloc.alloc, &p_data);
    vk_staging_buffer->m_head_ptr = reinterpret_cast<uint8_t*>(p_data);
    vk_staging_buffer->m_tail_ptr = reinterpret_cast<uint8_t*>(p_data) + byte_size;
    vk_staging_buffer->m_cur_ptr  = vk_staging_buffer->m_head_ptr;

    return RHIStagingBufferRef(vk_staging_buffer);
}

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

void VulkanRHIImpl::RHIUploadBuffer(RHIBufferRef _buffer_ref, const uint8_t* _data, uint32_t _size) {
    auto* staging_buffer = dynamic_cast<VulkanRHIStagingBuffer*>(_buffer_ref.Get());
    if (staging_buffer == nullptr) {
        LOG_CRITICAL("Invalid RHIBufferRef type: {}.", typeid(_buffer_ref).name());
        return;
    }

    void* p_data_cur = staging_buffer->GetSuballocationFromBuffer(_size);
    memcpy(p_data_cur, _data, _size);
    staging_buffer->m_cur_ptr = p_data_cur;
}

void VulkanRHIImpl::RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) {
    LOG_WARNING("VulkanRHI::RHICopyBuffer is not implemented.");
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
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

    VK_CHECK_RESULT(vmaCreateBuffer(m_allocator, &buffer_create_info, &alloc_create_info, &vk_buffer->m_alloc.buffer, &vk_buffer->m_alloc.alloc, nullptr));

    return RHIBufferRef(vk_buffer);
}

void* VulkanRHIImpl::RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) {
    auto* vk_buffer = dynamic_cast<VulkanRHIBuffer*>(_buffer);
    if (vk_buffer == nullptr) {
        LOG_CRITICAL("Invalid RHIBuffer type: {}.", typeid(*_buffer).name());
        return nullptr;
    }

    void* p_data;
    VK_CHECK_RESULT(vmaMapMemory(m_allocator, vk_buffer->m_alloc.alloc, &p_data));

    return p_data;
}
void VulkanRHIImpl::RHIUnmapBuffer(RHIBuffer* _buffer) {
    auto* vk_buffer = dynamic_cast<VulkanRHIBuffer*>(_buffer);
    if (vk_buffer == nullptr) {
        LOG_CRITICAL("Invalid RHIBuffer type: {}.", typeid(*_buffer).name());
        return;
    }

    vmaUnmapMemory(m_allocator, vk_buffer->m_alloc.alloc);
}

RHITextureRef VulkanRHIImpl::RHICreateTexture(const RHITextureCreateInfo& info) {
    VulkanRHITexture* vk_texture = new VulkanRHITexture(info);

    VkImageCreateInfo image_create_info{};
    image_create_info.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext                 = nullptr;
    image_create_info.flags                 = 0;
    image_create_info.imageType             = VulkanRHITexture::METoVKImageType(info.dimension);
    image_create_info.format                = VkFormat(info.format);
    image_create_info.extent.width          = info.extent.x;
    image_create_info.extent.height         = info.extent.y;
    image_create_info.extent.depth          = info.depth;
    image_create_info.mipLevels             = info.num_mips;
    image_create_info.arrayLayers           = info.array_size;
    image_create_info.samples               = VulkanEnumTranslator::METoVKSampleCountFlagBits(info.num_samples);
    image_create_info.tiling                = VK_IMAGE_TILING_OPTIMAL;// MARK...
    image_create_info.usage                 = VulkanRHITexture::METoVKImageUsageFlags(info.usage);
    image_create_info.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.queueFamilyIndexCount = 0;
    image_create_info.pQueueFamilyIndices   = nullptr;
    image_create_info.initialLayout         = VulkanEnumTranslator::METoVKImageLayout(info.layout);

    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

    VK_CHECK_RESULT(vmaCreateImage(m_allocator, &image_create_info, &alloc_create_info, &vk_texture->m_alloc.image, &vk_texture->m_alloc.alloc, nullptr));

    return RHITextureRef(vk_texture);
};

RHIShaderResourceViewRef VulkanRHIImpl::RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {
    VulkanRHIShaderResourceView* vk_srv = new VulkanRHIShaderResourceView(_resource, _view_info);

    VkImageViewCreateInfo image_view_create_info{};
    image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_create_info.pNext = nullptr;
    image_view_create_info.flags = 0;

    auto* vk_texture = dynamic_cast<VulkanRHITexture*>(_resource);
    if (vk_texture == nullptr) {
        LOG_CRITICAL("Invalid RHIViewableResource input: {}", static_cast<void*>(_resource));
        return nullptr;
    }

    image_view_create_info.image                           = vk_texture->GetHandle();
    image_view_create_info.viewType                        = VulkanEnumTranslator::METoVKImageViewType(_view_info.texture.srv.dimension);
    image_view_create_info.format                          = VkFormat(_view_info.texture.srv.format);
    image_view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    image_view_create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;// MARK...
    image_view_create_info.subresourceRange.baseMipLevel   = _view_info.texture.srv.mip_min;
    image_view_create_info.subresourceRange.levelCount     = _view_info.texture.srv.mip_num;
    image_view_create_info.subresourceRange.baseArrayLayer = _view_info.texture.srv.array_min;
    image_view_create_info.subresourceRange.layerCount     = _view_info.texture.srv.array_num;

    VK_CHECK_RESULT(vkCreateImageView(*m_device, &image_view_create_info, nullptr, &vk_srv->m_view));

    return RHIShaderResourceViewRef(vk_srv);
}

RHIUnorderedAccessViewRef VulkanRHIImpl::RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {
    VulkanRHIUnorderedAccessView* vk_uav = new VulkanRHIUnorderedAccessView(_resource, _view_info);

    VkImageViewCreateInfo image_view_create_info{};
    image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_create_info.pNext = nullptr;
    image_view_create_info.flags = 0;

    auto* vk_texture = dynamic_cast<VulkanRHITexture*>(_resource);
    if (vk_texture == nullptr) {
        LOG_CRITICAL("Invalid RHIViewableResource input: {}", typeid(*_resource).name());
        return nullptr;
    }

    image_view_create_info.image                           = vk_texture->GetHandle();
    image_view_create_info.viewType                        = VulkanEnumTranslator::METoVKImageViewType(_view_info.texture.uav.dimension);
    image_view_create_info.format                          = VkFormat(_view_info.texture.uav.format);
    image_view_create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    image_view_create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;// MARK...
    image_view_create_info.subresourceRange.baseMipLevel   = _view_info.texture.uav.mip_min;
    image_view_create_info.subresourceRange.levelCount     = _view_info.texture.uav.mip_num;
    image_view_create_info.subresourceRange.baseArrayLayer = _view_info.texture.uav.array_min;
    image_view_create_info.subresourceRange.layerCount     = _view_info.texture.uav.array_num;

    VK_CHECK_RESULT(vkCreateImageView(*m_device, &image_view_create_info, nullptr, &vk_uav->m_view));

    return RHIUnorderedAccessViewRef(vk_uav);
}

RHICommandQueue* VulkanRHIImpl::CreateCommandQueue(ECommandQueueType type) {
    return nullptr;
}

RHIGraphicsCommandList* VulkanRHIImpl::CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state) {
    return new VulkanRHIGraphicsCommandList(m_device, m_device->GetDefaultCommandPool(), VK_COMMAND_BUFFER_LEVEL_PRIMARY);
}

RHIComputeCommandList* VulkanRHIImpl::CreateComputeCommandList(RHIComputePipelineState* _initial_state) {
    return nullptr;
}

#pragma endregion

void VulkanRHIImpl::InitSurface(GLFWwindow* _window) {
    VK_CHECK_RESULT(glfwCreateWindowSurface(m_instance, _window, nullptr, &m_surface));
}

void VulkanRHIImpl::InitVulkan() {
    DeviceInitializer initializer;
    initializer.instance           = m_instance;
    initializer.surface            = m_surface;
    initializer.api_version        = VK_API_VERSION_1_3;
    initializer.enabled_features   = VulkanDeviceFeature::GetMESupportedDeviceFeatures(initializer.api_version);
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
