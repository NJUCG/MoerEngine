#include "VulkanInterruptRuntime.h"

#include "VulkanSubmissionRuntime.h"
#include "VulkanRHITrace.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/GPUEventStream.h"

#include <thread>

namespace Moer::Render {

VulkanInterruptRuntime::VulkanInterruptRuntime() :
    interrupt_thread([this]() { RunInterruptThread(); }) {}

VulkanInterruptRuntime::~VulkanInterruptRuntime() {
    Stop();
}

void VulkanInterruptRuntime::EnqueueQueueCompletion(
    uint64                          op_seq,
    VkCommandQueue*                 queue,
    GraphEventRef                   completion_event,
    WaitEvent                       completion,
    uint64                          timeline_value,
    UniquePtr<VulkanAllocator>&&    allocator,
    Array<std::function<void()>>&&  callbacks,
    Array<SignalEvent>&&            signal_events
) {
    if (queue == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        task_queue.emplace_back(SubmissionQueueCompletionTask{
            .op_seq             = op_seq,
            .queue              = queue,
            .completion_event   = completion_event,
            .completion         = completion,
            .timeline_value     = timeline_value,
            .allocator          = std::move(allocator),
            .callbacks          = std::move(callbacks),
            .signal_events      = std::move(signal_events),
            .pending_since      = std::chrono::steady_clock::now(),
            .pending_warn_count = 0,
        });
    }
    queue_cv.notify_one();
}

void VulkanInterruptRuntime::EnqueueCopyQueueCompletion(
    uint64                          op_seq,
    VkCopyQueue*                    queue,
    GraphEventRef                   completion_event,
    IOWaitEvt                       completion,
    uint64                          timeline_value,
    UniquePtr<VulkanAllocator>&&    allocator,
    Array<std::function<void()>>&&  callbacks,
    Array<IOSignalEvt>&&            signal_events
) {
    if (queue == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        task_queue.emplace_back(SubmissionCopyQueueCompletionTask{
            .op_seq             = op_seq,
            .queue              = queue,
            .completion_event   = completion_event,
            .completion         = completion,
            .timeline_value     = timeline_value,
            .allocator          = std::move(allocator),
            .callbacks          = std::move(callbacks),
            .signal_events      = std::move(signal_events),
            .pending_since      = std::chrono::steady_clock::now(),
            .pending_warn_count = 0,
        });
    }
    queue_cv.notify_one();
}

void VulkanInterruptRuntime::EnqueuePresentCompletion(
    uint64                        op_seq,
    SubmissionPresentContext*     context,
    GraphEventRef                 completion_event,
    VkDevice                      host_fence_device,
    VkFence                       host_fence,
    bool                          owns_host_fence,
    uint64                        timeline_value,
    UniquePtr<VulkanPresentor>&&  presentor
) {
    if (context == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        task_queue.emplace_back(SubmissionPresentCompletionTask{
            .op_seq             = op_seq,
            .context            = context,
            .completion_event   = completion_event,
            .host_fence_device  = host_fence_device,
            .host_fence         = host_fence,
            .owns_host_fence    = owns_host_fence,
            .host_fence_failed  = false,
            .timeline_value     = timeline_value,
            .presentor          = std::move(presentor),
            .pending_since      = std::chrono::steady_clock::now(),
            .pending_warn_count = 0,
        });
    }
    queue_cv.notify_one();
}

void VulkanInterruptRuntime::EnqueueFrameEndMarker(
    uint64 batch_id,
    uint32 submission_count,
    uint32 present_count
) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        task_queue.emplace_back(SubmissionFrameEndMarkerTask{
            .batch_id         = batch_id,
            .submission_count = submission_count,
            .present_count    = present_count,
        });
    }
    queue_cv.notify_one();
}

void VulkanInterruptRuntime::Shutdown() {
    Stop();
    if (interrupt_thread.joinable()) {
        interrupt_thread.join();
    }
}

