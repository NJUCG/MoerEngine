#pragma once

#include "VulkanSubmission.h"
#include "VulkanSubmissionRequestDispatch.h"
#include "rhi/RHISubmissionPipelinePolicy.h"
#include "rhi/executor/RHIExecutorBackend.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace Moer::Render {
namespace VulkanSubmissionDetail {
struct VulkanSegmentMaterializationResult;
}

// Schedules Vulkan recording and ordered native submission.
// Translate may record independent lanes in parallel; Submission owns queue operations.
// Native queues handle GPU completion and resource retirement.
class VulkanSubmissionExecutor final : public RHIBackendExecutor {
public:
    // Executor lifetime and native queue ownership.
    explicit VulkanSubmissionExecutor(uint32 _batch_window = 2);
    ~VulkanSubmissionExecutor() override;

    // Publish ordered work and request explicit host synchronization.
    void Enqueue(RHISubmissionBatch&& _batch) override;
    void Sync(ERHISyncDepth _depth = ERHISyncDepth::RHI) override;
    void Flush() override;

    // Cancel dependency waits and stop the worker threads.
    void BeginShutdown() noexcept override;
    void ShutDown() override;

private:
    // Types used by input dispatch, GPU dependencies and failure ordering.
    using QueueFrontier      = StaticArray<std::optional<WaitEvent>, static_cast<size_t>(EQueueType::Num)>;
    using ERequestKind       = VulkanSubmissionDetail::EWorkerRequestKind;
    using SubmissionWork     = std::packaged_task<void()>;
    using SubmissionPosition = std::pair<uint64, size_t>; // Batch sequence, executable source index.

    // One input operation. Sync/Stop carry a CPU reply, independent of GPU completion.
    struct WorkerRequest {
        ERequestKind                      kind{ERequestKind::Submit};
        RHISubmissionBatch                batch{};
        ERHISyncDepth                     sync_depth{ERHISyncDepth::RHI};
        std::optional<std::promise<void>> reply{};
    };

    // One executable source throughout Translate and Submission. After handoff,
    // only Submission touches recorded; failure observers remain strongly owned.
    struct SubmissionSource {
        // Executable identity, destination queue and async dependency scope.
        size_t        source_index{0};
        SubmissionKey key{};
        EQueueType    queue_type{EQueueType::Ignore};
        uint64        async_queue_scope{0};

#if MOER_VULKAN_TEST_SUPPORT
        // Test-only provenance from the original source and its segments.
        uint32 original_source_index{0};
        uint32 source_segment_index{0};
        uint32 source_segment_count{1};
        bool   cross_native_predecessor_wait{false};
#endif

        // Recording progress and packet ownership during the thread handoff.
        bool                                   attempted{false};  // Translation started.
        bool                                   handed_off{false}; // Translate no longer owns the source.
        std::optional<VulkanPendingSubmission> recorded{};

        // Notification handles survive packet moves; publication_mutex protects their state.
        Array<std::pair<FenceRef, uint64>> signals{};
        Array<GpuCompletionToken>          gpu_completion_tokens{};
        GpuCompletionPublishBatch          gpu_completion_batch{};
        Array<QueryToken>                  query_tokens{};
        QueryPublishBatch                  query_batch{};
        uint64                             publication_epoch{0}; // Zero until failure publication claims this source.
    };

    // Owns source records, early failure publication and bounded CPU dispatch.
    // GPU completion and resource retirement stay with each native queue.
    struct BatchExecution {
        explicit BatchExecution(RHISubmissionBatch& _batch, bool _pipeline);

        // Track CPU Submission tasks; sealing prevents further additions.
        void AddWork() noexcept;
        void FinishWork() noexcept;
        void Seal() noexcept;

        // Reject a source suffix and publish its terminal observations.
        void                                                PublishSuffixRejection(size_t _first, int32 _result, bool _recoverable, std::string_view _reason) noexcept;
        void                                                PublishFailure(size_t _reject_from, int32 _result, bool _recoverable) noexcept;
        [[nodiscard]] std::optional<VulkanSubmissionResult> ReadFailure(size_t _source_index) const noexcept;

        // Batch identity and source records retained across thread handoff.
        uint64                  sequence{0};
        Array<SubmissionSource> sources{};

