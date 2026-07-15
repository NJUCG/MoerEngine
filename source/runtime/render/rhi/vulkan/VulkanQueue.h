#pragma once
#include "PixelFormat.h"
#include "VulkanAllocator.h"
#include "VulkanCommand.h"
// #include "VulkanDevice.h"
#include "misc/LockFree.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"
#include "vulkan/vulkan_core.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <variant>
namespace Moer::Render {
static constexpr uint s_queue_max_frame_in_flight = 3;
static constexpr uint s_query_max_storage         = 8 * 64;
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

    // Logical queues that alias one VkQueue must share one host synchronization mutex.
    void SetSubmitMutex(std::mutex* _mutex) {
        assert(_mutex != nullptr);
        submit_mutex = _mutex;
    }

    void BeginLabel(std::string_view _label, float4 _color);
    void EndLabel();
    void InsertLabel(std::string_view _label, float4 _color);

private:
    Array<VkSemaphoreSubmitInfo> wait_infos;
    Array<VkSemaphoreSubmitInfo> signal_infos;
    VkQueue                      queue;
    EQueueType                   type;
    
    // A local mutex covers standalone use before the device installs its canonical queue mutex.
    std::mutex                   local_submit_mutex;
    std::mutex*                  submit_mutex = &local_submit_mutex;
};

struct ProfilerStorage {
    static constexpr int s_max_num_profiler_queries_per_frame =
        s_query_max_storage * 2; // *2 is for begin&end
    static constexpr int s_total_query_count =
        s_max_num_profiler_queries_per_frame * s_queue_max_frame_in_flight;

    ProfilerStorage(VkNativeQueryPool& _timestamp_pool);
    void CollectProfiling(VkCommandBuffer _cmd);
    int  GetQueryStorageIndex(std::string_view _name);
    bool IsQueryUsed(int _idx) {
        return queries_used[_idx / 8] & (1 << (_idx % 8));
    }
    void SetQueryUsed(int _idx) {
        queries_used[_idx / 8] |= (1 << (_idx % 8));
    }
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
            if (sample.accumulated > 0)
                entries.push_back(
                    {name, double(sample.accumulated) / (Sample::s_range * 1e6) * timestamp_period}
                );
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

    bool               active = false;
    VkNativeQueryPool& timestamp_pool;
    float              timestamp_period = 0.0f;
    uint64             query_pool_results[s_max_num_profiler_queries_per_frame * 2];
    StaticArray<UnorderedMap<std::string_view, double>, s_queue_max_frame_in_flight> cpu_timestamps;
    // each result contain two int(VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
    // first int contains the queried timestamp
    // next(last) int contains availability of the timestamp, available if nonzero

    struct Sample {
        int                           index;
        uint64_t                      accumulated;
        size_t                        next_idx_to_store;
        static constexpr int          s_range = 13;
        std::array<uint64_t, s_range> deltas;

        Sample(int _idx) : index(_idx), accumulated(0), next_idx_to_store(0) {
            std::memset(deltas.data(), 0, sizeof(deltas));
        }
        Sample() : Sample(0) {}

        void Reset() {
            next_idx_to_store = 0;
            accumulated       = 0;
            std::memset(deltas.data(), 0, sizeof(deltas));
        }
        void Record(uint64_t _value) {
            next_idx_to_store = (next_idx_to_store + 1) % s_range;
            accumulated -= deltas[next_idx_to_store];
            deltas[next_idx_to_store] = _value;
            accumulated += _value;
        }
    };

    UnorderedMap<std::string, Sample> name2sample;
    ubyte                             queries_used[s_total_query_count / 8];

    uint64 cur_frame = 0;
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

    struct RhiExecuteWork {
        CmdSubmit                             submit;
        uint64                                timeline;
        uint64                                serial;
        std::chrono::steady_clock::time_point enqueued_at;
        uint32                                enqueue_depth;
        double                                caller_ms{0.0};

        RhiExecuteWork(
            CmdSubmit&&                           _submit,
            uint64                                _timeline,
            uint64                                _serial,
            std::chrono::steady_clock::time_point _enqueued_at,
            uint32                                _enqueue_depth
        ) :
            submit(std::move(_submit)),
            timeline(_timeline),
            serial(_serial),
            enqueued_at(_enqueued_at),
            enqueue_depth(_enqueue_depth) {}
    };

    struct RhiPresentWork {
        SwapchainRef                          swapchain;
        TextureRef                            source_texture;
        TextureView                           source_view;
        uint64                                timeline;
        uint64                                serial;
        std::chrono::steady_clock::time_point enqueued_at;
        uint32                                enqueue_depth;
        double                                caller_ms{0.0};

