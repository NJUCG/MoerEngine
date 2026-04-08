#pragma once

#include "VulkanSubmissionShared.h"

#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <span>
#include <thread>

namespace Moer::Render {

class VulkanInterruptRuntime;

struct SubmissionPresentResult {
    bool                       submitted{false};
    WaitEvent                  completion{};
    uint64                     timeline_value{0};
    UniquePtr<VulkanPresentor> presentor{};
};

class SubmissionPresentContext {
public:
    explicit SubmissionPresentContext(EQueueType in_queue_type);
    ~SubmissionPresentContext();

    SubmissionPresentContext(const SubmissionPresentContext&)                = delete;
    SubmissionPresentContext& operator=(const SubmissionPresentContext&)     = delete;
    SubmissionPresentContext(SubmissionPresentContext&&) noexcept            = delete;
    SubmissionPresentContext& operator=(SubmissionPresentContext&&) noexcept = delete;

    SubmissionPresentResult Present(
        const ExecutorPresentOp&   present_op,
        std::span<const WaitEvent> wait_events,
        const ResourceStateValue*  source_texture_state,
        SubmissionHostFence&       out_host_fence
    );
    void Flush();
    void Shutdown();
    void ResolvePresentCompletion(UniquePtr<VulkanPresentor>&& presentor, uint64 timeline_value);

private:
    struct Impl;
    UniquePtr<Impl> impl{};
};

struct SubmissionBatch {
    enum class EKind : uint8 {
        Submit,
        Drain,
        Sync,
        Flush,
    };

    EKind                              kind{EKind::Submit};
    Array<SubmitInfo>                  submits{};
    Array<WaitEvent>                   root_rhi_prerequisites{};
    bool                               frame_end{false};
    uint64                             batch_id{0};
    ERHISyncDepth                      sync_depth{ERHISyncDepth::RHI};
    std::shared_ptr<std::promise<void>> completion{};
    std::shared_ptr<std::promise<GraphEventRef>> sync_completion{};
};

class VulkanSubmissionRuntime {
public:
    explicit VulkanSubmissionRuntime(VulkanInterruptRuntime& interrupt_runtime);
    ~VulkanSubmissionRuntime();

    VulkanSubmissionRuntime(const VulkanSubmissionRuntime&)                = delete;
    VulkanSubmissionRuntime& operator=(const VulkanSubmissionRuntime&)     = delete;
    VulkanSubmissionRuntime(VulkanSubmissionRuntime&&) noexcept            = delete;
    VulkanSubmissionRuntime& operator=(VulkanSubmissionRuntime&&) noexcept = delete;

    void Enqueue(Array<SubmitInfo>&& submits, bool frame_end);
    GraphEventRef Sync(ERHISyncDepth depth);
    void Drain();
    void Flush();
    void Shutdown();

private:
    void             Stop();
    void             RunSubmissionThread();
    SubmissionPresentContext& GetOrCreatePresentContext(EQueueType queue_type);
    void             FlushPresentContexts();
    void             ShutdownPresentContexts();
    GraphEventRef    EnqueueOrderedSyncRequest(ERHISyncDepth depth, SubmissionBatch::EKind kind);
    Array<WaitEvent> SnapshotPendingRHITails();
    void             StorePendingRHITails(Array<WaitEvent>&& tails);
    static GraphEventRef CreateCompletedEvent();
    static GraphEventRef CreateFrozenSyncEvent(
                    ERHISyncDepth      depth,
                    const GraphEventRef& rhi_tail,
                    const GraphEventRef& present_tail
                );
    static void FoldSemanticTail(GraphEventRef& tail, const GraphEventRef& completion_event);

private:
    VulkanInterruptRuntime& interrupt_runtime;
    std::atomic_bool        running{true};
    std::atomic_uint64_t    next_batch_id{1};

    std::mutex              submission_mutex{};
    std::condition_variable submission_cv{};
    std::deque<SubmissionBatch> submission_queue{};
    std::jthread            submission_thread{};

    std::mutex              tail_mutex{};
    Array<WaitEvent>        pending_rhi_tails{};
    GraphEventRef           rhi_tail{nullptr};
    GraphEventRef           present_tail{nullptr};
    std::mutex              present_context_mutex{};
    std::array<std::unique_ptr<SubmissionPresentContext>, static_cast<size_t>(EQueueType::Num)>
        present_contexts{};
};

} // namespace Moer::Render
