#ifndef RHI_H
#define RHI_H
#include "PixelFormat.h"
#include "RHIResource.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include <vector>
enum class ERHIType {
    Vulkan,
    D3D12
};
class RHIGraphicsCommandList;
class RHIComputeCommandList;
class RHICommandQueue;
class Shader;

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

    virtual RHICommandQueue*        CreateCommandQueue(ECommandQueueType type)                                    = 0;
    virtual RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr) = 0;
    virtual RHIComputeCommandList*  CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr)   = 0;

    virtual RHIShaderRef RHICreateShader(Shader*) = 0;

#pragma endregion

#pragma region GUI
    virtual bool GUIInit(uint32_t _num_frames_in_flight);
    virtual void GUIShutDown();
    virtual void GUINewFrame();
    virtual void GUIRender(RHIGraphicsCommandList* _ui_command_list);
#pragma endregion

#pragma region Viewport

    virtual RHIViewportRef RHICreateViewport(const RHIViewportInitializer& _init) = 0;

    virtual void RHIResizeViewport(RHIViewport* _viewport, Extent2D _size, bool _b_full_screen, EPixelFormat _format = PF_UNDEFINED) = 0;

    virtual RHITextureRef RHIGetViewportBackBuffer(RHIViewport* _viewport) = 0;

    virtual RHIUnorderedAccessViewRef RHIGetViewportBackBufferUAV(RHIViewport* _viewport) { return nullptr; }

    virtual void RHIBeginDrawingViewport(RHIViewport*, RHITexture* _viewport_attachment) = 0;
    //present to swapchain here
    virtual void RHIEndDrawingViewport(RHIViewport*, bool _b_present) = 0;
#pragma endregion

protected:
    ERHIType rhi_type;
};

extern RHI* g_rhi;

#endif