        RhiPresentWork(
            SwapchainRef&&                       _swapchain,
            TextureRef&&                         _source_texture,
            TextureView                          _source_view,
            uint64                               _timeline,
            uint64                               _serial,
            std::chrono::steady_clock::time_point _enqueued_at,
            uint32                               _enqueue_depth
        ) :
            swapchain(std::move(_swapchain)),
            source_texture(std::move(_source_texture)),
            source_view(_source_view),
            timeline(_timeline),
            serial(_serial),
            enqueued_at(_enqueued_at),
            enqueue_depth(_enqueue_depth) {}
    };

    using RhiWork = std::variant<RhiExecuteWork, RhiPresentWork>;

    VkCommandQueue(
        VulkanDevice& _device,
        EQueueType    _type,
        bool          _enable_rhi_thread = false,
        bool          _thread_profile_logging = false
    );
    ~VkCommandQueue();
    WaitEvent   Execute(CmdSubmit&& _submit) override;
    void        Wait(WaitEvent _event) override;
    void        Present(SwapchainRef _viewport, TextureView _view) override;
    void        Sync() override;
    ProfileData GetProfilerEntry() override;

    void SetQueueSubmitMutex(std::mutex* _mutex) { queue.SetSubmitMutex(_mutex); }

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
    enum class ERhiWorkKind : uint8 { Execute, Present };

    struct RhiWorkProfileTotals {
        uint64 samples{0};
        double caller_total_ms{0.0};
        double queue_wait_total_ms{0.0};
        double work_total_ms{0.0};
    };

    void                       ExecuteNow(CmdSubmit&& _submit, uint64 _timeline);
    void PresentNow(
        SwapchainRef&& _swapchain,
        TextureRef&&   _source_texture,
        TextureView    _source_view,
        uint64         _timeline
    );
    void                       RhiThreadMain();
    void                       CompletionThreadMain();
    void                       AppendDeferredReleaseCallbacks(Array<std::function<void()>>& _callbacks);
    void                       EnqueueCompletionMarker(uint64 _timeline);
    UniquePtr<VulkanAllocator> GetAllocator();
    UniquePtr<VulkanPresentor> GetPresentor();
    void                       Complete(uint64 _timeline);
    void                       RecordThreadingProfile(
        ERhiWorkKind _kind,
        double       _caller_ms,
        double       _queue_wait_ms,
        double       _work_ms,
        uint32       _enqueue_depth
    );

private:
    std::atomic<uint64>                                last_frame{0};
    uint64                                             descriptor_submission{0};
    CircularQueue<uint64, s_queue_max_frame_in_flight> executed_queue;
    std::atomic<uint64>                                executed_frame = 0;
    CircularQueue<uint64, s_queue_max_frame_in_flight> presented_queue;
    VulkanFence*                                       timeline = nullptr;
    std::mutex                                         event_mutex;
    bool                                               completion_worker_running{false};
    std::condition_variable                            queue_cv;
    VkNativeQueue                                      queue;
    VkNativeQueryPool                                  timestamp_pool;
    ProfilerStorage                                    profiler_storage;
    ProfileData                                        cached_profiler_entry;
    std::mutex                                         profiler_mutex;

    bool                    rhi_thread_enabled{false};
    bool                    thread_profile_logging{false};
    bool                    rhi_worker_running{false};
    std::atomic<uint32_t>   rhi_thread_id{0};
    uint64                  enqueued_rhi_work{0};
    uint64                  completed_rhi_work{0};
    DEQueue<RhiWork>        rhi_work_queue;
    std::mutex              rhi_work_mutex;
    std::condition_variable rhi_work_cv;
    std::condition_variable rhi_work_done_cv;

    std::chrono::steady_clock::time_point thread_profile_window_start =
        std::chrono::steady_clock::now();
    uint64 thread_profile_samples{0};
    RhiWorkProfileTotals thread_profile_execute{};
    RhiWorkProfileTotals thread_profile_present{};
    double thread_profile_caller_total_ms{0.0};
    double thread_profile_caller_max_ms{0.0};
    double thread_profile_queue_wait_total_ms{0.0};
    double thread_profile_queue_wait_max_ms{0.0};
    double thread_profile_work_total_ms{0.0};
    double thread_profile_work_max_ms{0.0};
    uint32 thread_profile_max_enqueue_depth{0};

    std::mutex   exec_mtx;
    std::jthread completion_thread;
    std::jthread rhi_thread;
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
    IOWaitEvt Execute(CmdSubmit&& _submit) override;
    FenceRef  GetFenceHandle() override;
    void      Sync(uint64 _timeline) override;

    void SetQueueSubmitMutex(std::mutex* _mutex) { queue.SetSubmitMutex(_mutex); }

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
    Array<VkSemaphore>                             pending_semaphores;
    Queue<std::pair<IOQueueCommandList&&, uint64>> io_thread_cmds;
    std::mutex                                     io_mutex;
    Queue<std::pair<CommandList&&, uint64>>        io_rhi_cmdlists;
    std::mutex                                     rhi_mutex;
};
} // namespace Moer::Render
