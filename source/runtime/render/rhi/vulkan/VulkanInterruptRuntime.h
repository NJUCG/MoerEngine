#pragma once

#include "VulkanSubmissionShared.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <variant>

namespace Moer::Render {

class SubmissionPresentContext;

struct SubmissionQueueCompletionTask {
    using Clock = std::chrono::steady_clock;

    uint64                     op_seq{0};
    VkCommandQueue*            queue{nullptr};
    GraphEventRef              completion_event{nullptr};
    WaitEvent                  completion{};
    uint64                     timeline_value{0};
    UniquePtr<VulkanAllocator> allocator{};
    Array<std::function<void()>> callbacks{};
    Array<SignalEvent>         signal_events{};
    Clock::time_point          pending_since{};
    uint32                     pending_warn_count{0};
};

struct SubmissionCopyQueueCompletionTask {
    using Clock = std::chrono::steady_clock;

    uint64                     op_seq{0};
    VkCopyQueue*               queue{nullptr};
    GraphEventRef              completion_event{nullptr};
    IOWaitEvt                  completion{};
    uint64                     timeline_value{0};
    UniquePtr<VulkanAllocator> allocator{};
    Array<std::function<void()>> callbacks{};
    Array<IOSignalEvt>         signal_events{};
    Clock::time_point          pending_since{};
    uint32                     pending_warn_count{0};
};

struct SubmissionPresentCompletionTask {
    using Clock = std::chrono::steady_clock;

    uint64                      op_seq{0};
    SubmissionPresentContext*   context{nullptr};
    GraphEventRef               completion_event{nullptr};
    VkDevice                    host_fence_device{VK_NULL_HANDLE};
    VkFence                     host_fence{VK_NULL_HANDLE};
    bool                        owns_host_fence{false};
    bool                        host_fence_failed{false};
    uint64                      timeline_value{0};
    UniquePtr<VulkanPresentor>  presentor{};
    Clock::time_point           pending_since{};
    uint32                      pending_warn_count{0};
};

struct SubmissionFrameEndMarkerTask {
    uint64 batch_id{0};
    uint32 submission_count{0};
    uint32 present_count{0};
};

using SubmissionEventTask = std::variant<
    SubmissionQueueCompletionTask,
    SubmissionCopyQueueCompletionTask,
    SubmissionPresentCompletionTask,
    SubmissionFrameEndMarkerTask>;

class VulkanInterruptRuntime {
public:
    VulkanInterruptRuntime();
    ~VulkanInterruptRuntime();

    VulkanInterruptRuntime(const VulkanInterruptRuntime&)                = delete;
    VulkanInterruptRuntime& operator=(const VulkanInterruptRuntime&)     = delete;
    VulkanInterruptRuntime(VulkanInterruptRuntime&&) noexcept            = delete;
    VulkanInterruptRuntime& operator=(VulkanInterruptRuntime&&) noexcept = delete;

    void          EnqueueQueueCompletion(
                 uint64                        op_seq,
                 VkCommandQueue*               queue,
                 GraphEventRef                 completion_event,
                 WaitEvent                     completion,
                 uint64                        timeline_value,
                 UniquePtr<VulkanAllocator>&&  allocator,
                 Array<std::function<void()>>&& callbacks,
                 Array<SignalEvent>&&          signal_events
             );
    void          EnqueueCopyQueueCompletion(
                 uint64                        op_seq,
                 VkCopyQueue*                  queue,
                 GraphEventRef                 completion_event,
                 IOWaitEvt                     completion,
                 uint64                        timeline_value,
                 UniquePtr<VulkanAllocator>&&  allocator,
                 Array<std::function<void()>>&& callbacks,
                 Array<IOSignalEvt>&&          signal_events
             );
    void          EnqueuePresentCompletion(
                 uint64                       op_seq,
                 SubmissionPresentContext*    context,
                 GraphEventRef                completion_event,
                 VkDevice                     host_fence_device,
                 VkFence                      host_fence,
                 bool                         owns_host_fence,
                 uint64                       timeline_value,
                 UniquePtr<VulkanPresentor>&& presentor
             );
    void          EnqueueFrameEndMarker(uint64 batch_id, uint32 submission_count, uint32 present_count);
    void          Shutdown();

private:
    void   Stop();
    void   RunInterruptThread();
    bool   TryAcquireReadyTask(SubmissionEventTask& task);
    bool   IsTaskReady(SubmissionEventTask& task);
    bool   IsTaskReady(SubmissionQueueCompletionTask& task);
    bool   IsTaskReady(SubmissionCopyQueueCompletionTask& task);
    bool   IsTaskReady(SubmissionPresentCompletionTask& task);
    bool   IsTaskReady(SubmissionFrameEndMarkerTask& task);
    bool   PollTimelineCompletion(
               uint64                                     timeline_handle,
               uint64                                     timeline_value,
               uint64                                     op_seq,
               const char*                                task_name,
               std::chrono::steady_clock::time_point      pending_since,
               uint32&                                    pending_warn_count
           );
    bool   PollPresentFence(SubmissionPresentCompletionTask& task);
    void   ProcessTask(SubmissionEventTask& task);
    void   ProcessTask(SubmissionQueueCompletionTask& task);
    void   ProcessTask(SubmissionCopyQueueCompletionTask& task);
    void   ProcessTask(SubmissionPresentCompletionTask& task);
    void   ProcessTask(SubmissionFrameEndMarkerTask& task);
    void   UnlockTaskCompletion(SubmissionEventTask& task);
    void   UnlockTaskCompletion(SubmissionQueueCompletionTask& task);
    void   UnlockTaskCompletion(SubmissionCopyQueueCompletionTask& task);
    void   UnlockTaskCompletion(SubmissionPresentCompletionTask& task);
    void   UnlockTaskCompletion(SubmissionFrameEndMarkerTask& task);

private:
    std::atomic_bool     running{true};

    std::mutex                    queue_mutex{};
    std::condition_variable       queue_cv{};
    std::deque<SubmissionEventTask> task_queue{};
    std::jthread                  interrupt_thread{};
    std::chrono::microseconds     idle_poll_interval{100};
};

} // namespace Moer::Render