void VulkanInterruptRuntime::Stop() {
    bool expected_running = true;
    if (!running.compare_exchange_strong(expected_running, false)) {
        return;
    }
    Array<SubmissionEventTask> abandoned_tasks{};
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        abandoned_tasks.reserve(task_queue.size());
        while (!task_queue.empty()) {
            abandoned_tasks.emplace_back(std::move(task_queue.front()));
            task_queue.pop_front();
        }
    }
    for (auto& task : abandoned_tasks) {
        UnlockTaskCompletion(task);
    }
    queue_cv.notify_all();
}

void VulkanInterruptRuntime::RunInterruptThread() {
    Platform::SetCurrentThreadName("InterruptThread");
    while (true) {
        SubmissionEventTask task{};
        if (!TryAcquireReadyTask(task)) {
            return;
        }
        ProcessTask(task);
        UnlockTaskCompletion(task);
    }
}

bool VulkanInterruptRuntime::TryAcquireReadyTask(SubmissionEventTask& task) {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this]() {
                return !running.load(std::memory_order_acquire) || !task_queue.empty();
            });
            if (!running.load(std::memory_order_acquire) && task_queue.empty()) {
                return false;
            }

            const size_t task_count = task_queue.size();
            for (size_t index = 0; index < task_count; ++index) {
                SubmissionEventTask candidate = std::move(task_queue.front());
                task_queue.pop_front();
                if (IsTaskReady(candidate)) {
                    task = std::move(candidate);
                    return true;
                }
                task_queue.emplace_back(std::move(candidate));
            }
        }

        if (!running.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::sleep_for(idle_poll_interval);
    }
}

bool VulkanInterruptRuntime::IsTaskReady(SubmissionEventTask& task) {
    return std::visit([this](auto& typed_task) { return IsTaskReady(typed_task); }, task);
}

bool VulkanInterruptRuntime::IsTaskReady(SubmissionQueueCompletionTask& task) {
    return PollTimelineCompletion(
        task.completion.timeline_handle,
        task.completion.value,
        task.op_seq,
        "queue completion",
        task.pending_since,
        task.pending_warn_count
    );
}

bool VulkanInterruptRuntime::IsTaskReady(SubmissionCopyQueueCompletionTask& task) {
    return PollTimelineCompletion(
        task.completion.handle,
        task.completion.timeline,
        task.op_seq,
        "copy completion",
        task.pending_since,
        task.pending_warn_count
    );
}

bool VulkanInterruptRuntime::IsTaskReady(SubmissionPresentCompletionTask& task) {
    return PollPresentFence(task);
}

bool VulkanInterruptRuntime::IsTaskReady(SubmissionFrameEndMarkerTask& task) {
    return true;
}