        // CPU Submission work has drained; GPU completion remains queue-owned.
        std::optional<std::promise<void>>                   submission_tasks_done{};
        RHISubmissionPipelinePolicy::PipelineBatchWorkState work_state{};

        // Suffix failure state and reentrant notification publication.
        std::recursive_mutex                                publication_mutex{};
        mutable std::mutex                                  failure_mutex{}; // Released before publishing external notifications.
        size_t                                              reject_from{std::numeric_limits<size_t>::max()};
        VulkanSubmissionResult                              failure{};
        uint64                                              next_publication_epoch{1};
    };

    // Worker loops and dispatch to the single Submission owner.
    void RunTranslateThread();
    void RunSubmissionThread();
    bool EnqueueSubmissionWork(SubmissionWork&& _work);
    void StopSubmissionThread();

    template<typename Function>
    auto RunOnSubmissionThreadAndWait(Function&& _function) -> std::invoke_result_t<Function> {
        using Result = std::invoke_result_t<Function>;

        std::packaged_task<Result()> typed_task(std::forward<Function>(_function));
        std::future<Result>          result = typed_task.get_future();
        SubmissionWork               work([task = std::move(typed_task)]() mutable {
            task();
        });
        if (!EnqueueSubmissionWork(std::move(work))) {
            throw std::runtime_error("Vulkan Submission owner is not accepting work");
        }
        return result.get();
    }

    // Batch admission and ordered source scheduling on Translate.
    void ProcessBatch(RHISubmissionBatch& _batch, size_t& _first_unconsumed_source, std::shared_ptr<BatchExecution>& _execution_state);
    bool PrepareBatch(
        RHISubmissionBatch&                                         _batch,
        size_t&                                                     _first_unconsumed_source,
        VulkanSubmissionDetail::VulkanSegmentMaterializationResult& _materialization
    );
    bool CanPipelineBatch(const RHISubmissionBatch& _batch) const;
    bool PrepareSource(RHISubmissionBatch& _batch, size_t _source_index, bool _pipeline);
    void RejectUnhandedSources(
        RHISubmissionBatch& _batch,
        const std::shared_ptr<BatchExecution>& execution,
        std::span<SubmissionSource> sources,
        VkResult result
    ) noexcept;
    bool ProcessParallelSources(
        RHISubmissionBatch&                    _batch,
        const std::shared_ptr<BatchExecution>& _execution,
        size_t                                 _begin,
        size_t                                 _end,
        size_t&                                _first_unconsumed_source
    );
    bool ProcessSerialSource(
        RHISubmissionBatch&                    _batch,
        const std::shared_ptr<BatchExecution>& _execution,
        size_t                                 _source_index,
        size_t&                                _first_unconsumed_source
    );

    // Record RHI commands into owning Vulkan submission packets.
    void RecordSource(uint64 _sequence, SubmissionSource& _source, CmdSubmit&& _submit) noexcept;
    bool TranslateWave(RHISubmissionBatch& _batch, std::span<SubmissionSource*> _sources);
    [[nodiscard]] std::optional<VulkanPendingSubmission> TranslateSourceForRuntime(
        EQueueType  _queue,
        CmdSubmit&& _submit
    ) noexcept;

    // Hand recorded packets to Submission and commit native submission decisions.
    bool EnqueuePipelineSource(const std::shared_ptr<BatchExecution>& _execution, size_t _source_index);
    void ExecutePipelineSource(const std::shared_ptr<BatchExecution>& _batch, size_t _source_index) noexcept;
    VulkanSubmissionResult SubmitSourceAndWait(
        const std::shared_ptr<BatchExecution>& _execution,
        size_t                                 _source_index,
        VulkanPendingSubmission&               _packet
    );
    [[nodiscard]] VulkanSubmissionResult SubmitRecordedSourceForRuntime(
        VulkanPendingSubmission _packet,
        BatchExecution*         _execution   = nullptr,
        size_t                  _reject_from = 0
    ) noexcept;

    // Bound outstanding CPU submission batches and drain their tasks.
    void WaitForPipelineCapacity();
    void DrainPipelineBatches();
    void DrainBatchSubmissionTasks(const std::shared_ptr<BatchExecution>& _execution);

