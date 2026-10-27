#pragma once
#include "VulkanTimeline.h"
#include "PixelFormat.h"
#include "VulkanCommandContext.h"
#include "VulkanCommandList.h"
#include "VulkanCommandQueries.h"
#include "VulkanCommon.h"
#include "VulkanDescriptor.h"
#include "VulkanFault.h"
#include "misc/LockFree.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIThreadOwnership.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
namespace Moer::Render {

static constexpr uint s_queue_max_frame_in_flight = 3;
static constexpr uint s_query_max_storage         = 8 * 64;
class VkNativeQueue {
public:
    VkNativeQueue(EQueueType _type, VulkanDevice& _device);
    ~VkNativeQueue();

    VulkanOperationResult Submit(
        VulkanCommandList&            _cmdlist,
        const VulkanOperationContext& _context,
        VkFence                       _fence = VK_NULL_HANDLE
    );
    VulkanOperationResult Submit(
        std::span<VulkanCommandList* const> _cmdlists,
        const VulkanOperationContext&       _context,
        VkFence                             _fence = VK_NULL_HANDLE
    );
    VulkanOperationResult SubmitEmpty(
        const VulkanOperationContext& _context,
        VkFence                       _fence = VK_NULL_HANDLE
    );
    void Wait(
        VulkanTimeline*          _fence,
        uint64                _timeline,
        VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
    );
    void Wait(VkSemaphore _sem, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    void Signal(
        VulkanTimeline*          _fence,
        uint64                _timeline,
        VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
    );
    void Signal(VkSemaphore _semaphore, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    void DiscardPendingSubmitState() noexcept {
        wait_infos.clear();
        signal_infos.clear();
    }
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

// Immutable source-order program built before native
// recording. Bindless accesses remain symbolic until the Submission owner can
// evaluate them against the last natively accepted descriptor membership.
struct VulkanPresentationSourceStateDelta {
    TextureRef texture;
    bool       ready{false};
};

enum class EVulkanPresentationSourceStateEventType : uint8 {
    TextureState,
    BindlessAccess,
    BindlessTextureMembership,
};

struct VulkanPresentationSourceStateEvent {
    EVulkanPresentationSourceStateEventType type{
        EVulkanPresentationSourceStateEventType::TextureState
    };
    TextureRef       texture{};
    BindlessArrayRef bindless_array{};
    uint             bindless_slot{0};
    int32            membership_delta{0};
    bool             ready{false};
};

struct VulkanPresentationSourceStateProgram {
    Array<VulkanPresentationSourceStateEvent> events{};
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
        VulkanCommandList&      _cmd,
        std::string_view        _name,
        VkPipelineStageFlagBits _stage = kBeginTimestampStage
    );
    void EndProfilerSession(
        VulkanCommandList&      _cmd,
        std::string_view        _name,
        VkPipelineStageFlagBits _stage = kEndTimestampStage
    );

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

// Result returned to the upper submission runtime.  A queue timeline is only
// publishable as a dependency when vkQueueSubmit/SubmitEmpty actually accepted
// the work; retry/recreate/rejected paths deliberately carry no completion.
struct VulkanRuntimeSubmissionResult {
    VulkanOperationResult   outcome{};
    std::optional<WaitEvent> completion{};
    bool                     recoverable_rejection{false};

    [[nodiscard]] bool WasSubmitted() const {
        return completion.has_value();
    }

    [[nodiscard]] bool IsHardFailure() const {
        return outcome.status == EVulkanOperationStatus::Faulted ||
               (outcome.status == EVulkanOperationStatus::Rejected &&
                !recoverable_rejection);
    }

    [[nodiscard]] bool IsRecoverableRejection() const {
        return outcome.status == EVulkanOperationStatus::Rejected &&
               recoverable_rejection;
    }
};

using VulkanRuntimePreCompletionCallback =
    void (*)(void*, const VulkanRuntimeSubmissionResult&) noexcept;

struct VulkanRuntimePreCompletionHook {
    void*                                context{nullptr};
    VulkanRuntimePreCompletionCallback  callback{nullptr};
};

struct VulkanCommandContextBatch {
    Array<UniquePtr<VulkanCommandContext>> submitted;
    Array<UniquePtr<VulkanCommandContext>> abandoned;
};

// A runtime backend batch may retire through several logical Completion
// owners. Query/Fence waiters used by an earlier source callback must not
// block the only owner capable of publishing a later source. Every
// executable source therefore publishes its terminal state independently,
// then arrives here without waiting. The last Completion owner releases all
// user notifications in stable source order.
class VulkanBatchCompletionGroup final {
public:
    struct Participant {
        std::shared_ptr<const RHICommandLifetime> lifetime;
        Array<GpuCompletionToken>         gpu_completion_tokens;
        GpuCompletionPublishBatch         gpu_completion_batch{};
        Array<QueryToken>                query_tokens;
        QueryPublishBatch                query_batch{};
        Array<std::function<void()>>     callbacks;
        Array<std::function<void()>>     success_callbacks;
        Array<RHIResource*>              deferred_releases;
        VulkanDevice*                    device{nullptr};
        bool                             gpu_success{false};
        bool                             release_safe{false};
    };

    explicit VulkanBatchCompletionGroup(size_t _participant_count);

    VulkanBatchCompletionGroup(const VulkanBatchCompletionGroup&) = delete;
    VulkanBatchCompletionGroup& operator=(const VulkanBatchCompletionGroup&) = delete;

    void Arrive(size_t _participant_index, Participant&& _participant) noexcept;
    void WaitUntilSettled();

private:
    std::mutex                         mutex{};
    std::condition_variable            settled_cv{};
    Array<std::optional<Participant>> participants{};
    size_t                             remaining{0};
    bool                               released{false};
    bool                               settled{false};
};

struct VulkanBatchCompletionTicket {
    std::shared_ptr<VulkanBatchCompletionGroup> group{};
    size_t participant_index{std::numeric_limits<size_t>::max()};

    [[nodiscard]] bool Valid() const noexcept {
        return group != nullptr &&
               participant_index != std::numeric_limits<size_t>::max();
    }
};

struct VulkanBatchCompletionSettlement {
    uint64                                    retirement_serial{0};
    std::weak_ptr<VulkanBatchCompletionGroup> group{};
};

// Runtime-recorded submissions cross the Submission -> Completion boundary as
// one move-only ownership packet. Keeping the command payload, command context
// retirement, descriptor lease, callbacks, signals, and deferred releases in
// one completion packet makes that handoff atomic: no exception can leave a
// partially published completion sequence behind.
struct VulkanSubmitCompletionBatch {
    VulkanSubmitCompletionBatch(
        VulkanOperationResult         _outcome,
        VulkanOperationContext        _context,
        bool                          _gpu_submitted,
        bool                          _owns_queue_timeline,
        CmdSubmit&&                   _submit,
        VulkanCommandContextBatch&&   _command_contexts,
        VulkanDescriptorBufferLease&& _descriptor_lease,
        Array<RHIResource*>&&         _deferred_releases,
        VulkanBatchCompletionTicket   _batch_completion = {}
    ) noexcept :
        outcome(_outcome),
        context(_context),
        gpu_submitted(_gpu_submitted),
        owns_queue_timeline(_owns_queue_timeline),
        submit(std::move(_submit)),
        command_contexts(std::move(_command_contexts)),
        descriptor_lease(std::move(_descriptor_lease)),
        deferred_releases(std::move(_deferred_releases)),
        batch_completion(std::move(_batch_completion)) {}

    VulkanSubmitCompletionBatch(const VulkanSubmitCompletionBatch&) = delete;
    VulkanSubmitCompletionBatch& operator=(const VulkanSubmitCompletionBatch&) = delete;
    VulkanSubmitCompletionBatch(VulkanSubmitCompletionBatch&&) noexcept = default;
    VulkanSubmitCompletionBatch& operator=(VulkanSubmitCompletionBatch&&) noexcept = default;

    VulkanOperationResult                    outcome;
    VulkanOperationContext                   context;
    bool                                     gpu_submitted{false};
    bool                                     owns_queue_timeline{false};
    CmdSubmit                                submit;
    VulkanCommandContextBatch                command_contexts;
    VulkanDescriptorBufferLease              descriptor_lease;
    Array<RHIResource*>                      deferred_releases;
    VulkanBatchCompletionTicket              batch_completion{};
};

class VkCommandQueue : public CommandQueue {
public:
    VkCommandQueue(VulkanDevice& _device, EQueueType _type);
    ~VkCommandQueue();

    WaitEvent Execute(CmdSubmit&& _submit) override;
    void      Wait(WaitEvent _event) override;
    void      Present(
        SwapchainRef      _viewport,
        TextureView       _view,
        PresentReceiptRef _receipt = {}
    ) override;
    void        Sync() override;
    ProfileData GetProfilerEntry() override;

    void SetQueueSubmitMutex(std::mutex* _mutex) { queue.SetSubmitMutex(_mutex); }

private:
    friend class VulkanSubmissionExecutor;

    struct CurrentVulkanRecordedSubmit {
        CurrentVulkanRecordedSubmit(
            CmdSubmit&&                   _submit,
            const VulkanOperationContext& _context,
            uint64                        _timeline
        ) noexcept :
            submit(std::move(_submit)), context(_context), timeline(_timeline) {}

        CurrentVulkanRecordedSubmit(const CurrentVulkanRecordedSubmit&) = delete;
        CurrentVulkanRecordedSubmit& operator=(const CurrentVulkanRecordedSubmit&) = delete;
        CurrentVulkanRecordedSubmit(CurrentVulkanRecordedSubmit&&) noexcept = default;
        CurrentVulkanRecordedSubmit& operator=(CurrentVulkanRecordedSubmit&&) noexcept = default;

        CmdSubmit                            submit;
        VulkanOperationContext               context;
        uint64                               timeline{0};
        bool                                 has_commands{false};
        Array<VulkanCommandList*>            ordered_cmd_lists;
        Array<VkSemaphoreSubmitInfo>         native_waits;
        Array<VkSemaphoreSubmitInfo>         native_signals;
        VulkanCommandContextBatch            command_contexts;
        VulkanDescriptorBufferLease          descriptor_lease;
        Array<RHIResource*>                  deferred_releases;
        VulkanBatchCompletionTicket          batch_completion{};
        VulkanPresentationSourceStateProgram presentation_source_program;
        RHITransferableOwnershipGate::Lease  execution_lease{};
        VulkanOperationResult                retirement_outcome{
            EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
        };
        bool recoverable_rejection{false};
        bool native_submit_resolved{false};
        bool completion_committed{false};
    };

    struct CompletionEvent {
        VulkanSubmitCompletionBatch batch;
        uint64                      timeline;
        uint64                      retirement_serial;

        template<typename... Args>
            requires std::is_constructible_v<VulkanSubmitCompletionBatch, Args&&...>
        CompletionEvent(
            uint64 _timeline,
            uint64 _retirement_serial,
            Args&&... _args
        ) noexcept(std::is_nothrow_constructible_v<VulkanSubmitCompletionBatch, Args&&...>) :
            batch(std::forward<Args>(_args)...),
            timeline(_timeline),
            retirement_serial(_retirement_serial) {}
    };

    // Executor ownership and phase entry points.
    [[nodiscard]] bool ClaimRuntimeOwnership();
    void ReleaseRuntimeOwnership() noexcept;
    void CancelRuntimeDependencyWaits() noexcept;
    std::optional<CurrentVulkanRecordedSubmit> TranslateForRuntime(CmdSubmit&& _submit) noexcept;
    VulkanRuntimeSubmissionResult SubmitRecordedForRuntime(
        CurrentVulkanRecordedSubmit           _recorded,
        const VulkanRuntimePreCompletionHook* _pre_completion = nullptr
    ) noexcept;
    void RejectRecordedForRuntime(
        CurrentVulkanRecordedSubmit&& _recorded,
        VkResult                      _result,
        bool                          _recoverable
    ) noexcept;
    void RejectForRuntime(
        CmdSubmit&&                 _submit,
        VkResult                    _result,
        bool                        _recoverable,
        VulkanBatchCompletionTicket _batch_completion = {}
    ) noexcept;
    VulkanRuntimeSubmissionResult PresentForRuntime(
        SwapchainRef      _swapchain,
        TextureView       _source,
        PresentReceiptRef _receipt
    ) noexcept;

    // Recording, native submission and transfer to Completion.
    void RecordSubmissionCommands(
        CmdSubmit&&                                _submit,
        uint64                                     _timeline,
        uint64                                     _serial,
        std::optional<CurrentVulkanRecordedSubmit>& _recorded
    );
    VulkanOperationResult SubmitRecorded(
        CurrentVulkanRecordedSubmit& _recorded,
        const std::atomic_bool*      _continue_waiting = nullptr
    );
    VulkanRuntimeSubmissionResult PresentSwapchain(
        SwapchainRef&&    _swapchain,
        TextureRef&&      _source_texture,
        TextureView       _source_view,
        PresentReceiptRef _receipt,
        uint64            _timeline,
        uint64            _serial
    ) noexcept;
    void CommitRuntimeSubmitCompletion(
        CurrentVulkanRecordedSubmit& _recorded,
        const VulkanOperationResult& _outcome
    ) noexcept;
    UniquePtr<VulkanCommandContext> AcquireCommandContext();
    Array<RHIResource*> TakeDeferredReleases();

    void CompletionThreadMain();
    void CompleteAll(uint64 _timeline);

    void BeginSplitProfilingCpuFrame();
    void EndSplitProfilingCpuFrame();
    void ResetSplitProfilingCpuFrame();

    VulkanDevice&      vk_device;
    VkNativeQueue      queue;
    VulkanTimeline*    timeline{nullptr};

    // The gate transfers recording ownership with each submission packet.
    std::mutex                  exec_mtx;
    RHITransferableOwnershipGate runtime_execution_gate;
    std::atomic_bool            executor_claimed{false};
    std::atomic_bool            runtime_dependency_waits_enabled{true};
    std::atomic<uint64>          last_reserved_timeline{0};

    // CPU retirement also includes requests which never reached the GPU.
    std::mutex                            event_mutex;
    std::condition_variable_any           queue_cv;
    DEQueue<CompletionEvent>               event_queue;
    bool                                  completion_worker_running{false};
    std::jthread                          completion_thread;
    std::atomic<uint64>                    cpu_retired_timeline{0};
    uint64                                retirement_enqueued_serial{0};
    std::atomic<uint64>                    retirement_settled_serial{0};
    Array<VulkanBatchCompletionSettlement> batch_completion_settlements{};

    LockFreeQueueBase<VulkanCommandContext, false> command_contexts;
    Array<UniquePtr<VulkanCommandContext>>        command_context_quarantine;

    VkNativeQueryPool timestamp_pool;
    ProfilerStorage   profiler_storage;
    ProfileData       cached_profiler_entry;
    std::mutex        profiler_mutex;
    struct SplitProfilingCpuFrame {
        bool                                  active{false};
        std::chrono::steady_clock::time_point started{};
    } split_profiling_cpu_frame;
};

class VkCopyQueue : public CopyQueue {
public:
    VkCopyQueue(VulkanDevice& _device);
    ~VkCopyQueue();

    WaitEvent Execute(CmdSubmit&& _submit) override;
    FenceRef  GetFenceHandle() override;
    void      Sync(uint64 _timeline) override;

    void SetQueueSubmitMutex(std::mutex* _mutex) { queue.SetSubmitMutex(_mutex); }

    void CopyFrom(BufferView _src, BufferView _dst) override {};
    void CopyFrom(TextureView _src, TextureView _dst) override {};
    void CopyFrom(TextureView _src, BufferView _dst) override {};
    void CopyFrom(BufferView _src, TextureView _dst) override {};
    void CopyFrom(std::span<byte> _data, BufferView _dst) override {};
    void CopyFrom(std::span<byte> _data, TextureView _dst) override {};

private:
    friend class VulkanDevice;
    friend class VulkanSubmissionExecutor;

    struct CurrentVulkanCopyRecordedSubmit {
        explicit CurrentVulkanCopyRecordedSubmit(CmdSubmit&& _submit) noexcept :
            submit(std::move(_submit)) {}

        CurrentVulkanCopyRecordedSubmit(const CurrentVulkanCopyRecordedSubmit&) = delete;
        CurrentVulkanCopyRecordedSubmit& operator=(const CurrentVulkanCopyRecordedSubmit&) = delete;
        CurrentVulkanCopyRecordedSubmit(CurrentVulkanCopyRecordedSubmit&&) noexcept = default;
        CurrentVulkanCopyRecordedSubmit& operator=(CurrentVulkanCopyRecordedSubmit&&) noexcept = default;

        CmdSubmit                            submit;
        VulkanOperationContext               context{};
        uint64                               logical_timeline{0};
        VulkanCommandContextBatch            command_contexts{};
        VulkanBatchCompletionTicket          batch_completion{};
        VulkanPresentationSourceStateProgram presentation_source_program;
        RHITransferableOwnershipGate::Lease  execution_lease{};
        VulkanOperationResult                retirement_outcome{
            EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
        };
        bool recoverable_rejection{false};
        bool native_submit_resolved{false};
        bool completion_committed{false};
    };

    struct CompletionEvent {
        VulkanSubmitCompletionBatch batch;
        uint64                      timeline;
        uint64                      retirement_serial;

        template<typename... Args>
            requires std::is_constructible_v<VulkanSubmitCompletionBatch, Args&&...>
        CompletionEvent(
            uint64 _timeline,
            uint64 _retirement_serial,
            Args&&... _args
        ) noexcept(std::is_nothrow_constructible_v<VulkanSubmitCompletionBatch, Args&&...>) :
            batch(std::forward<Args>(_args)...),
            timeline(_timeline),
            retirement_serial(_retirement_serial) {}
    };

    [[nodiscard]] bool ClaimRuntimeOwnership();
    void ReleaseRuntimeOwnership() noexcept;
    void EnableRuntimeDependencyWaits() noexcept;
    void CancelRuntimeDependencyWaits() noexcept;
    std::optional<CurrentVulkanCopyRecordedSubmit> TranslateForRuntime(CmdSubmit&& _submit) noexcept;
    VulkanRuntimeSubmissionResult SubmitRecordedForRuntime(
        CurrentVulkanCopyRecordedSubmit       _recorded,
        const VulkanRuntimePreCompletionHook* _pre_completion = nullptr
    ) noexcept;
    void RejectRecordedForRuntime(
        CurrentVulkanCopyRecordedSubmit&& _recorded,
        VkResult                         _result,
        bool                             _recoverable
    ) noexcept;
    void RejectForRuntime(
        CmdSubmit&&                 _submit,
        VkResult                    _result,
        bool                        _recoverable,
        VulkanBatchCompletionTicket _batch_completion = {}
    ) noexcept;

    void CommitRuntimeSubmitCompletion(
        CurrentVulkanCopyRecordedSubmit& _recorded,
        const VulkanOperationResult&     _outcome
    ) noexcept;
    UniquePtr<VulkanCommandContext> AcquireCommandContext();
    void ExecuteThread();
    void CompleteAll(uint64 _timeline);

    VulkanDevice&     device;
    VkNativeQueue     queue;
    VulkanTimelineRef timeline{nullptr};

    std::mutex                  exec_mutex;
    RHITransferableOwnershipGate runtime_execution_gate;
    std::atomic_bool            executor_claimed{false};
    std::atomic_bool            dependency_waits_enabled{true};
    std::atomic<uint64>          last_frame{0};

    std::mutex                            event_mutex;
    std::condition_variable_any           queue_cv;
    std::condition_variable               settled_cv;
    DEQueue<CompletionEvent>               event_queue;
    std::atomic_bool                      enabled{false};
    std::jthread                          thread;
    std::atomic<uint64>                    cpu_settled_frame{0};
    uint64                                retirement_enqueued_serial{0};
    std::atomic<uint64>                    retirement_settled_serial{0};
    Array<VulkanBatchCompletionSettlement> batch_completion_settlements{};

    LockFreeQueueBase<VulkanCommandContext, false> command_contexts;
    Array<UniquePtr<VulkanCommandContext>>        command_context_quarantine;
};

} // namespace Moer::Render
