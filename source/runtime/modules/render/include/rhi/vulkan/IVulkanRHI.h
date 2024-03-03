//
// Created by 74535 on 2023/10/2.
//

#ifndef IVULKAN_RHI_H
#define IVULKAN_RHI_H

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include <cstdint>
#include <vulkan/vulkan.h>

class IVulkanRHI : public RHI {
public:
    IVulkanRHI() : RHI(ERHIType::Vulkan) {}

    void        Initialize(const RHIInitInfo& _init) override {}
    void        PostInit() override {}
    void        ShutDown() override {}
    const char* GetName() override { return "VulkanRHI Interface"; }

#pragma region resources creation
    RHISamplerRef            RHICreateSampler(const RHISamplerInitializer& _initializer) override { return RHISamplerRef{}; }
    RHIRasterizationStateRef RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) override { return RHIRasterizationStateRef{}; }
    RHIDepthStencilStateRef  RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init) override { return RHIDepthStencilStateRef{}; }
    RHIMultisampleStateRef   RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init) override { return RHIMultisampleStateRef{}; }
    RHIBlendStateRef         RHICreateBlendState(const RHIBlendStateInitializer& _init) override { return RHIBlendStateRef{}; }
    RHIVertexInputStateRef   RHICreateVertexInputState(const VertexInputStateInitializerList& _init) override { return RHIVertexInputStateRef{}; }

    RHIVertexShaderRef   RHICreateVertexShader(const class ShaderCodeEntry*, const Shader*) override { return RHIVertexShaderRef{}; }
    RHIFragmentShaderRef RHICreateFragmentShader(const class ShaderCodeEntry*, const Shader*) override { return RHIFragmentShaderRef{}; }
    RHIGeometryShaderRef RHICreateGeometryShader(const class ShaderCodeEntry*, const Shader*) override { return RHIGeometryShaderRef{}; }

    RHIMeshShaderRef          RHICreateMeshShader(const class ShaderCodeEntry*, const Shader*) override { return RHIMeshShaderRef{}; }
    RHIAmplificationShaderRef RHICreateAmplificationShader(const class ShaderCodeEntry*, const Shader*) override { return RHIAmplificationShaderRef{}; }

    RHIComputeShaderRef RHICreateComputeShader(const class ShaderCodeEntry*, const Shader*) override { return RHIComputeShaderRef{}; }

    RHIRayGenShaderRef          RHICreateRayGenShader(const class ShaderCodeEntry*, const Shader*) override { return RHIRayGenShaderRef{}; }
    RHIRayMissShaderRef         RHICreateRayMissShader(const class ShaderCodeEntry*, const Shader*) override { return RHIRayMissShaderRef{}; }
    RHIRayClosestHitShaderRef   RHICreateRayClosestHitShader(const class ShaderCodeEntry*, const Shader*) override { return RHIRayClosestHitShaderRef{}; }
    RHIRayCallableShaderRef     RHICreateRayCallableShader(const class ShaderCodeEntry*, const Shader*) override { return RHIRayCallableShaderRef{}; }
    RHIRayIntersectionShaderRef RHICreateRayIntersectionShader(const class ShaderCodeEntry*, const Shader*) override { return RHIRayIntersectionShaderRef{}; }
    RHIRayAnyhitShaderRef       RHICreateRayAnyhitShader(const class ShaderCodeEntry*, const Shader*) override { return RHIRayAnyhitShaderRef{}; }

    RHIShaderLibraryRef RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) override { return RHIShaderLibraryRef{}; }

    RHIFenceRef RHICreateFence(const RHIFenceCreateInfo&) override { return RHIFenceRef{}; }

    RHIShaderBoundStateRef RHICreateShaderBoundStage(
        RHIVertexInputState* _vertex_input,
        RHIVertexShader*     _vertex_shader,
        RHIFragmentShader*   _fragment_shader,
        RHIGeometryShader*   _geometry_shader) override { return RHIShaderBoundStateRef{}; }

    RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) override { return RHIGraphicsPipelineStateRef{}; }

    RHIComputePipelineStateRef RHICreateComputePipelineState(RHIComputeShader* _compute_shader) override { return RHIComputePipelineStateRef{}; }

    RHIRayTracingPipelineStateRef RHICreateRayTracingPipelineState(const RHIRayTracingPipelineStateInitializer& _init) override {
        return RHIRayTracingPipelineStateRef{};
    }

    void RHIBatchedBuildRayTracingBLAS(int batch_size, const RHIRayTracingBLASInitializer* _inits, RHIRayTracingBLASRef* results) override {
    }
    RHIRayTracingTLASRef RHIBuildRayTracingTLAS(const RHIRayTracingTLASInitializer& _init) override {
        return RHIRayTracingTLASRef{};
    }

    RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info) override { return RHIBufferRef{}; }
    void*        RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) override { return nullptr; }
    void         RHIUnmapBuffer(RHIBuffer* _buffer) override {}

    RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) override { return RHITextureRef{}; };

    RHIShaderResourceViewRef  RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override { return RHIShaderResourceViewRef{}; }
    RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override { return RHIUnorderedAccessViewRef{}; }

    RHICommandQueue* RHICreateCommandQueue(ECommandQueueType type) override { return nullptr; }
    // RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr) override { return nullptr; }

    RHIGraphicsCommandList* RHICreateGraphicsCommandList(RHICommandAllocator* _allocator, RHIGraphicsPipelineState* _initial_state = nullptr) override { return nullptr; }

    // RHIComputeCommandList* CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr) override { return nullptr; }
    RHIComputeCommandList* RHICreateComputeCommandList(RHICommandAllocator* _allocator, RHIComputePipelineState* _initial_state = nullptr) override { return nullptr; };

    RHIRayTracingCommandList* RHICreateRayTracingCommandList(RHICommandAllocator* _allocator, RHIRayTracingPipelineState* _initial_state = nullptr) override { return nullptr; }

    RHICopyCommandList* RHICreateCopyCommandList(RHICommandAllocator* _allocator) override { return nullptr; }

    // void RHISetBatchedShaderParameters(RHIGraphicsPipelineState* _pso, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant) override {}

    RHICommandAllocator* RHIGetCurrentCommandAllocator() override { return nullptr; }
#pragma endregion

#pragma region Viewport

    virtual RHIViewportRef RHICreateViewport(const RHIViewportInitializer& _init) override { return nullptr; }

    virtual void RHIResizeViewport(RHIViewport* _viewport, Extent2D _size, bool _b_full_screen, EPixelFormat _format = PF_UNDEFINED) override {}

    virtual RHIViewportNextBackBufferInfo RHIGetNextFrameViewportBufferInfo(RHIViewport* _viewport) override { return RHIViewportNextBackBufferInfo(); }

    virtual RHIUnorderedAccessView* RHIGetViewportBackBufferUAV(RHIViewport* _viewport, uint32_t index) override { return nullptr; }

    virtual void RHIPresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) override {}

    virtual RHIViewport* RHIGetMainViewport() override { return nullptr; }

#pragma endregion

protected:
    void RHISetBatchedShaderParametersInner(RHIResource* _resource, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant) override{};
};

#endif// IVULKAN_RHI_H
