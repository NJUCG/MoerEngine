#ifndef RHI_H
#define RHI_H
#include "PixelFormat.h"
#include "RHICommand.h"
#include "RHIResource.h"
#include "log/LogSystem.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "RenderAPI.h"
#include "Core.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include <cstdint>
#include <optional>
#include <type_traits>

enum class ERHIType : uint8_t {
    Vulkan,
    D3D12
};
struct ShaderTargetInfo {
    uint16_t shader_type;
    uint16_t shader_platform;
    ShaderTargetInfo(const ShaderTargetInfo& _other) : shader_type(_other.shader_type), shader_platform(_other.shader_platform) {}
    operator uint32_t() const { return *(uint32_t*)this; }
    ShaderTargetInfo(EShaderType _type, EShaderPlatform _platform)
        : shader_type(_type),
          shader_platform(_platform) {}
    ShaderTargetInfo(uint32_t _info) : shader_type(_info & 0xffff), shader_platform(static_cast<EShaderPlatform>(_info >> 16)) {}

    ShaderTargetInfo() = default;

    operator EShaderType() const { return static_cast<EShaderType>(shader_type); }
    operator EShaderPlatform() const { return static_cast<EShaderPlatform>(shader_platform); }
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
struct DeviceInitInfo {
    ERHIType            type;
    std::string_view    name;
    MoerRHIConfigAsJSON config_as_json;
};

template<typename T>
concept TPipelineStateRef = requires(T) {
    std::convertible_to<T, RHIGfxPsoRef> || std::convertible_to<T, RHIComputePsoRef>;
};
class RENDER_API RHI {
public:
    RHI() = default;

    virtual ~RHI() = default;

    virtual void Initialize(const RHIInitInfo& _init) = 0;

    virtual void PostInit() {}

    virtual void ShutDown() = 0;

    virtual const char* GetName() = 0;

    virtual ERHIType GetType() const = 0;

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

    virtual RHIGfxPsoRef RHICreateGraphicsPSO(RHIGraphicsPSOCreateInfo&& _init) = 0;
    /* create pso from cache */
    // virtual RHIGraphicsPipelineStateRef RHICreateGraphicsPipelineState(const RHIGraphicsPipelineStateInfo& _init, RHIPipelineBinaryDataLibrary* _pipeline_library) {
    //     return RHICreateGraphicsPipelineState(_init);
    // }

    virtual RHIComputePsoRef RHICreateComputePipelineState(RHIShader* _compute_shader) = 0;

    /* create pso from cache */
    virtual RHIComputePsoRef RHICreateComputePipelineState(RHIShader* _compute_shader, RHIPipelineBinaryDataLibrary* _pipeline_library) {
        return RHICreateComputePipelineState(_compute_shader);
    }

    virtual RHIRTPsoRef RHICreateRayTracingPipelineState(const RHIRayTracingPipelineStateInitializer& _init) = 0;

    /* create pso from cache */
    virtual RHIRTPsoRef RHICreateRayTracingPipelineState(const RHIRayTracingPipelineStateInitializer& _init, RHIPipelineBinaryDataLibrary* _pipeline_library) {
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

    // virtual RHICBVRef RHICreateCBV(RHIBuffer* _resource, uint64_t _size, uint64_t _byte_offset = 0) = 0;

    RHICBVRef RHICreateCBV(RHIBuffer* _resource, uint64_t _byte_offset = 0) {
        return RHICreateCBV(_resource, _resource->GetByteSize(), _byte_offset);
    }

    RHICBVRef RHICreateCBV(RHIBuffer* _resource, uint64_t _size, uint64_t _byte_offset = 0) {
        RHIViewRef view = RHICreateBufferView<v_type_buffer_cbv>(_resource, 0, _size, _byte_offset);
        return RHICBVRef(static_cast<RHICBV*>(view.Get()));
    }

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
        uint32_t   _stride      = 0,
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
        uint32_t   _stride      = 0,
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

    RHISRVRef RHICreateAccelerationStructureSRV(
        RHIRayTracingTLAS* _tlas);

    virtual RHICommandQueue* RHICreateCommandQueue(ECommandQueueType type) = 0;
    // DX12 only: _initial_state
    // virtual RHIGraphicsCommandList* CreateGraphicsCommandList(RHIGraphicsPipelineState* _initial_state = nullptr)                                     = 0;
    virtual RHIGraphicsCommandList* RHICreateGraphicsCommandList(RHIGfxPso* _initial_state = nullptr) = 0;
    // virtual RHIComputeCommandList*  CreateComputeCommandList(RHIComputePipelineState* _initial_state = nullptr)   = 0;
    virtual RHIComputeCommandList*    RHICreateComputeCommandList(RHIComputePso* _initial_state = nullptr) = 0;
    virtual RHIRayTracingCommandList* RHICreateRayTracingCommandList(RHIRTPso* _initial_state = nullptr)   = 0;
    virtual RHICopyCommandList*       RHICreateCopyCommandList()                                           = 0;
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
    virtual RHIViewRef   RHICreateViewInner(RHIViewableResource* _resource, const RHIViewInfo& _view_info)                                                     = 0;
    template<uint32_t _type>
    RHIViewRef RHICreateBufferView(RHIBuffer* _resource, uint64_t _stride, uint64_t _byte_size, uint64_t _byte_offset) {
        auto true_stride = _stride == 0 ? _resource->GetStride() : _stride;

        auto true_size = _byte_size == 0 ? _resource->GetByteSize() : _byte_size;
        true_size      = (true_size + _byte_offset) > _resource->GetByteSize() ? (_resource->GetByteSize() - _byte_offset) : true_size;
        if (_byte_offset >= _resource->GetByteSize()) {
            LOG_ERROR("Invalid byte offset: {} for buffer: {}", _byte_offset, _resource->GetName());
            return nullptr;
        }
        RHIViewInfo view_info(GetBufferInfo<_type>(_resource, _byte_offset, true_size / true_stride, true_stride));
        return RHICreateViewInner(_resource, std::move(view_info));
    }

protected:
    RHIInfo m_rhi_info;
};
namespace Moer::Render {

