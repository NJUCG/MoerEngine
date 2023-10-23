//
// Created by 74535 on 2023/10/2.
//

#ifndef IVULKAN_RHI_H
#define IVULKAN_RHI_H

#include "rhi/RHI.h"
#include "rhi/RHIResource.h"
#include <vulkan.h>

class IVulkanRHI : public RHI {
public:
    void        Initialize() override {}
    void        ShutDown() override {}
    const char* GetName() override { return "VulkanRHI Interface"; }

#pragma region resources creation
    RHISamplerRef            RHICreateSampler(const RHISamplerInitializer& _initializer) override { return RHISamplerRef{}; }
    RHIRasterizationStateRef RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) override { return RHIRasterizationStateRef{}; }
    RHIDepthStencilStateRef  RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) override { return RHIDepthStencilStateRef{}; }
    RHIMultisampleStateRef   RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) override { return RHIMultisampleStateRef{}; }
    RHIBlendStateRef         RHICreateBlendState(const RHIBlendStateInitializer& _init) override { return RHIBlendStateRef{}; }
    RHIVertexInputStateRef   RHICreateVertexInputState(const VertexInputStateInitializerList& _init) override { return RHIVertexInputStateRef{}; }

    RHIVertexShaderRef   RHICreateVertexShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override { return RHIVertexShaderRef{}; }
    RHIFragmentShaderRef RHICreateFragmentShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override { return RHIFragmentShaderRef{}; }
    RHIGeometryShaderRef RHICreateGeometryShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override { return RHIGeometryShaderRef{}; }

    RHIMeshShaderRef          RHICreateMeshShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override { return RHIMeshShaderRef{}; }
    RHIAmplificationShaderRef RHICreateAmplificationShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override { return RHIAmplificationShaderRef{}; }

    RHIComputeShaderRef RHICreateComputeShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) override { return RHIComputeShaderRef{}; }

    RHIShaderLibraryRef RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) override { return RHIShaderLibraryRef{}; }

    RHIFenceRef RHICreateFence(const std::string& name) override { return RHIFenceRef{}; }

    RHIShaderBoundStateRef RHICreateShaderBoundStage(
        RHIVertexInputState* _vertex_input,
        RHIVertexShader*     _vertex_shader,
        RHIFragmentShader*   _fragment_shader,
        RHIGeometryShader*   _geometry_shader) override { return RHIShaderBoundStateRef{}; }

    RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) override { return RHIGraphicsPipelineStateRef{}; }

    RHIComputePipelineStateRef RHICreateComputePipelineState(RHIComputeShader* _compute_shader) override { return RHIComputePipelineStateRef{}; }

    void         RHIUploadBuffer(RHIBufferRef _buffer_ref, const uint8_t* _data, uint32_t _size) override{};
    void         RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) override {}
    RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info) override { return RHIBufferRef{}; }
    void*        RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) override { return nullptr; }
    void         RHIUnmapBuffer(RHIBuffer* _buffer) override {}

    RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) override { return RHITextureRef{}; };

    RHIShaderResourceViewRef  RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override { return RHIShaderResourceViewRef{}; }
    RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override { return RHIUnorderedAccessViewRef{}; }

    RHICommandQueue*        CreateCommandQueue(ECommandQueueType type) override { return nullptr; }
    RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr) override { return nullptr; }
    RHIComputeCommandList*  CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr) override { return nullptr; }

#pragma endregion
};

#endif// IVULKAN_RHI_H