    // GPU dependency tails and entry dependencies for async queue scopes.
    void PrepareStreamSubmit(CmdSubmit& _submit, EQueueType _queue);
    void AdvanceSourceFrontier(const SubmissionSource& _source, WaitEvent _completion);
    void UpdateStreamFrontier(EQueueType _queue, WaitEvent _completion, bool _collapse);
    void AppendFrontierWaits(CmdSubmit& _submit, EQueueType _queue, const QueueFrontier& _frontier) const;
    bool HasForeignAliasedFrontier(EQueueType _queue) const;
    void ResetStreamScope() noexcept;

    // Present and explicit synchronization boundaries.
    void ProcessPresent(
        RHISubmissionBatch&                    _batch,
        const std::shared_ptr<BatchExecution>& _execution,
        size_t&                                _first_unconsumed_source
    );
    void ProcessSync(ERHISyncDepth _depth);

    // Reject unconsumed work and retain the earliest hard-failure boundary.
    void RejectRemainingSources(
        RHISubmissionBatch&                    _batch,
        const std::shared_ptr<BatchExecution>& _execution,
        size_t                                 _begin,
        VkResult                               _result,
        std::string_view                       _reason,
        bool                                   _recoverable,
        size_t&                                _first_unconsumed_source
    );
    void RejectBatch(
        RHISubmissionBatch&& _batch,
        int32                _result,
        std::string_view     _reason,
        bool                 _recoverable,
        size_t               _first_source           = 0,
        size_t*              _next_unconsumed_source = nullptr
    );
    void RejectRecordedSourceForRuntime(VulkanPendingSubmission&& _packet, VkResult _result, bool _recoverable) noexcept;
    void RejectSourceForRuntime(
        EQueueType  _queue,
        CmdSubmit&& _submit,
        VkResult    _result,
        bool        _recoverable
    ) noexcept;
    [[nodiscard]] VkResult GetQueueFaultResult(EQueueType _queue) const noexcept;
    void                   LatchHardFailure(SubmissionPosition _position, int32 _result) noexcept;
    [[nodiscard]] bool     IsHardFailureAtOrBefore(SubmissionPosition _position, int32* _result = nullptr) const noexcept;

    // Reply to control requests and report worker exceptions.
    void CompleteRequest(std::optional<std::promise<void>>& _completion, std::exception_ptr _failure = {});
    void ReportRequestFailure(
        ERequestKind                                       _kind,
        VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
        const std::exception_ptr&                          _exception
    ) noexcept;

    // Input FIFO, Translate worker lifetime and native queue ownership.
    std::mutex                    mutex{};
    std::condition_variable_any   cv{};
    std::deque<WorkerRequest>     requests{};
    std::jthread                  translate_thread{};
    bool                          accepting{true};
    bool                          constructor_abort{false};
    bool                          stopped{false};
    bool                          claims_owned{false};

    // One-time topology and scheduling diagnostics.
    bool                          logged_multi_source_topology{false};
    bool                          logged_multi_segment_topology{false};
    bool                          logged_async_queue_scope{false};
    bool                          logged_parallel_translate_wave{false};
    bool                          logged_cross_batch_pipeline{false};

    // Shared Stop reply for concurrent shutdown callers.
    std::shared_future<void>      stop_completion{};

    // Bounded CPU pipeline window and pending batch task completions.
    size_t                        batch_window{2};
    bool                          runtime_queues_share_native_lane{false};
    std::deque<std::future<void>> pending_submission_batches{};

    // Global hard-failure position shared by Translate and Submission.
    mutable std::mutex                hard_failure_mutex{};
    std::optional<SubmissionPosition> hard_failure_position{};
    int32                             hard_failure_result{0};

    // Accepted GPU tails used by ordinary submissions, Present and Sync.
    uint64                                                  last_copy_timeline{0};
    QueueFrontier                                           gpu_frontier{};

    // Async-scope entry snapshot and the native lanes already attached to it.
    uint64                                                  active_async_queue_scope{0};
    QueueFrontier                                           async_scope_entry_frontier{};
    StaticArray<bool, static_cast<size_t>(EQueueType::Num)> async_scope_seen_queues{};

    // Internal FIFO and lifetime of the single Submission owner.
    std::mutex                  submission_mutex{};
    std::condition_variable_any submission_cv{};
    std::deque<SubmissionWork>  submission_work{};
    std::jthread                submission_thread{};
    bool                        submission_accepting{true};
};

} // namespace Moer::Render
