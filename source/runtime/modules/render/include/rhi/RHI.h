#ifndef RHI_H
#define RHI_H
#include "PixelFormat.h"
#include "RHIResource.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include <vector>
#include "RenderAPI.h"

enum class ERHIType {
    Vulkan,
    D3D12
};
class RHIGraphicsCommandList;
class RHIComputeCommandList;
class RHICommandQueue;
class Shader;

struct RHIInitInfo {
    uint32_t max_frame_in_flight = 3;
};
class RHI {
public:
    RHI(ERHIType _type) : rhi_type(_type) {}

    virtual ~RHI() = default;

    virtual void Initialize(const RHIInitInfo& _init) = 0;

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

    virtual RHIVertexShaderRef   RHICreateVertexShader(const Shader*)   = 0;
    virtual RHIFragmentShaderRef RHICreateFragmentShader(const Shader*) = 0;
    virtual RHIGeometryShaderRef RHICreateGeometryShader(const Shader*) = 0;

    virtual RHIMeshShaderRef          RHICreateMeshShader(const Shader*)          = 0;
    virtual RHIAmplificationShaderRef RHICreateAmplificationShader(const Shader*) = 0;

    virtual RHIComputeShaderRef RHICreateComputeShader(const Shader*) = 0;

    virtual RHIShaderLibraryRef RHICreateShaderLibrary(EShaderPlatform _platform, const std::string& _file_path, const std::string& name) { return nullptr; };

    virtual RHIFenceRef RHICreateFence(const RHIFenceCreateInfo&) = 0;

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
    virtual void         RHIUploadBuffer(RHIBufferRef _buffer_ref, const uint8_t* _data, uint32_t _size) = 0;
    virtual void         RHICopyBuffer(RHIBuffer* _src, RHIBuffer* _dst)                                 = 0;
    virtual RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info)                                = 0;
    virtual void*        RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size)              = 0;
    virtual void         RHIUnmapBuffer(RHIBuffer* _buffer)                                              = 0;

    virtual RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) = 0;

    virtual RHIShaderResourceViewRef  RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info)  = 0;
    virtual RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) = 0;

    virtual RHICommandQueue* CreateCommandQueue(ECommandQueueType type) = 0;
    // DX12 only: _initial_state
    virtual RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr) = 0;
    virtual RHIComputeCommandList*  CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr)   = 0;

    virtual void RHISetBatchedShaderParameters(RHIGraphicsPipelineState* _pso, const RHIBatchedShaderParameters& _batched_params) = 0;

#pragma endregion

#pragma region GUI

    virtual bool GUIInit(uint32_t _num_frames_in_flight);
    virtual void GUIShutDown();
    virtual void GUINewFrame();
    virtual void GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list);
#pragma endregion

#pragma region Viewport

    virtual RHIViewport* RHIGetMainViewport() = 0;

    virtual RHIViewportRef RHICreateViewport(const RHIViewportInitializer& _init) = 0;

    virtual void RHIResizeViewport(RHIViewport* _viewport, Extent2D _size, bool _b_full_screen, EPixelFormat _format = PF_UNDEFINED) = 0;

    virtual RHIViewportNextBackBufferInfo RHIGetNextFrameViewportBufferInfo(RHIViewport* _viewport) = 0;

    virtual RHIUnorderedAccessView* RHIGetViewportBackBufferUAV(RHIViewport* _viewport, uint32_t index) = 0;

    virtual void RHIPresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) = 0;
#pragma endregion

protected:
    ERHIType rhi_type;
    uint32_t max_frame_in_flight;
};

extern RENDER_API RHI* g_rhi;

#endif