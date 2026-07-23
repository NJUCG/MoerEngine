#pragma once
#include "PixelFormat.h"
#include "VulkanAllocator.h"
#include "VulkanCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanFault.h"
// #include "VulkanDevice.h"
#include "misc/LockFree.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"
#include "rhi/ExternalCpuJoinPool.h"
#include "rhi/RHIParallelRecord.h"
#include "rhi/RHIRecordDiagnostics.h"
#include "vulkan/vulkan_core.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <variant>
namespace Moer::Render {
class CmdReorderer;
struct FunctionTable;

static constexpr uint s_queue_max_frame_in_flight = 3;
static constexpr uint s_query_max_storage         = 8 * 64;
class VkNativeQueue {
public:
    VkNativeQueue(EQueueType _type, VulkanDevice& _device);
    ~VkNativeQueue();

    VulkanOperationResult Submit(
        VulkanCmdList&               _cmdlist,
        const VulkanOperationContext& _context,
        VkFence                       _fence = VK_NULL_HANDLE
    );
    VulkanOperationResult Submit(
        std::span<VulkanCmdList* const> _cmdlists,
        const VulkanOperationContext&   _context,
        VkFence                         _fence = VK_NULL_HANDLE
    );
    VulkanOperationResult SubmitEmpty(
        const VulkanOperationContext& _context,
        VkFence                       _fence = VK_NULL_HANDLE
    );
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
    VulkanDevice&                device;
    VkQueue                      queue;
    EQueueType                   type;
    
    // A local mutex covers standalone use before the device installs its canonical queue mutex.
    std::mutex                   local_submit_mutex;
    std::mutex*                  submit_mutex = &local_submit_mutex;
};

struct QueryFrameDiagnostics {
    uint64 digest{0};
    uint32 used_query_count{0};
};

struct ProfilerStorage {
    static constexpr int s_max_num_profiler_queries_per_frame =
        s_query_max_storage * 2; // *2 is for begin&end
    static constexpr int s_total_query_count =
        s_max_num_profiler_queries_per_frame * s_queue_max_frame_in_flight;
    // These stages are part of the serial query golden contract.  Keep the
    // actual timestamp writes and the diagnostic event sourced from the same
    // constants so they cannot silently drift apart.
    static constexpr VkPipelineStageFlagBits kBeginTimestampStage =
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    static constexpr VkPipelineStageFlagBits kEndTimestampStage =
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    static constexpr VkPipelineStageFlags2 kResetQueryStage = VK_PIPELINE_STAGE_2_NONE;

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

    void BeginProfilerSession(
        VulkanCmdList&           _cmd,
        std::string_view         _name,
        VkPipelineStageFlagBits  _stage = kBeginTimestampStage
    );
    void EndProfilerSession(
        VulkanCmdList&           _cmd,
        std::string_view         _name,
        VkPipelineStageFlagBits  _stage = kEndTimestampStage
    );
    QueryFrameDiagnostics GetCurrentFrameQueryDiagnostics() const;

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

struct VulkanSubmissionEvent {
    VulkanOperationResult  outcome;
    VulkanOperationContext context;
    bool                   gpu_submitted{false};
};

// Result returned to the upper submission runtime.  A queue timeline is only
// publishable as a dependency when vkQueueSubmit/SubmitEmpty actually accepted
// the work; retry/recreate/rejected paths deliberately carry no completion.
struct VulkanRuntimeSubmissionResult {
    VulkanOperationResult   outcome{};
    std::optional<WaitEvent> completion{};

    [[nodiscard]] bool WasSubmitted() const {
        return completion.has_value();
    }

