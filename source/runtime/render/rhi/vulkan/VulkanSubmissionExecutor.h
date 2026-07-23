#pragma once

#include "rhi/RHIExecutorBackend.h"

#include <condition_variable>
#include <deque>
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

    enum class ERequestKind : uint8_t {
        Submit,
        Sync,
        Stop,
    };

    struct Request {
        ERequestKind                kind{ERequestKind::Submit};
        RHIBackendSubmissionBatch   batch{};
        ERHISyncDepth               sync_depth{ERHISyncDepth::RHI};
        std::shared_ptr<Completion> completion{};
    };

    void Run();
    void ProcessBatch(RHIBackendSubmissionBatch&& _batch);
    void ProcessSync(ERHISyncDepth _depth);
    void RejectBatch(
        RHIBackendSubmissionBatch&& _batch,
        int32                       _result,
        std::string_view            _reason
    );
    void CompleteRequest(const std::shared_ptr<Completion>& _completion);

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
