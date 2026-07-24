#pragma once

#include "rhi/RHIExecutorBackend.h"
#include "VulkanSubmissionRequestDispatch.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace Moer::Render {

// Owns the upper RHI submission stream for Vulkan. The Translate coordinator
// may dispatch one explicit-RDG source per native queue lane to TaskGraph
// workers, then transfers every move-only packet in stable source order to the
// sole native Submission owner. Native queue submission remains single-owner
// and serial. Validated non-zero async scopes may overlap CPU translation and
// GPU execution across distinct native queues.
class VulkanSubmissionExecutor final : public RHIBackendExecutor {
public:
    VulkanSubmissionExecutor();
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
        // Sources below this cursor have already been terminalized by their
        // native queue. The current source and every later source remain the
        // rejection responsibility of the request on an unexpected exception.
        size_t first_unconsumed_source{0};
    };

    struct SubmissionWork {
        std::packaged_task<void()> execute{};
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
    void ProcessSync(ERHISyncDepth _depth);
    void RejectBatch(
        RHIBackendSubmissionBatch&& _batch,
        int32                       _result,
        std::string_view            _reason,
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
    bool                       hard_failed{false};
    bool                       logged_multi_source_topology{false};
    bool                       logged_async_queue_scope{false};
    bool                                                    logged_parallel_translate_wave{false};
    int32                      hard_failure_result{0};
    std::shared_ptr<Completion> stop_completion{};
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