    [[nodiscard]] bool IsHardFailure() const {
        return outcome.status == EVulkanOperationStatus::Faulted ||
               outcome.status == EVulkanOperationStatus::Rejected;
    }
};

struct VulkanCallbackBatch {
    Array<std::function<void()>> callbacks;
    bool                         success_only{false};
};

struct VulkanAllocatorBatch {
    Array<UniquePtr<VulkanAllocator>> submitted;
    Array<UniquePtr<VulkanAllocator>> abandoned;
};

struct VulkanDeferredReleaseBatch {
    Array<RHIResource*> resources;
};

struct VulkanCompletionMarker {};

class VkCommandQueue : public CommandQueue {
public:
    using EventType = std::variant<
        VulkanSubmissionEvent,
        UniquePtr<VulkanAllocator>,
        VulkanAllocatorBatch,
        VulkanDescriptorPushLease,
        UniquePtr<VulkanPresentor>,
        VulkanCallbackBatch,
        VulkanDeferredReleaseBatch,
        VulkanCompletionMarker,
        VulkanFence*,
        SignalEvent,
        WaitEvent>;

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
        PresentReceiptRef                     receipt;
        uint64                                timeline;
        uint64                                serial;
        std::chrono::steady_clock::time_point enqueued_at;
        uint32                                enqueue_depth;
        double                                caller_ms{0.0};

        RhiPresentWork(
            SwapchainRef&&                       _swapchain,
            TextureRef&&                         _source_texture,
            TextureView                          _source_view,
            PresentReceiptRef                    _receipt,
            uint64                               _timeline,
            uint64                               _serial,
            std::chrono::steady_clock::time_point _enqueued_at,
            uint32                               _enqueue_depth
        ) :
            swapchain(std::move(_swapchain)),
            source_texture(std::move(_source_texture)),
            source_view(_source_view),
            receipt(std::move(_receipt)),
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
        bool          _thread_profile_logging = false,
        bool          _parallel_recording = false,
        uint32_t      _parallel_record_workers = 0,
        bool          _parallel_record_verify = false,
        bool          _parallel_record_profile = false,
        uint32_t      _parallel_record_min_work_units_per_job = 64,
        uint64_t      _parallel_record_worker_throw_trigger = 0
    );
    ~VkCommandQueue();
    WaitEvent   Execute(CmdSubmit&& _submit) override;
    void        Wait(WaitEvent _event) override;
    void Present(
        SwapchainRef      _viewport,
        TextureView       _view,
        PresentReceiptRef _receipt = {}
    ) override;
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
    friend class VulkanSubmissionExecutor;

    enum class ERhiWorkKind : uint8 { Execute, Present };

    struct RhiWorkProfileTotals {
        uint64 samples{0};
        double caller_total_ms{0.0};
        double queue_wait_total_ms{0.0};
        double work_total_ms{0.0};
    };

    struct RhiRecordExecuteSample {
        RecordTopologySummary topology;
        Array<RecordLayerTiming> layer_timings;
        double                   serial_command_sum_ms{0.0};
        StableRecordHash         barrier_hash;
        uint64                   descriptor_digest{StableRecordHash::kOffset};
        uint64                   query_digest{StableRecordHash::kOffset};
        uint64                   descriptor_bytes{0};
        uint32                   buffer_barriers{0};
        uint32                   texture_barriers{0};
        uint32                   memory_barriers{0};
        uint32                   used_queries{0};
        SerialGoldenSummary      serial_golden{};
        uint32                   golden_unresolved{0};
        uint32                   golden_opaque{0};
        uint64                   golden_unresolved_command_mask{0};
        uint64                   golden_opaque_command_mask{0};
        uint32                   golden_unresolved_native_buffers{0};
        uint32                   golden_unresolved_native_images{0};
        bool                     golden_has_unresolved_buffer_barrier{false};
        SerialBarrierItem        golden_first_unresolved_buffer_barrier{};
    };

