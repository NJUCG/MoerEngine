#pragma once
#include "PixelFormat.h"
#include "VulkanAllocator.h"
#include "VulkanCommand.h"
#include "VulkanQueryRuntime.h"
#include "RHICmdReorderer.h"
#include "VulkanResourceTracker.h"
// #include "VulkanDevice.h"
#include "misc/LockFree.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"
#include "string/StringConvert.h"
#include "vulkan/vulkan_core.h"
#include <functional>
#include <chrono>
#include <mutex>
#include <optional>
#include <span>
namespace Moer::Render {
static constexpr uint s_queue_max_frame_in_flight = 3;
static constexpr uint s_query_max_storage         = 8 * 64;
struct QueryCmd;
class VkCommandQueue;
class VkCopyQueue;
class SubmissionPresentContext;

struct PendingProfilerSample {
    String     name{};
    QueryToken token{};
};

struct ProfilerSubmitData {
    UnorderedMap<String, double> cpu_timestamps{};
    Array<PendingProfilerSample> pending_samples{};
};

enum class EVulkanSubmitPayloadType : uint8 {
    Queue,
    Copy,
    Present,
};

enum class EVulkanSubmitPayloadPhase : uint8 {
    Recording,
    Wait,
    Signal,
    Closed,
};

struct VulkanSubmitPayload {
    using Clock = std::chrono::steady_clock;

    EVulkanSubmitPayloadType  type{EVulkanSubmitPayloadType::Queue};
    EVulkanSubmitPayloadPhase phase{EVulkanSubmitPayloadPhase::Recording};
    EQueueType                queue_type{EQueueType::Ignore};
    VkCommandQueue*           queue_owner{nullptr};
    VkCopyQueue*              copy_queue_owner{nullptr};
    SubmissionPresentContext* present_context{nullptr};
    VulkanCmdList*            cmd_list{nullptr};
    WaitEvent                 completion{};
    VkFence                   host_fence{VK_NULL_HANDLE};
    bool                      owns_host_fence{false};
    bool                      host_fence_failed{false};
    bool                      has_frame_boundary_event{false};
    bool                      tick_profiling{false};
    bool                      scheduled_completion{false};
    uint64                    timeline_value{0};
    uint64                    op_seq{0};
    GraphEventRef             completion_event{nullptr};
    Clock::time_point         pending_since{};
    uint32                    pending_warn_count{0};
    UniquePtr<VulkanAllocator> allocator_owner{};
    std::optional<VulkanQueryRuntime::SubmissionState> query_submission{};
    std::optional<ProfilerSubmitData> profiler_submit_data{};
    UniquePtr<VulkanPresentor> presentor{};
    Array<std::function<void()>> callbacks{};
    Array<GraphEventRef>       completion_events{};
    Array<WaitEvent>           wait_events{};
    Array<SignalEvent>         signal_events{};
    Array<GPUEvent>            gpu_events{};

    bool HasCommandBuffer() const {
        return cmd_list != nullptr;
    }

    bool HasCompletionWork() const {
        return allocator_owner != nullptr || query_submission.has_value() || !callbacks.empty() ||
               !completion_events.empty() || !signal_events.empty() || !gpu_events.empty() ||
               presentor != nullptr;
    }
};

class VulkanQueueAccessScope {
public:
    VulkanQueueAccessScope(VkQueue queue, StringView operation);
    ~VulkanQueueAccessScope();

    VulkanQueueAccessScope(const VulkanQueueAccessScope&) = delete;
    VulkanQueueAccessScope& operator=(const VulkanQueueAccessScope&) = delete;

private:
    VkQueue queue{VK_NULL_HANDLE};
    uint64  thread_id{0};
    bool    active{false};
};

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
    bool SupportsTimestampQueries() const {
        return supports_timestamp_queries;
    }

