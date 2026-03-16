#pragma once
#include "PixelFormat.h"
#include "VulkanAllocator.h"
#include "VulkanCommand.h"
#include "VulkanQueryRuntime.h"
#include "RHICmdReorderer.h"
// #include "VulkanDevice.h"
#include "misc/LockFree.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"
#include "vulkan/vulkan_core.h"
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
namespace Moer::Render {
static constexpr uint s_queue_max_frame_in_flight = 3;
static constexpr uint s_query_max_storage         = 8 * 64;
struct QueryCmd;
class VkNativeQueue {
public:
    VkNativeQueue(EQueueType _type, VulkanDevice& _device);
    ~VkNativeQueue();

    void Submit(VulkanCmdList& _cmdlist, VkFence _fence = VK_NULL_HANDLE);
    void SubmitEmpty(VkFence _fence = VK_NULL_HANDLE);
    void Wait(
        VulkanFence*          _fence,
        uint64                _timeline,
        VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
    );
    void Wait(VkSemaphore _sem, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    void Signal(
        VulkanFence*          _fence,
        uint64                _timeline,
        VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
    );
    void Signal(VkSemaphore _semaphore, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    VkQueue GetHandle() const {
        return queue;
    }
    EQueueType GetType() const {
        return type;
    }

    void BeginLabel(std::string_view _label, float4 _color);
    void EndLabel();
    void InsertLabel(std::string_view _label, float4 _color);

private:
    Array<VkSemaphoreSubmitInfo> wait_infos;
    Array<VkSemaphoreSubmitInfo> signal_infos;
    VkQueue                      queue;
    EQueueType                   type;
};

struct ProfilerStorage {
    ProfilerStorage(VulkanQueryRuntime& _query_runtime);
    void CollectProfiling();
    bool IsActive() const {
        return active;
    }
    void RegisterCpuTimestamp(std::string_view _name, double _timestamp) {
        cpu_timestamps[cur_frame][_name] = _timestamp;
    }
    void AdvanceFrame() {
        cur_frame = (cur_frame + 1) % s_queue_max_frame_in_flight;
    }
    ProfileData GetProfilerEntry() {
        ProfileData               data{};
        Array<ProfileResultEntry> entries;
        entries.reserve(name2sample.size());
        uint last_frame = (cur_frame + s_queue_max_frame_in_flight - 1) % s_queue_max_frame_in_flight;
        for (auto& [name, sample] : name2sample) {
            if (sample.accumulated_ns > 0) {
                entries.push_back({name, double(sample.accumulated_ns) / (Sample::s_range * 1e6)});
            }
        }
        data.gpu_entries = std::move(entries);
        //cpu timestamp
        data.cpu_entries.reserve(cpu_timestamps[last_frame].size());
        for (auto& [name, timestamp] : cpu_timestamps[last_frame]) {
            data.cpu_entries.emplace_back(name.data(), timestamp);
        }
        return data;
    }

    void BeginProfilerSession(VulkanCmdList& _cmd, std::string_view _name);
    void EndProfilerSession(VulkanCmdList& _cmd, std::string_view _name);
    void VisitQueryCmd(VulkanCmdList& _cmd, const QueryCmd& _query_cmd);

    struct ResolvedGpuSample {
        std::string name{};
        uint64_t    begin_tick{0};
        uint64_t    end_tick{0};
    };
    const Array<ResolvedGpuSample>& GetResolvedGpuSamples() const {
        return resolved_gpu_samples;
    }

    bool active = false;
    VulkanQueryRuntime& query_runtime;
    float               timestamp_period = 0.0f;
    StaticArray<UnorderedMap<std::string_view, double>, s_queue_max_frame_in_flight> cpu_timestamps;

    struct Sample {
        uint64_t                      accumulated_ns;
        size_t                        next_idx_to_store;
        static constexpr int          s_range = 13;
        std::array<uint64_t, s_range> durations_ns;

        Sample() : accumulated_ns(0), next_idx_to_store(0) {
            std::memset(durations_ns.data(), 0, sizeof(durations_ns));
        }

        void Reset() {
            next_idx_to_store = 0;
            accumulated_ns    = 0;
            std::memset(durations_ns.data(), 0, sizeof(durations_ns));
        }
        void Record(uint64_t _duration_ns) {
            next_idx_to_store = (next_idx_to_store + 1) % s_range;
            accumulated_ns -= durations_ns[next_idx_to_store];
            durations_ns[next_idx_to_store] = _duration_ns;
            accumulated_ns += _duration_ns;
        }
    };

    struct PendingSample {
        std::string name{};
        QueryToken  token{};
    };

    UnorderedMap<std::string, Sample>            name2sample;
    UnorderedMap<std::string, Array<QueryToken>> active_scope_queries;
    Array<PendingSample>                         pending_samples{};
    Array<ResolvedGpuSample>                     resolved_gpu_samples{};

    uint64 cur_frame = 0;
};

struct VulkanRecordedSubmit {
    std::optional<CmdSubmit>   submit{};
    UniquePtr<VulkanAllocator> allocator{};
    bool                       has_cmd{false};
    double                     reorder_time_ms{0.0};
    double                     preprocess_time_ms{0.0};
};

class VkCommandQueue : public CommandQueue {
public:
    struct FencePlaceHoler {};

    using EventType = std::variant<
        UniquePtr<VulkanAllocator>,
        UniquePtr<VulkanPresentor>,
        Array<std::function<void()>>,
        VulkanFence*,
        SignalEvent,
        WaitEvent,
        FencePlaceHoler>;

    struct QueueEvent {
        EventType event;
        uint64    timeline;
        bool      wake_thread;
        template<typename Arg>
            requires std::is_constructible_v<EventType, Arg&&>
        QueueEvent(Arg&& _event, uint64 _timeline, bool _wake_thread) :
            event(std::forward<Arg>(_event)),
            timeline(_timeline),
            wake_thread(_wake_thread) {}

        QueueEvent(QueueEvent&& _other) noexcept :
            event(std::move(_other.event)),
            timeline(_other.timeline),
            wake_thread(_other.wake_thread) {}
    };

    VkCommandQueue(VulkanDevice& _device, EQueueType _type) :
        CommandQueue(),
        vk_device(_device),
        queue(_type, _device),
        query_runtime(_device),
        profiler_storage(query_runtime) {
        timeline = MoerNew(VulkanFence(vk_device));
        thread   = std::jthread(&VkCommandQueue::ExecuteThread, this);
        enabled  = true;
    }

    ~VkCommandQueue() {
        enabled = false;
        queue_cv.notify_all();
        thread.join();
        //clear allocators
        Array<VulkanAllocator*> allocs;
        allocators.PopAll(allocs);
        // uint32 alloc_count = 0;
        for (auto& allocator : allocs) {
            MoerDelete(allocator);
            // ++alloc_count
        }
        // LOG_INFO("Allocator count {}", alloc_count);
        Array<VulkanPresentor*> presents;
        presentors.PopAll(presents);
        // uint32 present_count = 0;
        for (auto& presentor : presents) {
            MoerDelete(presentor);
            // present_count++;
        }
        // LOG_INFO("Presentor count {}", present_count);
        MoerDelete(timeline);
    }
    VulkanRecordedSubmit Translate(CmdSubmit&& _submit, const CmdReorderer* _reordered = nullptr);
    WaitEvent            SubmitRecorded(VulkanRecordedSubmit&& _recorded);
    WaitEvent            SubmitRestoreTransitions(
                   Array<ReadBuffer>&& _read_buffers,
                   Array<ReadTexture>&& _read_textures,
                   std::optional<WaitEvent> _wait_evt = std::nullopt
               );
    WaitEvent   Execute(CmdSubmit&& _submit) override;
    void        Wait(WaitEvent _event) override;
    void        Present(SwapchainRef _viewport, TextureView _view) override;
    void        Present(SwapchainRef _viewport, TextureView _view, std::span<const WaitEvent> _wait_events);
    void        Sync() override;
    ProfileData GetProfilerEntry() override;

    void                                      ExecuteThread();
    VulkanDevice&                             vk_device;
    LockFreeQueueBase<VulkanAllocator, false> allocators;
    LockFreeQueueBase<VulkanPresentor, false> presentors;
    DEQueue<QueueEvent>                       event_queue;

#if WITH_CUDA
public:
    VkNativeQueue& GetVkNativeQueue() {
        return queue;
    }
#endif

private:
    UniquePtr<VulkanAllocator> GetAllocator();
    UniquePtr<VulkanPresentor> GetPresentor();
    void                       Complete(uint64 _timeline);
    void                       Signal();
#if defined(MOER_TRACE_ENABLED) && MOER_TRACE_ENABLED && defined(MOER_TRACE_GPU_ENABLED) && \
    MOER_TRACE_GPU_ENABLED
    void EmitResolvedGpuTraceEvents(const Array<ProfilerStorage::ResolvedGpuSample>& _samples);
#endif

private:
    uint64                                             last_frame = 0;
    CircularQueue<uint64, s_queue_max_frame_in_flight> executed_queue;
    std::atomic<uint64>                                executed_frame = 0;
    CircularQueue<uint64, s_queue_max_frame_in_flight> presented_queue;
    VulkanFence*                                       timeline = nullptr;
    std::mutex                                         event_mutex;
    bool                                               enabled{false};
    std::condition_variable                            queue_cv; // wake up execute thread from sleeping
    VkNativeQueue                                      queue;
    VulkanQueryRuntime                                 query_runtime;
    ProfilerStorage                                    profiler_storage;
    ProfileData                                        cached_profiler_entry;
    Array<ProfilerStorage::ResolvedGpuSample>          cached_gpu_samples;
#if defined(MOER_TRACE_ENABLED) && MOER_TRACE_ENABLED && defined(MOER_TRACE_GPU_ENABLED) && \
    MOER_TRACE_GPU_ENABLED
    uint64_t gpu_trace_tick_anchor{0};
    uint64_t gpu_trace_time_anchor_ns{0};
    bool     gpu_trace_anchor_valid{false};
#endif

    std::mutex   exec_mtx;
    std::mutex   alloc_mtx;
    std::atomic<uint64> record_frame{0};
    std::jthread thread;
};
class VkCopyQueue : public CopyQueue {
public:
    friend VulkanDevice;
    VkCopyQueue(VulkanDevice& _device);
    ~VkCopyQueue();
    struct Placeholder {};
    using EventType = std::variant<
        UniquePtr<VulkanAllocator>,
        UniquePtr<VulkanPresentor>,
        Array<std::function<void()>>,
        IOSignalEvt,
        IOWaitEvt,
        Placeholder>;
    struct IOEvent {
        EventType event;
        uint64    timeline;
        bool      wake_thread;
        template<typename Arg>
            requires std::is_constructible_v<EventType, Arg&&>
        IOEvent(Arg&& _event, uint64 _timeline, bool _wake_thread) :
            event(std::forward<Arg>(_event)),
            timeline(_timeline),
            wake_thread(_wake_thread) {}

        IOEvent(IOEvent&& _other) noexcept :
            event(std::move(_other.event)),
            timeline(_other.timeline),
            wake_thread(_other.wake_thread) {}
    };

    IOWaitEvt Execute(IOQueueSubmission&& _submit) override;
    VulkanRecordedSubmit Translate(CmdSubmit&& _submit, const CmdReorderer* _reordered = nullptr);
    IOWaitEvt            SubmitRecorded(VulkanRecordedSubmit&& _recorded);
    IOWaitEvt Execute(CmdSubmit&& _submit) override;
    FenceRef  GetFenceHandle() override;
    void      Sync(uint64 _timeline) override;

    virtual void CopyFrom(BufferView _src, BufferView _dst) override {};
    virtual void CopyFrom(TextureView _src, TextureView _dst) override {};
    virtual void CopyFrom(TextureView _src, BufferView _dst) override {};
    virtual void CopyFrom(BufferView _src, TextureView _dst) override {};
    virtual void CopyFrom(std::span<byte> _data, BufferView _dst) override {};
    virtual void CopyFrom(std::span<byte> _data, TextureView _dst) override {};

    ///
    void Enqueue(FileHandle _handle, size_t _file_offset, void* _ptr, size_t _len);
    void Enqueue(FileHandle _handle, size_t _file_offset, void* _buffer_ptr, size_t _offset, size_t _len);
    void Enqueue(
        FileHandle   _handle,
        size_t       _file_offset,
        void*        _tex_ptr,
        EPixelFormat _format,
        uint3        _offset,
        uint3        _size,
        uint         _mip_offset
    );
    void Enqueue(void const* _mem, size_t _file_offset, void* _buffer_ptr, size_t _offset, size_t _len);
    void Enqueue(
        void const*  _mem,
        size_t       _file_offset,
        void*        _tex_ptr,
        EPixelFormat _format,
        uint3        _offset,
        uint3        _size,
        uint         _mip_offset
    );
    void EnqueueSignal(FenceRef _fence, uint64_t _timeline);
    void Execute();

    void ExecuteThread();
    void Submit(IOQueueCommandList&& _cmd_list);
    void SubmitSignal(FenceRef _fence, uint64_t _timeline);
    void SubmitWait(FenceRef _fence, uint64_t _timeline);

private:
    void ExecuteIOThread(IOQueueCommandList&& _cmd_list, uint64_t _timeline);

    void IOThreadLoop();
    void RHIThreadLoop();

private:
    UniquePtr<VulkanAllocator>                GetAllocator();
    void                                      Complete(uint64 _timeline);
    LockFreeQueueBase<VulkanAllocator, false> allocators;
    DEQueue<IOEvent>                          event_queue;

private:
    VulkanDevice& device;

    uint                    last_frame     = 0;
    std::atomic<uint64>     executed_frame = 0;
    VulkanFenceRef          timeline       = nullptr;
    std::mutex              event_mutex;
    bool                    enabled{false};
    std::condition_variable queue_cv; // wake up execute thread from sleeping
    VkNativeQueue           queue;
    std::jthread            thread;

    //tmp
    LockFreeQueueBase<IOCmd> commands;

    std::mutex                                     exec_mutex;
    std::mutex                                     alloc_mtx;
    Array<VkSemaphore>                             pending_semaphores;
    Queue<std::pair<IOQueueCommandList&&, uint64>> io_thread_cmds;
    std::mutex                                     io_mutex;
    Queue<std::pair<CommandList&&, uint64>>        io_rhi_cmdlists;
    std::mutex                                     rhi_mutex;
};
} // namespace Moer::Render
