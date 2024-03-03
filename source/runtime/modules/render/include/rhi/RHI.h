#ifndef RHI_H
#define RHI_H
#include "PixelFormat.h"
#include "RHIResource.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "RenderAPI.h"
#include "Core.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include <type_traits>

enum class ERHIType {
    Vulkan,
    D3D12
};
class RHIGraphicsCommandList;
class RHIComputeCommandList;
class RHIRayTracingCommandList;
class RHICopyCommandList;
class RHICommandQueue;
class RHICommandAllocator;
class Shader;

struct RHIInitInfo {
    uint32_t max_frame_in_flight = 3;
};

template<typename T>
concept TPipelineStateRef = requires(T) {
                                std::convertible_to<T, RHIGraphicsPipelineStateRef> || std::convertible_to<T, RHIComputePipelineStateRef>;
                            };
class RENDER_API RHI {
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

    virtual RHIComputeShaderRef RHICreateComputeShader(const class ShaderCodeEntry*, const Shader*) = 0;

    virtual RHIVertexShaderRef   RHICreateVertexShader(const class ShaderCodeEntry*, const Shader*)   = 0;
    virtual RHIFragmentShaderRef RHICreateFragmentShader(const class ShaderCodeEntry*, const Shader*) = 0;
    virtual RHIGeometryShaderRef RHICreateGeometryShader(const class ShaderCodeEntry*, const Shader*) = 0;

    virtual RHIMeshShaderRef          RHICreateMeshShader(const class ShaderCodeEntry*, const Shader*)          = 0;
    virtual RHIAmplificationShaderRef RHICreateAmplificationShader(const class ShaderCodeEntry*, const Shader*) = 0;

    virtual RHIRayGenShaderRef          RHICreateRayGenShader(const class ShaderCodeEntry*, const Shader*)          = 0;
    virtual RHIRayMissShaderRef         RHICreateRayMissShader(const class ShaderCodeEntry*, const Shader*)         = 0;
    virtual RHIRayClosestHitShaderRef   RHICreateRayClosestHitShader(const class ShaderCodeEntry*, const Shader*)   = 0;
    virtual RHIRayCallableShaderRef     RHICreateRayCallableShader(const class ShaderCodeEntry*, const Shader*)     = 0;
    virtual RHIRayIntersectionShaderRef RHICreateRayIntersectionShader(const class ShaderCodeEntry*, const Shader*) = 0;
    virtual RHIRayAnyhitShaderRef       RHICreateRayAnyhitShader(const class ShaderCodeEntry*, const Shader*)       = 0;

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

    virtual RHIRayTracingPipelineStateRef RHICreateRayTracingPipelineState(const RHIRayTracingPipelineStateInitializer& _init) = 0;

    /* create pso from cache */
    virtual RHIRayTracingPipelineStateRef RHICreateRayTracingPipelineState(const RHIRayTracingPipelineStateInitializer& _init, RHIPipelineBinaryDataLibrary* _pipeline_library) {
        return RHICreateRayTracingPipelineState(_init);
    }

    /*batching creation and building of blases*/
    virtual RHIRayTracingBLASRef RHIBuildRayTracingBLAS(const RHIRayTracingBLASInitializer& _init) {
        RHIRayTracingBLASRef result;
        RHIBatchedBuildRayTracingBLAS(1, &_init, &result);
        return result;
    }
    virtual void RHIBatchedBuildRayTracingBLAS(int batch_size, const RHIRayTracingBLASInitializer* _inits, RHIRayTracingBLASRef* results) = 0;

    virtual RHIRayTracingTLASRef RHIBuildRayTracingTLAS(const RHIRayTracingTLASInitializer& _init) = 0;

