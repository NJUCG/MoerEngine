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
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace Moer::Render {

// Schedules Vulkan recording and ordered native submission.
// Translate may record independent lanes in parallel; Submission owns queue operations.
// Native queues handle GPU completion and resource retirement.
class VulkanSubmissionExecutor final : public RHIBackendExecutor {
public:
    explicit VulkanSubmissionExecutor(uint32 _batch_window = 2);
    ~VulkanSubmissionExecutor() override;

    void Enqueue(RHISubmissionBatch&& _batch) override;
    void Sync(ERHISyncDepth _depth = ERHISyncDepth::RHI) override;
    void Flush() override;
    void BeginShutdown() noexcept override;
    void ShutDown() override;

private:
    using ERequestKind       = VulkanSubmissionDetail::EWorkerRequestKind;
    using SubmissionWork     = std::packaged_task<void()>;
    using SubmissionPosition = std::pair<uint64, size_t>; // Batch sequence, executable source index.

    // One input operation. Sync/Stop carry a CPU reply, independent of GPU completion.
    struct WorkerRequest {
        ERequestKind                      kind{ERequestKind::Submit};
        RHISubmissionBatch                batch{};
        ERHISyncDepth                     sync_depth{ERHISyncDepth::RHI};
        std::optional<std::promise<void>> completion{};
    };

    // One executable source throughout Translate and Submission. After handoff,
    // only Submission touches recorded; failure observers remain strongly owned.
    struct SubmissionSource {
        size_t                                 source_index{0};
        SubmissionKey                          key{};
        EQueueType                             queue_type{EQueueType::Ignore};
        uint64                                 async_queue_scope{0};
        uint32                                 original_source_index{0};
        uint32                                 source_segment_index{0};
        uint32                                 source_segment_count{1};
        bool                                   cross_native_predecessor_wait{false};
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

        void                                                AddWork() noexcept;
        void                                                FinishWork() noexcept;
        void                                                Seal() noexcept;
        void                                                RejectFrom(size_t _first, int32 _result, bool _recoverable, std::string_view _reason) noexcept;
        void                                                PublishFailure(size_t _reject_from, int32 _result, bool _recoverable) noexcept;
        [[nodiscard]] std::optional<VulkanSubmissionResult> ReadFailure(size_t _source_index) const noexcept;

        uint64                                              sequence{0};
        Array<SubmissionSource>                             sources{};
        std::optional<std::promise<void>>                   completion{};
        RHISubmissionPipelinePolicy::PipelineBatchWorkState work_state{};
        std::recursive_mutex                                publication_mutex{};
        mutable std::mutex                                  failure_mutex{}; // Released before publishing external notifications.
        size_t                                              reject_from{std::numeric_limits<size_t>::max()};
        VulkanSubmissionResult                              failure{};
        uint64                                              next_publication_epoch{1};
    };

    void RunTranslateThread();
    void RunSubmissionThread();
    bool EnqueueSubmissionWork(SubmissionWork&& _work);
    void StopSubmissionThread();

    template<typename Function>
    auto ExecuteOnSubmissionThread(Function&& _function) -> std::invoke_result_t<Function> {
        using Result = std::invoke_result_t<Function>;

        std::packaged_task<Result()> typed_task(std::forward<Function>(_function));
        std::future<Result>          result = typed_task.get_future();
        SubmissionWork               work([task = std::move(typed_task)]() mutable {
            task();
        });
        if (!EnqueueSubmissionWork(std::move(work))) {
            throw std::runtime_error("Vulkan Submission owner is not accepting work");
        }
        if constexpr (std::is_void_v<Result>) {
            result.get();
        } else {
            return result.get();
        }
    }

    void                                                 ProcessBatch(RHISubmissionBatch& _batch, size_t& _first_unconsumed_source, std::shared_ptr<BatchExecution>& _execution_state);
    void                                                 ExecutePipelineSource(const std::shared_ptr<BatchExecution>& _batch, size_t _source_index) noexcept;
    [[nodiscard]] std::optional<VulkanPendingSubmission> TranslateSourceForRuntime(
        EQueueType  _queue,
        CmdSubmit&& _submit
    ) noexcept;
    [[nodiscard]] VulkanSubmissionResult SubmitRecordedSourceForRuntime(
        VulkanPendingSubmission _packet,
        BatchExecution*         _execution   = nullptr,
        size_t                  _reject_from = 0
    ) noexcept;
    void RejectRecordedSourceForRuntime(
        EQueueType                               _queue,
        std::optional<VulkanPendingSubmission>&& _packet,
        VkResult                                 _result,
        bool                                     _recoverable
    ) noexcept;
    void RejectSourceForRuntime(
        EQueueType  _queue,
        CmdSubmit&& _submit,
        VkResult    _result,
        bool        _recoverable
    ) noexcept;
    [[nodiscard]] VkResult GetQueueFaultResult(EQueueType _queue) const noexcept;
    void                   WaitForPipelineCapacity();
    void                   DrainPipelineBatches();
    void                   PrepareStreamSubmit(CmdSubmit& _submit, EQueueType _queue);
    void                   UpdateStreamFrontier(EQueueType _queue, WaitEvent _completion, bool _collapse);
    void                   ResetStreamScope() noexcept;
    void                   LatchHardFailure(SubmissionPosition _position, int32 _result) noexcept;
    [[nodiscard]] bool     IsHardFailureAtOrBefore(SubmissionPosition _position, int32* _result = nullptr) const noexcept;
    void                   ProcessSync(ERHISyncDepth _depth);
    void                   RejectBatch(
                          RHISubmissionBatch&&            _batch,
                          int32                           _result,
                          std::string_view                _reason,
                          bool                            _recoverable,
                          size_t                          _first_source           = 0,
                          size_t*                         _next_unconsumed_source = nullptr,
                          std::shared_ptr<BatchExecution> _execution              = {}
                      );
    void CompleteRequest(std::optional<std::promise<void>>& _completion, std::exception_ptr _failure = {});
    void ReportRequestFailure(
        ERequestKind                                       _kind,
        VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
        const std::exception_ptr&                          _exception
    ) noexcept;

    std::mutex                           mutex{};
    std::condition_variable_any          cv{};
    std::deque<WorkerRequest>            requests{};
    std::jthread                         translate_thread{};
    bool                                 accepting{true};
    bool                                 constructor_abort{false};
    bool                                 stopped{false};
    bool                                 claims_owned{false};
    bool                                 logged_multi_source_topology{false};
    bool                                 logged_multi_segment_topology{false};
    bool                                 logged_async_queue_scope{false};
    bool                                 logged_parallel_translate_wave{false};
    bool                                 logged_cross_batch_pipeline{false};
    std::shared_future<void>             stop_completion{};
    size_t                               batch_window{2};
    bool                                 runtime_queues_share_native_lane{false};
    std::deque<std::shared_future<void>> in_flight_batches{};

    mutable std::mutex                hard_failure_mutex{};
    std::optional<SubmissionPosition> hard_failure_position{};
    int32                             hard_failure_result{0};

    StaticArray<bool, static_cast<size_t>(EQueueType::Num)>                     used_queues{};
    uint64                                                                      last_copy_timeline{0};
    StaticArray<std::optional<WaitEvent>, static_cast<size_t>(EQueueType::Num)> gpu_frontier{};
    uint64                                                                      active_async_queue_scope{0};
    StaticArray<std::optional<WaitEvent>, static_cast<size_t>(EQueueType::Num)> async_scope_entry_frontier{};
    StaticArray<bool, static_cast<size_t>(EQueueType::Num)>                     async_scope_seen_queues{};

    std::mutex                  submission_mutex{};
    std::condition_variable_any submission_cv{};
    std::deque<SubmissionWork>  submission_work{};
    std::jthread                submission_thread{};
    bool                        submission_accepting{true};
};

} // namespace Moer::Render