    struct RhiRecordWindowTotals {
        uint64 samples{0};
        double serial_record_wall_total_ms{0.0};
        double serial_record_wall_max_ms{0.0};
        double serial_command_sum_total_ms{0.0};
        double eligible_record_total_ms{0.0};
        double predicted_critical_total_ms{0.0};
        double dispatch_join_estimate_total_ms{0.0};
        double predicted_net_saving_total_ms{0.0};
        uint64 layer_total{0};
        uint64 command_total{0};
        uint64 candidate_command_total{0};
        uint64 safe_command_total{0};
        uint64 parallel_layer_total{0};
        uint64 descriptor_bytes_total{0};
        uint64 buffer_barrier_total{0};
        uint64 texture_barrier_total{0};
        uint64 memory_barrier_total{0};
        uint64 used_query_total{0};
        uint32 layer_max{0};
        uint32 command_max{0};
        uint32 parallel_layer_max{0};
        uint64 topology_changes{0};
        uint64 golden_complete{0};
        uint64 golden_incomplete{0};
        uint64 golden_unresolved_total{0};
        uint64 golden_opaque_total{0};
        UnorderedSet<uint64> command_digests;
        UnorderedSet<uint64> layer_digests;
        UnorderedSet<uint64> barrier_digests;
        UnorderedSet<uint64> descriptor_digests;
        UnorderedSet<uint64> query_digests;
        UnorderedSet<uint64> golden_command_digests;
        UnorderedSet<uint64> golden_layer_digests;
        UnorderedSet<uint64> golden_barrier_digests;
        UnorderedSet<uint64> golden_descriptor_digests;
        UnorderedSet<uint64> golden_query_digests;
        UnorderedSet<uint64> golden_combined_digests;
        UnorderedSet<uint64> golden_manifest_entries;
    };

    struct RhiThreadProfileState {
        RhiThreadProfileState() : window_start(std::chrono::steady_clock::now()) {}

        std::chrono::steady_clock::time_point window_start;
        uint64                 samples{0};
        RhiWorkProfileTotals   execute{};
        RhiWorkProfileTotals   present{};
        double                 caller_total_ms{0.0};
        double                 caller_max_ms{0.0};
        double                 queue_wait_total_ms{0.0};
        double                 queue_wait_max_ms{0.0};
        double                 work_total_ms{0.0};
        double                 work_max_ms{0.0};
        uint32                 max_enqueue_depth{0};
        RhiRecordWindowTotals  record{};
        bool                   calibration_ready{false};
        uint32                 model_workers{1};
        double                 dispatch_join_median_ms{0.0};
        double                 dispatch_join_tail_ms{0.0};
        bool                   has_topology{false};
        uint64                 last_topology_digest{0};
        UnorderedSet<uint64>   observed_golden_manifests;
    };

    struct ParallelRecordProfileSample {
        bool   requested{false};
        bool   planned{false};
        bool   effective{false};
        bool   worker_fallback{false};
        double record_wall_ms{0.0};
        double execute_cpu_wall_ms{0.0};
        double reorder_ms{0.0};
        double preprocess_ms{0.0};
        double worker_join_ms{0.0};
        double submit_cpu_ms{0.0};
        uint32 layers{0};
        uint32 jobs{0};
        uint64 work_units{0};
        uint32 ordered_cb{0};
        uint32 max_active{0};
    };

    struct ParallelRecordProfileState {
        ParallelRecordProfileState() : window_start(std::chrono::steady_clock::now()) {}

        std::chrono::steady_clock::time_point window_start;
        Array<double> record_samples_ms;
        Array<double> execute_cpu_samples_ms;
        uint64 samples{0};
        uint64 requested{0};
        uint64 planned{0};
        uint64 effective{0};
        uint64 worker_fallbacks{0};
        double reorder_total_ms{0.0};
        double preprocess_total_ms{0.0};
        double worker_join_total_ms{0.0};
        double submit_cpu_total_ms{0.0};
        uint64 layer_total{0};
        uint64 job_total{0};
        uint64 work_unit_total{0};
        uint64 ordered_cb_total{0};
        uint32 max_active{0};
    };

    // Current-backend ownership packet at the translate/submit boundary. The
    // command buffers remain valid because their allocator owners travel in
    // the same move-only packet until the completion queue takes ownership.
    struct CurrentVulkanRecordedSubmitProfile {
        bool                                  enabled{false};
        std::chrono::steady_clock::time_point execute_started{};
        ParallelRecordProfileSample           sample{};
    };

