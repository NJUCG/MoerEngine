#pragma once

#include "rhi/RHIExecutorBackend.h"
#include "rhi/RHISubmissionPipelinePolicy.h"
#include "VulkanQueue.h"
#include "VulkanSubmissionRequestDispatch.h"

#include <atomic>
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
#include <variant>

namespace Moer::Render {

// Owns the upper RHI submission stream for Vulkan. The Translate coordinator
// may dispatch one explicit-RDG source per native queue lane to TaskGraph
// workers, then transfers every move-only packet in stable source order to the
// sole native Submission owner. Native queue submission remains single-owner
// and serial. Validated non-zero async scopes may overlap CPU translation and
// GPU execution across distinct native queues.
class VulkanSubmissionExecutor final : public RHIBackendExecutor {
public:
    explicit VulkanSubmissionExecutor(uint32 _batch_window = 2);
    ~VulkanSubmissionExecutor() override;

    void          Enqueue(RHIBackendSubmissionBatch&& _batch) override;
    GraphEventRef Sync(ERHISyncDepth _depth = ERHISyncDepth::RHI) override;
    void          Flush(ERHIFlushDepth _depth = ERHIFlushDepth::SubmitGPU) override;
    void          BeginShutdown() noexcept override;
    void          ShutDown() override;

private:
    struct Completion {
        void Signal();
        void Wait();

        std::mutex              mutex{};
        std::condition_variable cv{};
        bool                    done{false};
    };

    using ERequestKind = VulkanSubmissionDetail::EWorkerRequestKind;

    struct Request {
        ERequestKind                kind{ERequestKind::Submit};
        RHIBackendSubmissionBatch   batch{};
        ERHISyncDepth               sync_depth{ERHISyncDepth::RHI};
        std::shared_ptr<Completion> completion{};
    };

    struct BatchExceptionState {
        // Executable entries below this cursor have already been terminalized
        // by their native queue. A multi-segment upper source may occupy
        // several entries; its source-level callback lifetime is protected by
        // a completion aggregate spanning every segment.
        size_t first_unconsumed_source{0};
    };

    struct SubmissionWork {
        std::packaged_task<void()> execute{};
    };

    struct PipelineFailure {
        size_t reject_from{std::numeric_limits<size_t>::max()};
        int32  result{0};
        bool   recoverable{false};
    };

    struct RejectionSignalHandle {
        FenceRef fence{};
        uint64   value{0};
    };

    struct RejectionSourceSnapshot {
        Array<RejectionSignalHandle> signals{};
        Array<QueryToken>            query_tokens{};
        QueryPublishBatch            query_batch{};
        bool                         terminalized{false};
    };

    // Immutable, strongly-owned terminal handles captured before any source
    // leaves the request-owned batch. Pipeline workers can therefore publish a
    // failed suffix immediately even when its later raw CmdSubmits have not yet
    // entered Translate.
    struct BatchRejectionPublication {
        explicit BatchRejectionPublication(
            RHIBackendSubmissionBatch& _batch
        );

        void PublishSuffix(
            size_t           _reject_from,
            int32            _result,
            bool             _recoverable,
            std::string_view _reason
        ) noexcept;

        Array<RejectionSourceSnapshot> sources{};
        std::mutex                     mutex{};
    };

    struct RuntimePreCompletionPublication {
        std::shared_ptr<BatchRejectionPublication> rejection_publication{};
        size_t                                     reject_from{0};
    };

    using RecordedSourcePacket = std::variant<
        std::monostate,
        VkCommandQueue::CurrentVulkanRecordedSubmit,
        VkCopyQueue::CurrentVulkanCopyRecordedSubmit>;

    struct PipelineSourceSlot {
        EQueueType queue{EQueueType::Ignore};
        uint64     async_queue_scope{0};
        uint32     original_source_index{0};
        uint32     source_segment_index{0};
        uint32     source_segment_count{1};
        bool       cross_native_predecessor_wait{false};
        RecordedSourcePacket recorded_packet{};
    };

    struct PipelineBatchState {
        explicit PipelineBatchState(
            uint64                      _sequence,
            size_t                      _source_count,
            std::shared_ptr<Completion> _completion,
            std::shared_ptr<BatchRejectionPublication>
                _rejection_publication
        );

        void AddWork() noexcept;
        void FinishWork() noexcept;
        void Seal() noexcept;
        void PublishFailure(
            size_t _reject_from,
            int32  _result,
            bool   _recoverable
        ) noexcept;
        [[nodiscard]] PipelineFailure ReadFailure() const noexcept;

        uint64                       sequence{0};
        Array<PipelineSourceSlot>    slots{};
        std::shared_ptr<Completion>  completion{};
        RHISubmissionPipelinePolicy::PipelineBatchWorkState work_state{};
        std::atomic_bool             completion_signalled{false};
        mutable std::mutex           failure_mutex{};
        PipelineFailure              failure{};
        std::shared_ptr<BatchRejectionPublication>
            rejection_publication{};
    };

    struct StreamPosition {
        uint64 batch_sequence{0};
        size_t source_index{0};
    };

    void RunExecutor();
    void RunSubmission();
    bool EnqueueSubmissionWork(SubmissionWork&& _work);
    void StopSubmissionThread();

