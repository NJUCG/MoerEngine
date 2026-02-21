#ifndef RHI_H
#define RHI_H
#include "Core.h"
#include "PixelFormat.h"
#include "RenderAPI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "taskgraph/ThreadManager.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <type_traits>

// Forward declarations to break circular dependency
namespace Moer::Render {
class CommandQueue;
class CopyQueue;
class IOInterface;
using IOInterfaceRef = std::shared_ptr<IOInterface>;
} // namespace Moer::Render

#include "rhi/RHICommand.h"

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
    ERHIType         rhi_type;
    std::string_view name;
    std::string_view rhi_api_version;
};
namespace Moer::Render {

enum class ERHIPipeline : uint8 {
    None         = 0,
    Graphics     = 1,
    AsyncCompute = 2,

    All = 3,
    Num = 2
};

inline constexpr size_t GetRHIPipelineCount() {
    return static_cast<size_t>(ERHIPipeline::Num);
}

inline constexpr size_t GetRHIPipelineIndex(ERHIPipeline Pipeline) {
    switch (Pipeline) {
        case ERHIPipeline::Graphics:
            return 0;
        case ERHIPipeline::AsyncCompute:
            return 1;
        default:
            assert(false && "Invalid ERHIPipeline for indexing.");
            return 0;
    }
}

/** Array of elements by RHI pipeline, with overloads to help with enum conversion. */
template<typename ElementType>
class TRHIPipelineArray : public Moer::StaticArray<ElementType, GetRHIPipelineCount()> {
    using Base = Moer::StaticArray<ElementType, GetRHIPipelineCount()>;

public:
    using Base::Base;

    inline ElementType& operator[](int32 Index) {
        return Base::operator[](static_cast<size_t>(Index));
    }

    inline const ElementType& operator[](int32 Index) const {
        return Base::operator[](static_cast<size_t>(Index));
    }

    inline ElementType& operator[](ERHIPipeline Pipeline) {
        return Base::operator[](GetRHIPipelineIndex(Pipeline));
    }

    inline const ElementType& operator[](ERHIPipeline Pipeline) const {
        return Base::operator[](GetRHIPipelineIndex(Pipeline));
    }
};

template<typename T>
static T ResolveConfigAs(const DeviceInitInfo& _info);

template<typename T>
struct user_trivial_type {
    //has user defined const expr user_flag
    template<typename U>
    static auto Test(U* _p) -> decltype(U::user_trival_type, std::true_type{});
    static auto Test(...) -> std::false_type;

    static constexpr bool value = decltype(Test(static_cast<T*>(nullptr)))::value;
};

#define USER_TRIVIAL_TYPE(T)                \
    template<>                              \
    struct user_trivial_type<T> {           \
        static constexpr bool value = true; \
    }

USER_TRIVIAL_TYPE(uint2);
USER_TRIVIAL_TYPE(uint3);
USER_TRIVIAL_TYPE(uint4);
USER_TRIVIAL_TYPE(int2);
USER_TRIVIAL_TYPE(int3);
USER_TRIVIAL_TYPE(int4);
USER_TRIVIAL_TYPE(float2);
USER_TRIVIAL_TYPE(float3);
USER_TRIVIAL_TYPE(float4);
USER_TRIVIAL_TYPE(float2x2);
USER_TRIVIAL_TYPE(float3x3);
USER_TRIVIAL_TYPE(float4x4);

#undef USER_TRIVIAL_TYPE

template<typename T>
static constexpr bool user_trivial_type_v = user_trivial_type<T>::value;

struct DeviceConfig {
    uint b_support_ray_tracing : 1;
    uint b_support_mesh_shader : 1;
    uint b_support_task_shader : 1;
    uint b_support_bindless : 1;
    uint b_support_direct_storage : 1;
    uint b_support_virtual_texture : 1;
};

class DeviceExtension {
protected:
    virtual ~DeviceExtension() = default;
};

template<typename T>
concept DeviceExt =
    std::is_base_of_v<DeviceExtension, T> && std::is_same_v<const std::string_view, decltype(T::name)>;

class RenderDevice {
public:
    RENDER_API static void          Init(DeviceInitInfo&& _info);
    RENDER_API static void          Dispose();
    RENDER_API static RenderDevice& Get();

public:
    template<typename TElement>
        requires(
            std::is_trivially_copyable_v<TElement> && std::is_standard_layout_v<TElement> ||
            NumericType<TElement> || user_trivial_type_v<TElement>
        )
    BufferRef CreateBuffer(
        std::string_view  _name,
        uint              _element_cnt,
        EBufferUsageFlags _usage,
        EPixelFormat      _format = PF_UNDEFINED
    ) {
        return CreateBuffer(_name, _element_cnt, sizeof(TElement), _usage, _format);
    }