    virtual RHIBufferRef RHICreateBuffer(const RHIBufferCreateInfo& info)                   = 0;
    virtual void*        RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) = 0;
    virtual void         RHIUnmapBuffer(RHIBuffer* _buffer)                                 = 0;

    virtual RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) = 0;

    virtual RHIShaderResourceViewRef  RHICreateShaderResourceView(RHIViewableResource* _resource, const RHIViewInfo& _view_info)  = 0;
    virtual RHIUnorderedAccessViewRef RHICreateUnorderedAccessView(RHIViewableResource* _resource, const RHIViewInfo& _view_info) = 0;

    virtual RHICommandQueue* RHICreateCommandQueue(ECommandQueueType type) = 0;
    // DX12 only: _initial_state
    // virtual RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr)                                     = 0;
    virtual RHIGraphicsCommandList* RHICreateGraphicsCommandList(RHICommandAllocator* _allocator, RHIGraphicsPipelineState* _initial_state = nullptr) = 0;
    // virtual RHIComputeCommandList*  CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr)   = 0;
    virtual RHIComputeCommandList*    RHICreateComputeCommandList(RHICommandAllocator* _allocator, RHIComputePipelineState* _initial_state = nullptr)       = 0;
    virtual RHIRayTracingCommandList* RHICreateRayTracingCommandList(RHICommandAllocator* _allocator, RHIRayTracingPipelineState* _initial_state = nullptr) = 0;
    virtual RHICopyCommandList*       RHICreateCopyCommandList(RHICommandAllocator* _allocator)                                                             = 0;
    template<TPipelineStateRef TPipelineRef>
    void RHISetBatchedShaderParameters(TPipelineRef _pso, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant = false) {
        RHISetBatchedShaderParametersInner(_pso, _batched_params, b_update_constant);
    };

    virtual RHICommandAllocator* RHIGetCurrentCommandAllocator() = 0;
#pragma endregion

    // #pragma region GUI

    // virtual bool GUIInit(uint32_t _num_frames_in_flight);
    // virtual void GUIShutDown();
    // virtual void GUINewFrame();
    // virtual void GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list);
    // #pragma endregion

#pragma region Viewport

    virtual RHIViewport* RHIGetMainViewport() = 0;

    virtual RHIViewportRef RHICreateViewport(const RHIViewportInitializer& _init) = 0;

    virtual void RHIResizeViewport(RHIViewport* _viewport, Extent2D _size, bool _b_full_screen, EPixelFormat _format = PF_UNDEFINED) = 0;

    virtual RHIViewportNextBackBufferInfo RHIGetNextFrameViewportBufferInfo(RHIViewport* _viewport) = 0;

    virtual RHIUnorderedAccessView* RHIGetViewportBackBufferUAV(RHIViewport* _viewport, uint32_t index) = 0;

    virtual void RHIPresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) = 0;
#pragma endregion

#pragma region RenderThread methods

    void RHIFlushPendingDeletes();
#pragma endregion
protected:
    virtual void RHISetBatchedShaderParametersInner(RHIResource* _resource, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant) = 0;

protected:
    ERHIType rhi_type;
    uint32_t max_frame_in_flight;
};

extern RENDER_API RHI* g_rhi;

class RenderThreadTask {
public:
    // All render commands run on the render thread
    static EThread::Type GetPreferredThread() {
        assert(EThread::ERenderThread != EThread::EMainThread);
        return EThread::ERenderThread;
    }
};

template<typename TaskNameType, typename Funtion>
class RenderThreadTaskType : public RenderThreadTask {
public:
    RenderThreadTaskType(Funtion&& _func) : funtion(std::forward<Funtion>(_func)) {}
    static EThread::Type GetPreferredThread() {
        return EThread::ERenderThread;
    }
    void Fire(EThread::Type _thread, const GraphEventRef& _my_completion_graph_event) {
        //TODO: profiler here
        funtion();
    }

protected:
    Funtion funtion;
};

struct UndefinedRenderTaskName {
    static const char* Name() {
        return "UndefinedRenderTask";
    }
};
/**
 * @brief Enqueue a render task to render thread,
 *        if current thread is render thread, execute immediately
 * 
 * @tparam TaskNameType task name type for statistic profiling
 * @tparam Funtion lambda type
 * @param _func lambda function
 * @return FORCEINLINE 
 */
template<typename Funtion, typename TaskNameType = UndefinedRenderTaskName>
FORCEINLINE void EnqueueRenderTask(Funtion&& _func) {
    using TRenderTaskType = RenderThreadTaskType<TaskNameType, Funtion>;
    if (Moer::IsCurrentlyGameThread()) {
        GraphTask<TRenderTaskType>::CreateTask().ConstructAndDispatchWhenReady(std::forward<Funtion>(_func));
    } else {
        //immediately execute on render thread
        TRenderTaskType task(std::forward<Funtion>(_func));
        task.Fire(EThread::EMainThread, nullptr);
    }
}
#endif