    struct CurrentVulkanRecordedSubmit {
        CurrentVulkanRecordedSubmit(
            CmdSubmit&&                   _submit,
            const VulkanOperationContext& _context,
            uint64                        _timeline
        ) :
            submit(std::move(_submit)), context(_context), timeline(_timeline) {}

        CurrentVulkanRecordedSubmit(const CurrentVulkanRecordedSubmit&) = delete;
        CurrentVulkanRecordedSubmit& operator=(const CurrentVulkanRecordedSubmit&) = delete;
        CurrentVulkanRecordedSubmit(CurrentVulkanRecordedSubmit&&) noexcept = default;
        CurrentVulkanRecordedSubmit& operator=(CurrentVulkanRecordedSubmit&&) noexcept = default;

        CmdSubmit                               submit;
        VulkanOperationContext                  context;
        uint64                                  timeline{0};
        bool                                    has_commands{false};
        Array<VulkanCmdList*>                   ordered_cmd_lists;
        VulkanAllocatorBatch                    allocators;
        std::optional<VulkanDescriptorPushLease> descriptor_lease;
        Array<RHIResource*>                     deferred_releases;
        CurrentVulkanRecordedSubmitProfile      profile;
        std::optional<std::unique_lock<std::mutex>> runtime_execution_lock{};
    };

    struct CurrentVulkanSubmitResult {
        VulkanOperationResult outcome{};
        uint32                ordered_cmd_list_count{0};
    };

    void                       ExecuteNow(
        CmdSubmit&& _submit,
        uint64 _timeline,
        uint64 _serial,
        std::optional<CurrentVulkanRecordedSubmit>* _out_recorded = nullptr,
        bool _runtime_owner = false
    );
    CurrentVulkanSubmitResult SubmitRecorded(CurrentVulkanRecordedSubmit _recorded);
    std::optional<CurrentVulkanRecordedSubmit> TranslateForRuntime(CmdSubmit&& _submit);
    VulkanRuntimeSubmissionResult SubmitRecordedForRuntime(
        CurrentVulkanRecordedSubmit _recorded
    );
    void RejectForRuntime(CmdSubmit&& _submit, VkResult _result);
    VulkanRuntimeSubmissionResult PresentForRuntime(
        SwapchainRef _swapchain,
        TextureView _source,
        PresentReceiptRef _receipt
    );
    bool                       TryExecuteParallel(
        CmdSubmit&                    _submit,
        uint64                        _timeline,
        uint64                        _serial,
        const VulkanOperationContext& _context,
        const FunctionTable&           _function_table,
        const CmdReorderer&            _reorderer,
        double                         _reorder_time,
        std::chrono::steady_clock::time_point _profile_started,
        std::optional<CurrentVulkanRecordedSubmit>* _out_recorded = nullptr
    );
    VulkanRuntimeSubmissionResult PresentNow(
        SwapchainRef&& _swapchain,
        TextureRef&&   _source_texture,
        TextureView    _source_view,
        PresentReceiptRef _receipt,
        uint64         _timeline,
        uint64         _serial,
        bool           _runtime_owner = false
    );
    void                       RhiThreadMain();
    void                       CompletionThreadMain();
    Array<RHIResource*>        TakeDeferredReleases();
    void                       EnqueueCompletionMarker(uint64 _timeline);
    UniquePtr<VulkanAllocator> GetAllocator();
    UniquePtr<VulkanPresentor> GetPresentor();
    void                       Complete(uint64 _timeline);
    void                       EnsureRecordCalibration();
    void                       RecordRhiRecordProfile(const RhiRecordExecuteSample& _sample);
    void                       RecordThreadingProfile(
        ERhiWorkKind _kind,
        double       _caller_ms,
        double       _queue_wait_ms,
        double       _work_ms,
        uint32       _enqueue_depth
    );
    void                       RecordParallelRecordProfile(
        const ParallelRecordProfileSample& _sample
    );
    void                       FlushParallelRecordProfile();
    void                       BeginSplitProfilingCpuFrame();
    void                       AccumulateSplitProfilingCpuFrame(
        double _reorder_ms,
        double _preprocess_ms
    );
    void                       EndSplitProfilingCpuFrame();
    void                       ResetSplitProfilingCpuFrame();

private:
    std::atomic<uint64>                                last_frame{0};
    CircularQueue<uint64, s_queue_max_frame_in_flight> executed_queue;
    std::atomic<uint64>                                cpu_settled_frame = 0;
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

