#include "misc/MacroUtils.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "vulkan/config.h"

#include <string>
#include <vulkan.h>

const std::string vk_layer = MACRO_STR(__ENGINE_NAME__)MACRO_STR(_VK_LAYER_PATH);

void VulkanRHIImpl::Initialize() {

}

void VulkanRHIImpl::ShutDown() {}

const char* VulkanRHIImpl::GetName() {
    return "VulkanRHIImplementation";
}

#pragma region resources creation
RHISamplerRef VulkanRHIImpl::RHICreateSampler(const RHISamplerInitializer& _initializer) {}
RHIRasterizationStateRef VulkanRHIImpl::RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) {}
RHIDepthStencilStateRef VulkanRHIImpl::RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) {}
RHIMultisampleStateRef VulkanRHIImpl::RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) {}
RHIBlendStateRef VulkanRHIImpl::RHICreateBlendState(const RHIBlendStateInitializer& _init) {}
RHIVertexInputStateRef VulkanRHIImpl::RHICreateVertexInputState(const VertexInputStateInitializerList& _init) {}

RHIVertexShaderRef VulkanRHIImpl::RHICreateVertexShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) {}
RHIFragmentShaderRef VulkanRHIImpl::RHICreateFragmentShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) {}
RHIGeometryShaderRef VulkanRHIImpl::RHICreateGeometryShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) {}

RHIMeshShaderRef VulkanRHIImpl::RHICreateMeshShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) {}
RHIAmplificationShaderRef VulkanRHIImpl::RHICreateAmplificationShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) {}

RHIComputeShaderRef VulkanRHIImpl::RHICreateComputeShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) {}

RHIShaderLibraryRef VulkanRHIImpl::RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) {}

RHIFenceRef VulkanRHIImpl::RHICreateFence(const std::string& name) {}

/* create cpu visible buffer for direct data transfer */
RHIStagingBufferRef VulkanRHIImpl::RHICreateStagingBuffer() {}

RHIShaderBoundStateRef VulkanRHIImpl::RHICreateShaderBoundStage(
    RHIVertexInputState* _vertex_input,
    RHIVertexShader* _vertex_shader,
    RHIFragmentShader* _fragment_shader,
    RHIGeometryShader* _geometry_shader) {}

RHIGraphicsPipelineStateRef VulkanRHIImpl::RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) {}

RHIComputePipelineStateRef VulkanRHIImpl::RHICreateComputePipelineState(RHIComputeShader* _compute_shader) {}

RHIUniformBufferRef  VulkanRHIImpl::RHICreateUniformBuffer(const void* data, const RHIUniformBufferLayout* layout, EBufferUsageFlags _usage) {}

void VulkanRHIImpl::RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) {}

RHIBufferRef VulkanRHIImpl::RHICreateBuffer(const RHIBufferCreateInfo& info) {}

RHIShaderResourceViewRef VulkanRHIImpl::RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {}
RHIUnorderedAccessViewRef VulkanRHIImpl::RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) {}
#pragma endregion

#pragma region helper functions
VkResult VulkanRHIImpl::CreateInstance(bool _enable_validation) {
    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // applicationInfo.pApplicationName = MACRO_STR(__ENGINE_NAME__);
    // applicationInfo.applicationVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    applicationInfo.pEngineName = MACRO_STR(__ENGINE_NAME__);
    applicationInfo.engineVersion = VK_MAKE_VERSION(PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    applicationInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceCreateInfo{};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pApplicationInfo = &applicationInfo;

}

#pragma endregion



