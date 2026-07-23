#pragma once

#include "rhi/RHIExecutorBackend.h"
#include "VulkanSubmissionRequestDispatch.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace Moer::Render {

// Owns the upper RHI submission stream for Vulkan. Translation remains
// streaming (one source submission at a time) so descriptor leases can retire
// while later work is being prepared; native queue submission order is stable.
class VulkanSubmissionExecutor final : public RHIBackendExecutor {
public:
    VulkanSubmissionExecutor();
    ~VulkanSubmissionExecutor() override;

    void          Enqueue(RHIBackendSubmissionBatch&& _batch) override;
    GraphEventRef Sync(ERHISyncDepth _depth = ERHISyncDepth::RHI) override;
    void          Flush(ERHIFlushDepth _depth = ERHIFlushDepth::SubmitGPU) override;
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

    void Run();
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
    std::jthread               thread{};
    bool                       accepting{true};
    bool                       stopped{false};
    bool                       hard_failed{false};
    bool                       logged_multi_source_topology{false};
    int32                      hard_failure_result{0};
    std::shared_ptr<Completion> stop_completion{};
    StaticArray<bool, static_cast<size_t>(EQueueType::Num)> used_queues{};
    uint64                     last_copy_timeline{0};
    std::optional<EQueueType>  ordered_tail_queue{};
    std::optional<WaitEvent>   ordered_gpu_tail{};
};

} // namespace Moer::Render