    // 当多个 VkNativeQueue 实例共享同一个 VkQueue handle 时，
    // 必须通过同一把 mutex 互斥 vkQueueSubmit2，否则违反 Vulkan 线程安全要求。
    void SetSubmitMutex(std::mutex* _mutex) { submit_mutex = _mutex; }

    void BeginLabel(StringView _label, float4 _color);
    void EndLabel();
    void InsertLabel(StringView _label, float4 _color);

private:
    Array<VkSemaphoreSubmitInfo> wait_infos;
    Array<VkSemaphoreSubmitInfo> signal_infos;
    VkQueue                      queue;
    EQueueType                   type;
    bool                         supports_timestamp_queries{false};
    bool                         validation_layer_enabled{false};
    
    // 这个锁只在AMD GPU上使用，因为AMD GPU没有TransferQueue
    // 在现代NVIDIA GPU上，这个锁不会被触发，接近0开销，不用在意性能
    std::mutex*                  submit_mutex = nullptr;
};

struct ProfilerStorage {
    ProfilerStorage(VulkanQueryRuntime& _query_runtime);
    void CollectProfiling();
    ProfilerSubmitData DrainSubmitData();
    void MergeSubmitData(ProfilerSubmitData&& submit_data);
    bool IsActive() const {
        return active;
    }
    void RegisterCpuTimestamp(StringView _name, double _timestamp) {
        cpu_timestamps[cur_frame][String(_name)] = _timestamp;
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
            data.cpu_entries.emplace_back(name, timestamp);
        }
        return data;
    }

    void BeginProfilerSession(VulkanCmdList& _cmd, StringView _name);
    void EndProfilerSession(VulkanCmdList& _cmd, StringView _name);
    void VisitQueryCmd(VulkanCmdList& _cmd, const QueryCmd& _query_cmd);

    struct ResolvedGpuSample {
        String name{};
        uint64_t    begin_tick{0};
        uint64_t    end_tick{0};
    };
    const Array<ResolvedGpuSample>& GetResolvedGpuSamples() const {
        return resolved_gpu_samples;
    }

    bool active = false;
    VulkanQueryRuntime& query_runtime;
    float               timestamp_period = 0.0f;
    StaticArray<UnorderedMap<String, double>, s_queue_max_frame_in_flight> cpu_timestamps;

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

    UnorderedMap<String, Sample>            name2sample;
    UnorderedMap<String, Array<QueryToken>> active_scope_queries;
    Array<PendingProfilerSample>                 pending_samples{};
    Array<ResolvedGpuSample>                     resolved_gpu_samples{};

    uint64 cur_frame = 0;
};

struct VulkanRecordedSubmit {
    std::optional<CmdSubmit>   submit{};
    UniquePtr<VulkanAllocator> allocator_owner{};
    Array<UniquePtr<VulkanSubmitPayload>> payloads{};
    bool                       has_cmd{false};
    double                     reorder_time_ms{0.0};
    double                     preprocess_time_ms{0.0};
};

class VkCommandQueue : public CommandQueue {
public:
    VkCommandQueue(VulkanDevice& _device, EQueueType _type) :
        CommandQueue(),
        vk_device(_device),
        queue(_type, _device),
        query_runtime(_device),
        profiler_storage(query_runtime) {
        timeline = MoerNew(VulkanFence(vk_device));
    }

    ~VkCommandQueue() {
        //clear allocators
        Array<VulkanAllocator*> allocs;
        allocators.PopAll(allocs);
        // uint32 alloc_count = 0;
        for (auto& allocator : allocs) {
            MoerDelete(allocator);
            // ++alloc_count
        }
        // LOG_INFO(MOER_TEXT("Allocator count {}"), alloc_count);
        MoerDelete(timeline);
    }
    VulkanRecordedSubmit Translate(
        CmdSubmit&& _submit,
        const CmdReorderer* _reordered = nullptr,
        TrackerSeed seed = {},
        VulkanAllocator* allocator_override = nullptr
    );
    UniquePtr<VulkanSubmitPayload> SubmitPayloadForRuntime(
        UniquePtr<VulkanSubmitPayload>&& payload,
        std::span<const WaitEvent>      runtime_waits,
        uint64                          signal_value,
        VkFence                         _submit_fence = VK_NULL_HANDLE
    );