    template<typename Function>
    auto ExecuteOnSubmissionThread(Function&& _function)
        -> std::invoke_result_t<Function> {
        using Result = std::invoke_result_t<Function>;

        std::packaged_task<Result()> typed_task(std::forward<Function>(_function));
        std::future<Result>          result = typed_task.get_future();
        SubmissionWork work{
            .execute = std::packaged_task<void()>(
                [task = std::move(typed_task)]() mutable { task(); }
            ),
        };
        if (!EnqueueSubmissionWork(std::move(work))) {
            throw std::runtime_error("Vulkan Submission owner is not accepting work");
        }
        if constexpr (std::is_void_v<Result>) {
            result.get();
        } else {
            return result.get();
        }
    }

    void ProcessBatch(
        RHIBackendSubmissionBatch& _batch,
        BatchExceptionState&       _exception_state
    );
    void ExecutePipelineSource(
        const std::shared_ptr<PipelineBatchState>& _batch,
        size_t                                     _source_index
    ) noexcept;
    [[nodiscard]] RecordedSourcePacket TranslateSourceForRuntime(
        EQueueType _queue,
        CmdSubmit&& _submit
    ) noexcept;
    [[nodiscard]] VulkanRuntimeSubmissionResult SubmitRecordedSourceForRuntime(
        EQueueType                            _queue,
        RecordedSourcePacket&&                _packet,
        const VulkanRuntimePreCompletionHook* _pre_completion = nullptr
    ) noexcept;
    static void PublishRuntimeFailureBeforeCompletion(
        void*                                _context,
        const VulkanRuntimeSubmissionResult& _result
    ) noexcept;
    void RejectRecordedSourceForRuntime(
        EQueueType             _queue,
        RecordedSourcePacket&& _packet,
        VkResult               _result,
        bool                   _recoverable
    ) noexcept;
    void RejectSourceForRuntime(
        EQueueType _queue,
        CmdSubmit&& _submit,
        VkResult    _result,
        bool        _recoverable
    ) noexcept;
    [[nodiscard]] static bool HasRecordedSourcePacket(
        const RecordedSourcePacket& _packet
    ) noexcept;
    [[nodiscard]] static CmdSubmit* GetRecordedSourceSubmit(
        RecordedSourcePacket& _packet
    ) noexcept;
    [[nodiscard]] VkResult GetQueueFaultResult(
        EQueueType _queue
    ) const noexcept;
    void WaitForPipelineCapacity();
    void DrainPipelineBatches();
    void PrepareStreamSubmit(CmdSubmit& _submit, EQueueType _queue);
    void UpdateStreamFrontier(
        EQueueType _queue,
        WaitEvent  _completion,
        bool       _collapse
    );
    void ResetStreamScope() noexcept;
    void LatchHardFailure(
        StreamPosition _position,
        int32          _result
    ) noexcept;
    [[nodiscard]] bool IsHardFailureAtOrBefore(
        StreamPosition _position,
        int32*         _result = nullptr
    ) const noexcept;
    void ProcessSync(ERHISyncDepth _depth);
    void RejectBatch(
        RHIBackendSubmissionBatch&& _batch,
        int32                       _result,
        std::string_view            _reason,
        bool                        _recoverable,
        size_t                      _first_source = 0,
        size_t*                     _next_unconsumed_source = nullptr
    );
    void CompleteRequest(const std::shared_ptr<Completion>& _completion);
    void ReportRequestFailure(
        ERequestKind                                  _kind,
        VulkanSubmissionDetail::EWorkerRequestFailurePhase _phase,
        const std::exception_ptr&                     _exception
    ) noexcept;

    std::mutex                 mutex{};
    std::condition_variable    cv{};
    std::deque<Request>        requests{};
    std::jthread               executor_thread{};
    bool                       accepting{true};
    bool                       constructor_abort{false};
    bool                       stopped{false};
    bool                       claims_owned{false};
    bool                       logged_multi_source_topology{false};
    bool                       logged_multi_segment_topology{false};
    bool                       logged_async_queue_scope{false};
    bool                       logged_parallel_translate_wave{false};
    bool                       logged_cross_batch_pipeline{false};
    std::shared_ptr<Completion> stop_completion{};
    size_t                     batch_window{2};
    bool                       runtime_queues_share_native_lane{false};
    std::deque<std::shared_ptr<Completion>> in_flight_batches{};

    mutable std::mutex         hard_failure_mutex{};
    std::optional<StreamPosition> hard_failure_position{};
    int32                      hard_failure_result{0};

    StaticArray<bool, static_cast<size_t>(EQueueType::Num)> used_queues{};
    uint64                     last_copy_timeline{0};
    StaticArray<
        std::optional<WaitEvent>,
        static_cast<size_t>(EQueueType::Num)> gpu_frontier{};
    uint64 active_async_queue_scope{0};
    StaticArray<
        std::optional<WaitEvent>,
        static_cast<size_t>(EQueueType::Num)> async_scope_entry_frontier{};
    StaticArray<bool, static_cast<size_t>(EQueueType::Num)>
        async_scope_seen_queues{};

    std::mutex                 submission_mutex{};
    std::condition_variable    submission_cv{};
    std::deque<SubmissionWork> submission_work{};
    std::jthread               submission_thread{};
    bool                       submission_accepting{true};
};

} // namespace Moer::Render
