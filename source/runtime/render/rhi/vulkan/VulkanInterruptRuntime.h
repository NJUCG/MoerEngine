#pragma once

#include "VulkanSubmissionShared.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

namespace Moer::Render {

class SubmissionPresentContext;
struct SubmissionPresentResult;
struct VulkanQueueRuntimeSubmitResult;
struct VulkanCopyQueueRuntimeSubmitResult;

struct SubmissionCompletionCommon {
    using Clock = std::chrono::steady_clock;

    uint64            op_seq{0};
    GraphEventRef     completion_event{nullptr};
    Clock::time_point pending_since{};
    uint32            pending_warn_count{0};

    SubmissionCompletionCommon() = default;
    SubmissionCompletionCommon(uint64 in_op_seq, GraphEventRef in_completion_event);
};

struct SubmissionQueueCompletionPayload {
    VkCommandQueue*            queue{nullptr};
    WaitEvent                  completion{};
    uint64                     timeline_value{0};
    UniquePtr<VulkanAllocator> allocator{};
    Array<std::function<void()>> callbacks{};
    Array<SignalEvent>         signal_events{};

    SubmissionQueueCompletionPayload() = default;
    SubmissionQueueCompletionPayload(
        VkCommandQueue&                 in_queue,
        VulkanQueueRuntimeSubmitResult&& in_result
    );
};

struct SubmissionCopyQueueCompletionPayload {
    VkCopyQueue*               queue{nullptr};
    IOWaitEvt                  completion{};
    uint64                     timeline_value{0};
    UniquePtr<VulkanAllocator> allocator{};
    Array<std::function<void()>> callbacks{};
    Array<IOSignalEvt>         signal_events{};

    SubmissionCopyQueueCompletionPayload() = default;
    SubmissionCopyQueueCompletionPayload(
        VkCopyQueue&                     in_queue,
        VulkanCopyQueueRuntimeSubmitResult&& in_result
    );
};

struct SubmissionPresentCompletionPayload {
    SubmissionPresentContext*   context{nullptr};
    VkDevice                    host_fence_device{VK_NULL_HANDLE};
    VkFence                     host_fence{VK_NULL_HANDLE};
    bool                        owns_host_fence{false};
    bool                        host_fence_failed{false};
    uint64                      timeline_value{0};
    UniquePtr<VulkanPresentor>  presentor{};

    SubmissionPresentCompletionPayload() = default;
    SubmissionPresentCompletionPayload(
        SubmissionPresentContext& in_context,
        SubmissionHostFence       in_host_fence,
        SubmissionPresentResult&& in_result
    );
};

using SubmissionCompletionPayload = std::variant<
    SubmissionQueueCompletionPayload,
    SubmissionCopyQueueCompletionPayload,
    SubmissionPresentCompletionPayload>;

struct SubmissionCompletionTask {
    SubmissionCompletionCommon  common{};
    SubmissionCompletionPayload payload{};

    template<typename TPayload>
    static SubmissionCompletionTask Create(
        uint64        op_seq,
        GraphEventRef completion_event,
        TPayload&&    in_payload
    ) {
        SubmissionCompletionTask task{};
        task.common  = SubmissionCompletionCommon(op_seq, std::move(completion_event));
        task.payload = SubmissionCompletionPayload(std::forward<TPayload>(in_payload));
        return task;
    }
};

class VulkanInterruptRuntime {
public:
    VulkanInterruptRuntime();
    ~VulkanInterruptRuntime();

    VulkanInterruptRuntime(const VulkanInterruptRuntime&)                = delete;
    VulkanInterruptRuntime& operator=(const VulkanInterruptRuntime&)     = delete;
    VulkanInterruptRuntime(VulkanInterruptRuntime&&) noexcept            = delete;
    VulkanInterruptRuntime& operator=(VulkanInterruptRuntime&&) noexcept = delete;

    void          EnqueueTask(SubmissionCompletionTask&& task);
    void          Shutdown();

private:
    void   Stop();
    void   RunInterruptThread();
    bool   TryAcquireReadyTask(SubmissionCompletionTask& task);
    bool   IsTaskReady(SubmissionCompletionTask& task);
    bool   PollTimelineCompletion(
               uint64                                     timeline_handle,
               uint64                                     timeline_value,
               uint64                                     op_seq,
               const char*                                task_name,
               std::chrono::steady_clock::time_point      pending_since,
               uint32&                                    pending_warn_count
           );
    bool   PollPresentFence(SubmissionCompletionTask& task, SubmissionPresentCompletionPayload& payload);
    void   ProcessTask(SubmissionCompletionTask& task);
    void   UnlockTaskCompletion(SubmissionCompletionTask& task);

private:
    std::atomic_bool     running{true};

    std::mutex                       queue_mutex{};
    std::condition_variable          queue_cv{};
    std::deque<SubmissionCompletionTask> task_queue{};
    std::jthread                     interrupt_thread{};
    std::chrono::microseconds        idle_poll_interval{100};
};

} // namespace Moer::Render
