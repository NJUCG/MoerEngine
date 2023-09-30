#ifndef VULKAN_RHI_H
#define VULKAN_RHI_H
#include <string>
#include "rhi/RHI.h"
#include <vulkan.h>
class IVulkanRHI : public RHI {
public:
    void Initialize() override {}
    void ShutDown() override {}
    const char* GetName() override { return "VulkanRHI"; }

#pragma region resources creation
    RHISamplerRef RHICreateSampler(const RHISamplerInitializer& _initializer) override {}
    RHIRasterizationStateRef RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) override {}
    RHIDepthStencilStateRef RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) override {}
    RHIMultisampleStateRef RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) override {}
    RHIBlendStateRef RHICreateBlendState(const RHIBlendStateInitializer& _init) override {}
    RHIVertexInputStateRef RHICreateVertexInputState(const VertexInputStateInitializerList& _init) override {}

    RHIVertexShaderRef RHICreateVertexShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override {}
    RHIFragmentShaderRef RHICreateFragmentShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override {}
    RHIGeometryShaderRef RHICreateGeometryShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override {}

    RHIMeshShaderRef RHICreateMeshShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override {}
    RHIAmplificationShaderRef RHICreateAmplificationShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override {}

    RHIComputeShaderRef RHICreateComputeShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override {}

    RHIShaderLibraryRef RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) override {}

    RHIFenceRef RHICreateFence(const std::string& name) override {}

    /* create cpu visible buffer for direct data transfer */
    RHIStagingBufferRef RHICreateStagingBuffer() override {}

    RHIShaderBoundStateRef RHICreateShaderBoundStage(
        RHIVertexInputState* _vertex_input,
        RHIVertexShader*     _vertex_shader,
        RHIFragmentShader*   _fragment_shader,
        RHIGeometryShader*   _geometry_shader) override {}

    RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) override {}

    RHIComputePipelineStateRef RHICreateComputePipelineState(RHIComputeShader* _compute_shader) override {}

    RHIUniformBufferRef RHICreateUniformBuffer(const void* data, const RHIUniformBufferLayout* layout, EBufferUsageFlags _usage) override {}

    void RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) override {}

    RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info) override {}

    RHIShaderResourceViewRef  RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override {}
    RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override {}

#pragma endregion
};

class VulkanRHIImpl : IVulkanRHI {
public:

    /**
     * @brief Setup the vulkan instance, enable required extensions and connect to the physical device (GPU).
     */
    void Initialize() override;

    /**
     * @brief Clean up the vulkan instance.
     */
    void ShutDown() override;

    /**
     * @brief Get the name of the RHI.
     * @return The name of the RHI
     */
    const char* GetName() override;

#pragma region resources creation
    RHISamplerRef RHICreateSampler(const RHISamplerInitializer& _initializer) override;
    RHIRasterizationStateRef RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) override;
    RHIDepthStencilStateRef RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) override;
    RHIMultisampleStateRef RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) override;
    RHIBlendStateRef RHICreateBlendState(const RHIBlendStateInitializer& _init) override;
    RHIVertexInputStateRef RHICreateVertexInputState(const VertexInputStateInitializerList& _init) override;

    RHIVertexShaderRef RHICreateVertexShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override;
    RHIFragmentShaderRef RHICreateFragmentShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override;
    RHIGeometryShaderRef RHICreateGeometryShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override;

    RHIMeshShaderRef RHICreateMeshShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override;
    RHIAmplificationShaderRef RHICreateAmplificationShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override;

    RHIComputeShaderRef RHICreateComputeShader(std::vector<const uint8_t> _code, const SHA256Hash& _hash) override;

    RHIShaderLibraryRef RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) override;

    RHIFenceRef RHICreateFence(const std::string& name) override;

    /* create cpu visible buffer for direct data transfer */
    RHIStagingBufferRef RHICreateStagingBuffer() override;

    RHIShaderBoundStateRef RHICreateShaderBoundStage(
        RHIVertexInputState* _vertex_input,
        RHIVertexShader*     _vertex_shader,
        RHIFragmentShader*   _fragment_shader,
        RHIGeometryShader*   _geometry_shader) override;

    RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) override;

    RHIComputePipelineStateRef RHICreateComputePipelineState(RHIComputeShader* _compute_shader) override;

    RHIUniformBufferRef RHICreateUniformBuffer(const void* data, const RHIUniformBufferLayout* layout, EBufferUsageFlags _usage) override;

    void RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) override;

    RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info) override;

    RHIShaderResourceViewRef RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override;
    RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override;
#pragma endregion

private:
    VkInstance m_instance;
    VkPhysicalDevice m_physical_device;

#pragma region helper functions
private:
    VkResult CreateInstance(bool _enable_validation);

#pragma endregion
};
#endif// !VULKAN_RHI_H