    template<typename T>
    static T ResolveConfigAs(const MoerRHIConfigAsJSON& _config_as_json);

    struct DeviceConfig {
        uint b_support_ray_tracing : 1;
        uint b_support_mesh_shader : 1;
        uint b_support_task_shader : 1;
        uint b_support_bindless : 1;
        uint b_support_direct_storage : 1;
        uint b_support_virtual_texture : 1;
    };
    class RenderDevice {
    public:
        RENDER_API static void          Init(DeviceInitInfo&& _info);
        RENDER_API static void          Dispose();
        RENDER_API static RenderDevice& Get();

    public:
        template<typename TElement>
            requires(std::is_trivially_copyable_v<TElement> && std::is_standard_layout_v<TElement> || NumericType<TElement>)
        BufferRef CreateBuffer(uint _element_cnt, EBufferUsageFlags _usage) {
            return CreateBuffer(_element_cnt, sizeof(TElement), _usage);
        }

        RENDER_API BufferRef CreateStagingBuffer(uint64_t _byte_size);

        RENDER_API TextureRef CreateTexture(Extent2D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt = 1, uint32_t _array_size = 1);

        RENDER_API TextureRef CreateTexture(Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt = 1, uint32_t _array_size = 1);

        RENDER_API DepthBufferRef CreateDepthBuffer(Extent2D _size, EPixelFormat _format, uint32_t _array_size = 1);

        RENDER_API BindlessArrayRef CreateBindlessArray(uint _max_size = 5000);
        // BackBufferInfo GetNextBackBufferInfo(RHIViewport* _viewport);

        // TextureView GetBackBuffer(RHIViewport* _viewport, uint32_t _index);

        // void PresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence);
        RENDER_API void FlushPendingDeletes();

        RENDER_API const EShaderPlatform GetShaderPlatform() const;

        ERHIType GetRHIType() const { return rhi_type; }

        RENDER_API PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders);//gfx
        RENDER_API PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders);                              //compute

        RENDER_API CommandQueue& GetCommandQueue(EQueueType _type);

        RENDER_API CopyQueue& GetCopyQueue();

        RENDER_API SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info);

        RENDER_API FenceRef CreateFence();

        RENDER_API RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo& _init);

        RENDER_API RaytracingSceneRef CreateRaytracingScene();

        class Impl;

    protected:
    private:
        RENDER_API BufferRef CreateBuffer(uint _element_cnt, uint _stride, EBufferUsageFlags _usage);

        RenderDevice() = default;
        UniquePtr<Impl>
                 impl;
        ERHIType rhi_type;

        DeviceConfig config;
    };
};// namespace Moer::Render

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
    if (Moer::IsCurrentlyRenderThread()) {
        TRenderTaskType task(std::forward<Funtion>(_func));
        task.Fire(EThread::EMainThread, nullptr);
    }
    if (Moer::IsCurrentlyGameThread()) {
        // GraphTask<TRenderTaskType>::CreateTask().ConstructAndDispatchWhenReady(std::forward<Funtion>(_func));
        GraphTask<TRenderTaskType>::Create(std::forward<Funtion>(_func)).Dispatch(EThread::ERenderThread);
    } else {
        // Any Thread maybe
        // immediately execute on render thread
        // TRenderTaskType task(std::forward<Funtion>(_func));
        // task.Fire(EThread::EMainThread, nullptr);
        // GraphTask<TRenderTaskType>::CreateTask().ConstructAndDispatchWhenReady(std::forward<Funtion>(_func));
        GraphTask<TRenderTaskType>::Create(std::forward<Funtion>(_func)).Dispatch(EThread::ERenderThread);
    }
}
#endif