    struct SplitProfilingCpuFrame {
        bool                                  active{false};
        std::chrono::steady_clock::time_point started{};
        double                                reorder_ms{0.0};
        double                                preprocess_ms{0.0};
    } split_profiling_cpu_frame;

    bool                    rhi_thread_enabled{false};
    bool                    parallel_record_requested{false};
    bool                    parallel_record_verify{false};
    bool                    parallel_record_profile_enabled{false};
    uint32                  parallel_record_workers{0};
    uint32                  parallel_record_min_work_units_per_job{64};
    UniquePtr<ExternalCpuJoinPool> parallel_record_pool;
    uint64                  parallel_record_batch_serial{0};
    uint64                  parallel_record_worker_throw_trigger{0};
    std::atomic<uint64>     parallel_record_worker_attempts{0};
    UniquePtr<ParallelRecordProfileState> parallel_record_profile;
    UniquePtr<RhiThreadProfileState> thread_profile;
    bool                    rhi_worker_running{false};
    std::atomic<uint32_t>   rhi_thread_id{0};
    uint64                  enqueued_rhi_work{0};
    uint64                  completed_rhi_work{0};
    DEQueue<RhiWork>        rhi_work_queue;
    std::mutex              rhi_work_mutex;
    std::condition_variable rhi_work_cv;
    std::condition_variable rhi_work_done_cv;

    std::mutex   exec_mtx;
    std::jthread completion_thread;
    std::jthread rhi_thread;
    Array<UniquePtr<VulkanAllocator>> allocator_quarantine;
    Array<UniquePtr<VulkanPresentor>> presentor_quarantine;
};
class VkCopyQueue : public CopyQueue {
public:
    friend VulkanDevice;
    friend class VulkanSubmissionExecutor;
    VkCopyQueue(VulkanDevice& _device);
    ~VkCopyQueue();
    using EventType = std::variant<
        VulkanSubmissionEvent,
        UniquePtr<VulkanAllocator>,
        UniquePtr<VulkanPresentor>,
        VulkanCallbackBatch,
        VulkanCompletionMarker,
        IOSignalEvt,
        IOWaitEvt>;
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
    VulkanRuntimeSubmissionResult ExecuteForRuntime(CmdSubmit&& _submit);
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
    void RejectForRuntime(CmdSubmit&& _submit, VkResult _result);

    struct TrackedExecuteResult {
        VulkanRuntimeSubmissionResult runtime{};
        uint64                        logical_timeline{0};
    };

    TrackedExecuteResult ExecuteTracked(CmdSubmit&& _submit);
    UniquePtr<VulkanAllocator>                GetAllocator();
    void                                      Complete(uint64 _timeline);
    LockFreeQueueBase<VulkanAllocator, false> allocators;
    DEQueue<IOEvent>                          event_queue;

private:
    VulkanDevice& device;

    uint                    last_frame     = 0;
    std::atomic<uint64>     cpu_settled_frame = 0;
    VulkanFenceRef          timeline       = nullptr;
    std::mutex              event_mutex;
    std::atomic_bool        enabled{false};
    std::condition_variable queue_cv; // wake up execute thread from sleeping
    std::condition_variable settled_cv;
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
    Array<UniquePtr<VulkanAllocator>>              allocator_quarantine;
};
} // namespace Moer::Render
