#ifndef RHI_H
#define RHI_H
#include "RHIResource.h"
#include <vector>
enum class ERHIType {
    Vulkan,
    D3D12
};
class RHIGraphicsCommandList;
class RHIComputeCommandList;
class RHICommandQueue;

class RHI {
public:
    virtual ~RHI() = default;

    virtual void Initialize() = 0;

    virtual void PostInit() {}

    virtual void ShutDown() = 0;

    virtual const char* GetName() = 0;

    ERHIType GetType() const { return rhi_type; }

    //todo: test usage, delete later
    static void Test();

#pragma region resources creation

    virtual RHISamplerRef            RHICreateSampler(const RHISamplerInitializer& _initializer)                = 0;
    virtual RHIRasterizationStateRef RHICreateRasterizationState(const RHIRasterizationStateInitializer& _init) = 0;
    virtual RHIDepthStencilStateRef  RHICreateDepthStencilState(const RHIDepthStencilStateInitializer& _init)   = 0;
    virtual RHIMultisampleStateRef   RHICreateMultiSampleState(const RHIMultisampleStateInitializer& _init)     = 0;
    virtual RHIBlendStateRef         RHICreateBlendState(const RHIBlendStateInitializer& _init)                 = 0;
    virtual RHIVertexInputStateRef   RHICreateVertexInputState(const VertexInputStateInitializerList& _init)    = 0;

    virtual RHIVertexShaderRef   RHICreateVertexShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash)   = 0;
    virtual RHIFragmentShaderRef RHICreateFragmentShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) = 0;
    virtual RHIGeometryShaderRef RHICreateGeometryShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) = 0;

    virtual RHIMeshShaderRef          RHICreateMeshShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash)          = 0;
    virtual RHIAmplificationShaderRef RHICreateAmplificationShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) = 0;

    virtual RHIComputeShaderRef RHICreateComputeShader(const std::vector<uint8_t>& _code, const SHA256Hash& _hash) = 0;

    virtual RHIShaderLibraryRef RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) { return nullptr; };

    virtual RHIFenceRef RHICreateFence(const std::string& name) = 0;

    /* create cpu visible buffer for direct data transfer */
    virtual RHIStagingBufferRef RHICreateStagingBuffer() = 0;

    virtual RHIShaderBoundStateRef RHICreateShaderBoundStage(
        RHIVertexInputState* _vertex_input,
        RHIVertexShader*     _vertex_shader,
        RHIFragmentShader*   _fragment_shader,
        RHIGeometryShader*   _geometry_shader) = 0;

    virtual RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init) = 0;

    /* create pso from cache */
    virtual RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInitializer& _init, RHIPipelineBinaryDataLibrary* _pipeline_library) {
        return RHICreateGraphicsPipelineState(_init);
    }

    virtual RHIComputePipelineStateRef RHICreateComputePipelineState(RHIComputeShader* _compute_shader) = 0;

    /* create pso from cache */
    virtual RHIComputePipelineStateRef RHICreateComputePipelineState(RHIComputeShader* _compute_shader, RHIPipelineBinaryDataLibrary* _pipeline_library) {
        return RHICreateComputePipelineState(_compute_shader);
    }

    /* constant buffer creation */
    virtual RHIGlobalBufferRef RHICreateUniformBuffer(const void* data, const RHIGlobalBufferLayout* layout, EBufferUsageFlags _usage) = 0;

    //todo: constant buffer update

    virtual void RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst) = 0;

    virtual RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info)                   = 0;
    virtual void*        RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) = 0;
    virtual void         RHIUnmapBuffer(RHIBuffer* _buffer)                                 = 0;

    virtual RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) = 0;

    virtual RHIShaderResourceViewRef  RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info)  = 0;
    virtual RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) = 0;

    virtual RHICommandQueue* CreateCommandQueue(
        ECommandQueueType type) = 0;

    virtual RHIGraphicsCommandList* CreateGraphicsCommandList(
        RHIGraphicsPipelineState* _initial_state = nullptr) = 0;

    virtual RHIComputeCommandList* CreateComputeCommandList(
        RHIComputePipelineState* _initial_state = nullptr) = 0;

#pragma endregion

protected:
    ERHIType rhi_type;
};

extern RHI* g_rhi;

#endif