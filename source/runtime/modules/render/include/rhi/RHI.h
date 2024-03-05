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
#include <cstdint>
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
    bool     ray_tracing         = false;
};

struct RHIInfo {
    ERHIType rhi_type;
    uint32_t max_frame_in_flight;
    bool     ray_tracing;
};

template<typename T>
concept TPipelineStateRef = requires(T) {
    std::convertible_to<T, RHIGraphicsPipelineStateRef> || std::convertible_to<T, RHIComputePipelineStateRef>;
};
class RENDER_API RHI {
public:
    RHI(ERHIType _type) : m_rhi_info(_type) {}

    virtual ~RHI() = default;

    virtual void Initialize(const RHIInitInfo& _init) = 0;

    virtual void PostInit() {}

    virtual void ShutDown() = 0;

    virtual const char* GetName() = 0;

    ERHIType GetType() const { return m_rhi_info.rhi_type; }

    //todo: test usage, delete later
    static void Test();

#pragma region resources creation

    virtual RHISamplerRef RHICreateSampler(const RHISamplerCreateInfo& _initializer) = 0;

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

    virtual RHIGraphicsPipelineStateRef RHICreateGraphicsPSO(RHIGraphicsPSOCreateInfo&& _init) = 0;
    /* create pso from cache */
    // virtual RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInfo& _init, RHIPipelineBinaryDataLibrary* _pipeline_library) {
    //     return RHICreateGraphicsPipelineState(_init);
    // }

    virtual RHIComputePipelineStateRef RHICreateComputePipelineState(RHIShader* _compute_shader) = 0;

    /* create pso from cache */
    virtual RHIComputePipelineStateRef RHICreateComputePipelineState(RHIShader* _compute_shader, RHIPipelineBinaryDataLibrary* _pipeline_library) {
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

    template<typename TElement>
        requires(std::is_trivially_copyable_v<TElement> && std::is_standard_layout_v<TElement>)
    RHIBufferRef RHICreateBuffer(uint64_t _byte_size, EBufferUsageFlags _usage) {
        auto create_info = RHIBufferCreateInfo::Create(_byte_size, sizeof(TElement), _usage);
        return RHICreateBufferInner(create_info);
    }
    virtual RHIBufferRef RHICreateStagingBuffer(uint64_t _byte_size)                        = 0;
    virtual void*        RHIMapBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size) = 0;
    virtual void         RHIUnmapBuffer(RHIBuffer* _buffer)                                 = 0;

    virtual RHITextureRef RHICreateTexture(const RHITextureCreateInfo& info) = 0;

    template<typename TElement>
        requires(std::is_trivially_copyable_v<TElement> && std::is_standard_layout_v<TElement>)
    RHISRVRef RHICreateBufferSRV(
        RHIBuffer* _resource,
        uint64_t   _byte_size   = 0,
        uint64_t   _byte_offset = 0) {
        return RHICreateBufferSRV(_resource, sizeof(TElement), _byte_size, _byte_offset);
    };

    RHISRVRef RHICreateBufferSRV(
        RHIBuffer* _resource,
        uint32_t   stride       = 0,
        uint64_t   _byte_size   = 0,
        uint64_t   _byte_offset = 0);

    template<typename TElement>
        requires(std::is_trivially_copyable_v<TElement> && std::is_standard_layout_v<TElement>)
    RHIUAVRef RHICreateBufferUAV(
        RHIBuffer* _resource,
        uint64_t   _byte_size   = 0,
        uint64_t   _byte_offset = 0) {
        return RHICreateBufferUAV(_resource, sizeof(TElement), _byte_size, _byte_offset);
    };

    RHIUAVRef RHICreateBufferUAV(
        RHIBuffer* _resource,
        uint32_t   stride       = 0,
        uint64_t   _byte_size   = 0,
        uint64_t   _byte_offset = 0);

    RHISRVRef RHICreateTextureSRV(
        RHITexture*  _resource,
        EPixelFormat _format      = PF_UNDEFINED,
        uint32_t     _mip_level   = 0,
        uint32_t     _mip_levels  = 1,
        uint32_t     _array_index = 0,
        uint32_t     _array_size  = 1);

    RHIUAVRef RHICreateTextureUAV(
        RHITexture*  _resource,
        EPixelFormat _format      = PF_UNDEFINED,
        uint32_t     _mip_level   = 0,
        uint32_t     _array_index = 0,
        uint32_t     _array_size  = 1);

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

#pragma region Viewport

    virtual RHIViewport* RHIGetMainViewport() = 0;

    virtual RHIViewportRef RHICreateViewport(const RHIViewportInitializer& _init) = 0;

    virtual void RHIResizeViewport(RHIViewport* _viewport, Extent2D _size, bool _b_full_screen, EPixelFormat _format = PF_UNDEFINED) = 0;

    virtual RHIViewportNextBackBufferInfo RHIGetNextFrameViewportBufferInfo(RHIViewport* _viewport) = 0;

    virtual RHIUAV* RHIGetViewportBackBufferUAV(RHIViewport* _viewport, uint32_t index) = 0;

    virtual void RHIPresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) = 0;
#pragma endregion

#pragma region RenderThread methods

    void RHIFlushPendingDeletes();
#pragma endregion
protected:
    virtual void         RHISetBatchedShaderParametersInner(RHIResource* _resource, const RHIBatchedShaderParameters& _batched_params, bool b_update_constant) = 0;
    virtual RHIBufferRef RHICreateBufferInner(const RHIBufferCreateInfo& info)                                                                                 = 0;
    virtual RHISRVRef    RHICreateSRVInner(RHIViewableResource* _resource, const RHIViewInfo& _view_info)                                                      = 0;
    virtual RHIUAVRef    RHICreateUAVInner(RHIViewableResource* _resource, const RHIViewInfo& _view_info)                                                      = 0;

protected:
    RHIInfo m_rhi_info;
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