bool VulkanInterruptRuntime::PollTimelineCompletion(
    uint64                                timeline_handle,
    uint64                                timeline_value,
    uint64                                op_seq,
    const char*                           task_name,
    std::chrono::steady_clock::time_point pending_since,
    uint32&                               pending_warn_count
) {
    if (timeline_handle == 0 || timeline_value == 0) {
        return true;
    }

    auto* fence = reinterpret_cast<VulkanFence*>(timeline_handle);
    if (fence == nullptr) {
        return true;
    }

    if (fence->IsDeviceComplete(timeline_value)) {
        return true;
    }

    const auto now     = std::chrono::steady_clock::now();
    const auto elapsed = now - pending_since;
    constexpr auto warn_step = std::chrono::seconds(2);
    if (elapsed >= warn_step * (pending_warn_count + 1)) {
        LOG_WARNING(
            "Interrupt task still pending, kind={}, op_seq={}, timeline={}, elapsed_ms={}",
            task_name,
            op_seq,
            timeline_value,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        ++pending_warn_count;
    }
    return false;
}

bool VulkanInterruptRuntime::PollPresentFence(SubmissionPresentCompletionTask& task) {
    if (task.host_fence == VK_NULL_HANDLE || task.host_fence_device == VK_NULL_HANDLE) {
        return true;
    }

    const VkResult result = vkGetFenceStatus(task.host_fence_device, task.host_fence);
    if (result == VK_SUCCESS) {
        return true;
    }
    if (result != VK_NOT_READY) {
        LOG_ERROR(
            "Present fence query failed, op_seq={}, result={}",
            task.op_seq,
            int(result)
        );
        task.host_fence_failed = true;
        return true;
    }

    const auto now     = std::chrono::steady_clock::now();
    const auto elapsed = now - task.pending_since;
    constexpr auto warn_step = std::chrono::seconds(2);
    if (elapsed >= warn_step * (task.pending_warn_count + 1)) {
        LOG_WARNING(
            "Present fence still pending, op_seq={}, elapsed_ms={}",
            task.op_seq,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        ++task.pending_warn_count;
    }
    return false;
}

void VulkanInterruptRuntime::ProcessTask(SubmissionEventTask& task) {
    std::visit([this](auto& typed_task) { ProcessTask(typed_task); }, task);
}

void VulkanInterruptRuntime::ProcessTask(SubmissionQueueCompletionTask& completion_task) {
    completion_task.queue->ResolveAllocatorCompletion(
        std::move(completion_task.allocator), completion_task.timeline_value
    );
    for (auto& callback : completion_task.callbacks) {
        callback();
    }
    for (const auto& evt : completion_task.signal_events) {
        auto* fence = reinterpret_cast<VulkanFence*>(evt.timeline_handle);
        if (fence != nullptr) {
            fence->Notify(evt.value);
        }
    }
    completion_task.queue->MarkExecutionComplete(completion_task.timeline_value);
    GPUEventStream::Get().ResolveCompleted(completion_task.completion);
    GPUEventStream::Get().FlushToProfiler();
}

void VulkanInterruptRuntime::ProcessTask(SubmissionCopyQueueCompletionTask& completion_task) {
    completion_task.queue->ResolveAllocatorCompletion(
        std::move(completion_task.allocator), completion_task.timeline_value
    );
    for (auto& callback : completion_task.callbacks) {
        callback();
    }
    for (const auto& evt : completion_task.signal_events) {
        auto* fence = reinterpret_cast<VulkanFence*>(evt.handle);
        if (fence != nullptr) {
            fence->Notify(evt.timeline);
        }
    }
    completion_task.queue->MarkExecutionComplete(completion_task.timeline_value);
    GPUEventStream::Get().ResolveCompleted(
        WaitEvent{completion_task.completion.handle, completion_task.completion.timeline}
    );
    GPUEventStream::Get().FlushToProfiler();
}

void VulkanInterruptRuntime::ProcessTask(SubmissionPresentCompletionTask& completion_task) {
    if (!completion_task.host_fence_failed && completion_task.context != nullptr) {
        completion_task.context->ResolvePresentCompletion(
            std::move(completion_task.presentor), completion_task.timeline_value
        );
    }
    if (completion_task.owns_host_fence &&
        completion_task.host_fence_device != VK_NULL_HANDLE &&
        completion_task.host_fence != VK_NULL_HANDLE) {
        vkDestroyFence(
            completion_task.host_fence_device,
            completion_task.host_fence,
            VK_NULL_HANDLE
        );
    }
}

void VulkanInterruptRuntime::ProcessTask(SubmissionFrameEndMarkerTask& frame_end_task) {
    RHITRACE_LOG(
        basic,
        "[RHITrace][FrameEnd] batch={} submits={} presents={} ",
        frame_end_task.batch_id,
        frame_end_task.submission_count,
        frame_end_task.present_count
    );
}

void VulkanInterruptRuntime::UnlockTaskCompletion(SubmissionEventTask& task) {
    std::visit([this](auto& typed_task) { UnlockTaskCompletion(typed_task); }, task);
}

void VulkanInterruptRuntime::UnlockTaskCompletion(SubmissionQueueCompletionTask& task) {
    if (task.completion_event) {
        task.completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    }
}

void VulkanInterruptRuntime::UnlockTaskCompletion(SubmissionCopyQueueCompletionTask& task) {
    if (task.completion_event) {
        task.completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    }
}

void VulkanInterruptRuntime::UnlockTaskCompletion(SubmissionPresentCompletionTask& task) {
    if (task.completion_event) {
        task.completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    }
}

void VulkanInterruptRuntime::UnlockTaskCompletion(SubmissionFrameEndMarkerTask&) {}

} // namespace Moer::Render
