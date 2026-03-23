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
#include <functional>
#include <mutex>
#include <string_view>
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

    // 当多个 VkNativeQueue 实例共享同一个 VkQueue handle 时，
    // 必须通过同一把 mutex 互斥 vkQueueSubmit2，否则违反 Vulkan 线程安全要求。
    void SetSubmitMutex(std::mutex* _mutex) { submit_mutex = _mutex; }

    void BeginLabel(std::string_view _label, float4 _color);
    void EndLabel();
    void InsertLabel(std::string_view _label, float4 _color);

private:
    Array<VkSemaphoreSubmitInfo> wait_infos;
    Array<VkSemaphoreSubmitInfo> signal_infos;
    VkQueue                      queue;
    EQueueType                   type;
    
    // 这个锁只在AMD GPU上使用，因为AMD GPU没有TransferQueue
    // 在现代NVIDIA GPU上，这个锁不会被触发，接近0开销，不用在意性能
    std::mutex*                  submit_mutex = nullptr;
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

    VkCommandQueue(VulkanDevice& _device, EQueueType _type) :
        CommandQueue(),
        vk_device(_device),
        queue(_type, _device),
        timestamp_pool(
            _device,
            VK_QUERY_TYPE_TIMESTAMP,
            s_queue_max_frame_in_flight * s_query_max_storage * 4
        ),
        profiler_storage(timestamp_pool) {
        timeline = MoerNew(VulkanFence(vk_device));
        enabled  = true;
        thread   = std::jthread(&VkCommandQueue::ExecuteThread, this);
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
    WaitEvent   Execute(CmdSubmit&& _submit) override;
    void        Wait(WaitEvent _event) override;
    void        Present(SwapchainRef _viewport, TextureView _view) override;
    void        Sync() override;
    ProfileData GetProfilerEntry() override;

    void SetQueueSubmitMutex(std::mutex* _mutex) { queue.SetSubmitMutex(_mutex); }

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
    VkNativeQueryPool                                  timestamp_pool;
    ProfilerStorage                                    profiler_storage;
    ProfileData                                        cached_profiler_entry;

    std::mutex   exec_mtx;
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