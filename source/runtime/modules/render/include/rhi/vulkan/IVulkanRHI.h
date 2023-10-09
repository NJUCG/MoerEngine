//
// Created by 74535 on 2023/10/2.
//

#ifndef IVULKAN_RHI_H
#define IVULKAN_RHI_H

#include "RHI.h"

#ifdef _WIN32
#include "./windows/VulkanWindowsPlatform.h"
#include <windows.h>
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
#include "./linux/VulkanLinuxPlatform.h"
#endif

#include <vulkan.h>

class IVulkanRHI : public RHI {
public:
    void        Initialize() override {}
    void        ShutDown() override {}
    const char* GetName() override { return "VulkanRHI Interface"; }

#pragma region resources creation
    RHISamplerRef            RHICreateSampler(const RHISamplerInitializer& _initializer) override {}
    RHIRasterizationStateRef RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) override {}
    RHIDepthStencilStateRef  RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) override {}
    RHIMultisampleStateRef   RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) override {}
    RHIBlendStateRef         RHICreateBlendState(const RHIBlendStateInitializer& _init) override {}
    RHIVertexInputStateRef   RHICreateVertexInputState(const VertexInputStateInitializerList& _init) override {}

    RHIVertexShaderRef   RHICreateVertexShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override {}
    RHIFragmentShaderRef RHICreateFragmentShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override {}
    RHIGeometryShaderRef RHICreateGeometryShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override {}

    RHIMeshShaderRef          RHICreateMeshShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override {}
    RHIAmplificationShaderRef RHICreateAmplificationShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override {}

    RHIComputeShaderRef RHICreateComputeShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override {}

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

#endif// IVULKAN_RHI_H
