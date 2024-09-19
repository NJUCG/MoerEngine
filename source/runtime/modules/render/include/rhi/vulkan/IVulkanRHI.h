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
    IVulkanRHI() = default;

    void        Initialize(const RHIInitInfo& _init) override {}
    void        PostInit() override {}
    void        ShutDown() override {}
    const char* GetName() override { return "VulkanRHI Interface"; }
    ERHIType    GetType() const override final { return ERHIType::Vulkan; };

#pragma region resources creation
    RHISamplerRef RHICreateSampler(const RHISamplerCreateInfo& _initializer) override { return RHISamplerRef{}; }

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

    // RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInfo& _init) override { return RHIGraphicsPipelineStateRef{}; }
    RHIGfxPsoRef RHICreateGraphicsPSO(RHIGraphicsPSOCreateInfo&& _init) override { return RHIGfxPsoRef{}; }

    RHIComputePsoRef RHICreateComputePipelineState(RHIShader* _compute_shader) override { return RHIComputePsoRef{}; }

    RHIRTPsoRef RHICreateRayTracingPipelineState(const RHIRayTracingPipelineStateInitializer& _init) override {
        return RHIRTPsoRef{};
    }

    void RHIBatchedBuildRayTracingBLAS(int batch_size, const RHIRayTracingBLASInitializer* _inits, RHIRayTracingBLASRef* results) override {
    }
    RHIRayTracingTLASRef RHIBuildRayTracingTLAS(const RHIRayTracingTLASInitializer& _init) override {
        return RHIRayTracingTLASRef{};
    }

    void* RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) override { return nullptr; }
    void  RHIUnmapBuffer(RHIBuffer* _buffer) override {}

    RHIBufferRef RHICreateStagingBuffer(uint64_t _byte_size) override { return RHIBufferRef{}; }

    RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) override { return RHITextureRef{}; };

    RHICommandQueue* RHICreateCommandQueue(ECommandQueueType type) override { return nullptr; }
    // RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr) override { return nullptr; }

    RHIGraphicsCommandList* RHICreateGraphicsCommandList(RHIGfxPso* _initial_state = nullptr) override { return nullptr; }

    // RHIComputeCommandList* CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr) override { return nullptr; }
    RHIComputeCommandList* RHICreateComputeCommandList(RHIComputePso* _initial_state = nullptr) override { return nullptr; };

    RHIRayTracingCommandList* RHICreateRayTracingCommandList(RHIRTPso* _initial_state = nullptr) override { return nullptr; }

    RHICopyCommandList* RHICreateCopyCommandList() override { return nullptr; }

    // void RHISetBatchedShaderParameters(RHIGraphicsPipelineState* _pso, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant) override {}

    RHICommandAllocator* RHIGetCurrentCommandAllocator() override { return nullptr; }

#pragma endregion

#pragma region Viewport

    virtual RHIViewportRef RHICreateViewport(const RHIViewportInitializer& _init) override { return nullptr; }

    virtual void RHIResizeViewport(RHIViewport* _viewport, Extent2D _size, bool _b_full_screen, EPixelFormat _format = PF_UNDEFINED) override {}

    virtual RHIViewportNextBackBufferInfo RHIGetNextFrameViewportBufferInfo(RHIViewport* _viewport) override { return RHIViewportNextBackBufferInfo(); }

    virtual RHIUAV* RHIGetViewportBackBufferUAV(RHIViewport* _viewport, uint32_t index) override { return nullptr; }

    virtual void RHIPresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) override {}

    virtual RHIViewport* RHIGetMainViewport() override { return nullptr; }

#pragma endregion

protected:
    void         RHISetBatchedShaderParametersInner(RHIResource* _resource, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant) override{};
    RHIBufferRef RHICreateBufferInner(const RHIBufferCreateInfo& info) override { return RHIBufferRef{}; }
    RHIViewRef   RHICreateViewInner(RHIViewableResource* _resource, const RHIViewInfo& _view_info) override { return RHIViewRef{}; }
};

#endif// IVULKAN_RHI_H