    WaitEvent   Execute(CmdSubmit&& _submit) override;
    void        Wait(WaitEvent _event) override;
    void        Present(SwapchainRef _viewport, TextureView _view) override;
    void        Sync() override;
    ProfileData GetProfilerEntry() override;
    uint64 GetTimelineHandle() const { return uint64(timeline); }
    void MarkExecutionComplete(uint64 _timeline);
    void SetQueueSubmitMutex(std::mutex* _mutex) { queue.SetSubmitMutex(_mutex); }

    void ResolveAllocatorCompletion(
        UniquePtr<VulkanAllocator>&& _allocator,
        uint64                        _timeline
    );
    void ResolveQueryCompletion(VulkanQueryRuntime::SubmissionState&& _submission);
    void AppendCompletionBoundary(const GraphEventRef& completion_event);
    void ResetCompletionBoundary() { completion_boundary = nullptr; }
    const GraphEventRef& GetCompletionBoundary() const { return completion_boundary; }
    VulkanDevice&                             vk_device;
    LockFreeQueueBase<VulkanAllocator, false> allocators;

#if WITH_CUDA
public:
    VkNativeQueue& GetVkNativeQueue() {
        return queue;
    }
#endif

private:
    UniquePtr<VulkanAllocator> GetAllocator();
    void                       Complete(uint64 _timeline);

private:
    uint64                                             last_frame = 0;
    std::atomic<uint64>                                executed_frame = 0;
    VulkanFence*                                       timeline = nullptr;
    VkNativeQueue                                      queue;
    VulkanQueryRuntime                                 query_runtime;
    ProfilerStorage                                    profiler_storage;
    GraphEventRef completion_boundary{nullptr};
};
class VkCopyQueue : public CopyQueue {
public:
    friend VulkanDevice;
    VkCopyQueue(VulkanDevice& _device);
    ~VkCopyQueue();

    IOWaitEvt Execute(IOQueueSubmission&& _submit) override;
    VulkanRecordedSubmit Translate(
        CmdSubmit&& _submit,
        const CmdReorderer* _reordered = nullptr,
        VulkanAllocator* allocator_override = nullptr
    );
    UniquePtr<VulkanSubmitPayload> SubmitPayloadForRuntime(
        UniquePtr<VulkanSubmitPayload>&& payload,
        std::span<const WaitEvent>      runtime_waits,
        uint64                          signal_value,
        VkFence                         _submit_fence = VK_NULL_HANDLE
    );
    IOWaitEvt Execute(CmdSubmit&& _submit) override;
    FenceRef  GetFenceHandle() override;
    uint64    GetTimelineHandle() const { return uint64(timeline.Get()); }
    VulkanDevice& GetDevice() { return device; }
    void      Sync(uint64 _timeline) override;
    void      MarkExecutionComplete(uint64 _timeline);
    void      ResolveAllocatorCompletion(
               UniquePtr<VulkanAllocator>&& _allocator,
               uint64                        _timeline
           );
    void      AppendCompletionBoundary(const GraphEventRef& completion_event);
    void      ResetCompletionBoundary() { completion_boundary = nullptr; }
    const GraphEventRef& GetCompletionBoundary() const { return completion_boundary; }

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

private:
    UniquePtr<VulkanAllocator>                GetAllocator();
    void                                      Complete(uint64 _timeline);
    LockFreeQueueBase<VulkanAllocator, false> allocators;

private:
    VulkanDevice& device;

    uint                    last_frame     = 0;
    std::atomic<uint64>     executed_frame = 0;
    VulkanFenceRef          timeline       = nullptr;
    VkNativeQueue           queue;
    GraphEventRef                                  completion_boundary{nullptr};
};
} // namespace Moer::Render