    RENDER_API BufferRef CreateStagingBuffer(uint64_t _byte_size);

    RENDER_API TextureRef CreateTexture(
        Extent2D           _size,
        EPixelFormat       _format,
        ETextureUsageFlags _usage,
        uint32_t           _mip_cnt    = 1,
        uint32_t           _array_size = 1
    );

    RENDER_API TextureRef CreateTexture(
        Extent3D           _size,
        EPixelFormat       _format,
        ETextureUsageFlags _usage,
        uint32_t           _mip_cnt    = 1,
        uint32_t           _array_size = 1
    );

    RENDER_API TextureRef CreateTexture(
        std::string_view   _name,
        Extent3D           _size,
        EPixelFormat       _format,
        ETextureUsageFlags _usage,
        uint32_t           _mip_cnt    = 1,
        uint32_t           _array_size = 1
    );

    RENDER_API TextureRef CreateCubeMap(
        std::string_view   _name,
        Extent2D           _size,
        EPixelFormat       _format,
        ETextureUsageFlags _usage,
        uint32_t           _mip_cnt = 1
    );

    RENDER_API DepthBufferRef CreateDepthBuffer(
        std::string_view   _name,
        Extent2D           _size,
        EPixelFormat       _format,
        uint32_t           _array_size = 1,
        ETextureUsageFlags _usage      = ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT
    );

    static constexpr uint       k_default_bindless_array_size = 5000;
    RENDER_API BindlessArrayRef CreateBindlessArray(uint _max_size = k_default_bindless_array_size);

    // BackBufferInfo GetNextBackBufferInfo(RHIViewport* _viewport);

    // TextureView GetBackBuffer(RHIViewport* _viewport, uint32_t _index);

    // void PresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence);
    RENDER_API void FlushPendingDeletes();

    RENDER_API const EShaderPlatform GetShaderPlatform() const;

    ERHIType GetRHIType() const {
        return rhi_type;
    }

    RENDER_API PipelineHandle
    CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders); //gfx
    RENDER_API PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders);     //compute

    RENDER_API CommandQueue& GetCommandQueue(EQueueType _type);

    RENDER_API CopyQueue& GetCopyQueue();

    RENDER_API SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info);

    RENDER_API FenceRef CreateFence();

    RENDER_API RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo& _init);

    RENDER_API RaytracingSceneRef CreateRaytracingScene();

    class Impl;

    RENDER_API Impl* GetImpl() const {
        return impl.get();
    }

    template<DeviceExt Ext>
    RENDER_API Ext* LoadExtension() const;

    RENDER_API void FlushDebugMessages() const;
    RENDER_API void WaitIdle();

protected:
    RENDER_API IOInterfaceRef CreateIOInterface(CopyQueue& _copy_queue);

private:
    RENDER_API BufferRef CreateBuffer(
        std::string_view  _name,
        uint              _element_cnt,
        uint              _stride,
        EBufferUsageFlags _usage,
        EPixelFormat      _format
    );

    RenderDevice() = default;
    UniquePtr<Impl> impl;
    ERHIType        rhi_type;

    DeviceConfig config;
};

class RENDER_API GPUCapturer {
public:
    virtual ~GPUCapturer() = default;

    virtual void Begin(const std::filesystem::path& outputFilename) = 0;

    virtual void End() = 0;
};

#ifdef PLATFORM_WINDOWS                                // && ENABLE_D3D12
RENDER_API UniquePtr<GPUCapturer> CreatePIXCapturer(); // call this before create device
//// not sure add this directly into RenderDevice
//RENDER_API void BeginCapture(GPUCapturer* capturer, const std::filesystem::path& outputFilename) {capturer->Begin(outputFilename); }
//RENDER_API void EndCapture(GPUCapturer* capturer) { capturer->End(); }
#endif
}; // namespace Moer::Render

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
