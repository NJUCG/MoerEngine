#pragma once

#include "VulkanSubmissionShared.h"
#include "taskgraph/ThreadManager.h"

#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

namespace Moer::Render {

class VulkanInterruptRuntime;
class VkCommandQueue;
class VkCopyQueue;

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
    void AppendCompletionBoundary(Swapchain* swapchain, const GraphEventRef& completion_event);
    void ResetCompletionBoundary();
    const GraphEventRef& GetCompletionBoundary() const;
    const GraphEventRef& GetCompletionBoundary(Swapchain* swapchain) const;

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
    Array<std::shared_ptr<TranslateBatch>> translate_batches{};
    Array<SubmitPresentStage>          present_ops{};
    RootRhiBoundary                    root_rhi_boundary{};
    ERHISyncDepth                      sync_depth{ERHISyncDepth::RHI};
    Swapchain*                         sync_swapchain{nullptr};
    GraphEventRef                      completion_event{nullptr};
};

struct QueueRuntimeState {
    uint64 timeline_handle{0};
    uint64 next_signal_value{1};
};

class SubmissionQueueStateSet {
public:
    QueueRuntimeState& Get(EQueueType queue);
    const QueueRuntimeState& Get(EQueueType queue) const;

private:
    std::array<QueueRuntimeState, static_cast<size_t>(EQueueType::Num)> states{};
};

struct SubmitRuntimeCache {
    bool             ready{false};
    bool             submitted{false};
    Array<WaitEvent> resolved_waits{};
};

struct OrderedBatchRuntimeState {
    Array<SubmitInfo>         submits{};
    Array<SubmitRuntimeCache> cache{};
    RootRhiBoundary           root_rhi_boundary{};
    uint32                    next_submit_index{0};
};

struct SubmissionSchedulerState {
    SubmissionQueueStateSet queue_states{};
};

class VulkanSubmissionRuntime {
public:
    explicit VulkanSubmissionRuntime(VulkanInterruptRuntime& interrupt_runtime);
    ~VulkanSubmissionRuntime();

    VulkanSubmissionRuntime(const VulkanSubmissionRuntime&)                = delete;
    VulkanSubmissionRuntime& operator=(const VulkanSubmissionRuntime&)     = delete;
    VulkanSubmissionRuntime(VulkanSubmissionRuntime&&) noexcept            = delete;
    VulkanSubmissionRuntime& operator=(VulkanSubmissionRuntime&&) noexcept = delete;

    void Enqueue(Array<SubmitInfo>&& submits, Array<SubmitPresentStage>&& present_ops);
    void Enqueue(Array<std::shared_ptr<TranslateBatch>>&& translate_batches, Array<SubmitPresentStage>&& present_ops);
    GraphEventRef Sync(ERHISyncDepth depth);
    GraphEventRef Sync(Swapchain* swapchain);
    void Drain();
    void Flush();
    void Shutdown();

private:
    class SubmissionRunnable;

    void             BindSubmitBatchRootBoundary(SubmissionBatch& batch);
    OrderedBatchRuntimeState InitOrderedBatchRuntimeState(SubmissionBatch& batch);
    void             MaterializeTranslateBatches(SubmissionBatch& batch);
    void             RunOrderedSubmitBatch(SubmissionBatch& batch);
    void             ScanBatchReadiness(OrderedBatchRuntimeState& state);
    uint32           FlushPendingReady(OrderedBatchRuntimeState& state);
    bool             TryResolveWaitSyncPoints(
                         std::span<const SyncPointRef> wait_syncpoints,
                         Array<WaitEvent>&            out_resolved_waits
                     ) const;
    Array<WaitEvent> BuildResolvedWaits(
        const OrderedBatchRuntimeState& state,
        const SubmitInfo&               submit,
        std::span<const WaitEvent>      resolved_waits
    ) const;
    void             PublishResolvedSyncPoint(
                         const SyncPointRef& syncpoint,
                         WaitEvent           resolved_wait
                     );
    RootRhiBoundary  BuildBatchTailBoundary(const OrderedBatchRuntimeState& state) const;
    void             ExecutePresentStages(
                         const Array<SubmitPresentStage>& present_ops,
                         std::span<const WaitEvent>       initial_waits
                     );
    std::optional<WaitEvent> ExecutePresentStage(
        const SubmitPresentStage& present_stage,
        std::span<const WaitEvent> wait_events
    );
    void             JoinSubmissionThread();
    void             RunSubmissionThread();
    SubmissionPresentContext& GetOrCreatePresentContext(EQueueType queue_type);
    void             FlushPresentContexts();
    void             ShutdownPresentContexts();
    void             ResetOwnerCompletionBoundaries();
    GraphEventRef    EnqueueOrderedSyncRequest(ERHISyncDepth depth, SubmissionBatch::EKind kind);
    GraphEventRef    EnqueueOrderedSwapchainSyncRequest(Swapchain* swapchain);
    GraphEventRef    EnqueueOrderedSyncRequestUnchecked(ERHISyncDepth depth, SubmissionBatch::EKind kind);
    GraphEventRef    EnqueueDrainRequestUnchecked();
    void             AttachSyncDependencies(SubmissionBatch& batch);

private:
    VulkanInterruptRuntime& interrupt_runtime;
    VkCommandQueue&         graphics_queue_owner;
    VkCommandQueue&         compute_queue_owner;
    VkCopyQueue&            copy_queue_owner;
    SubmissionSchedulerState scheduler_state{};
    std::atomic_bool        b_enable{true};

    std::mutex              submission_mutex{};
    std::condition_variable submission_cv{};
    std::deque<SubmissionBatch> submission_queue{};
    RunnableThread*        submission_thread{nullptr};
    SubmissionRunnable*    submission_runnable{nullptr};

    RootRhiBoundary         pending_rhi_boundary{};
    std::mutex              present_context_mutex{};
    std::array<std::unique_ptr<SubmissionPresentContext>, static_cast<size_t>(EQueueType::Num)>
        present_contexts{};
    uint64 next_submit_seq{0};
};

} // namespace Moer::Render
