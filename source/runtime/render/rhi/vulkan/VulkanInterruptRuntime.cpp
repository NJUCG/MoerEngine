#include "VulkanInterruptRuntime.h"

#include "VulkanQueue.h"
#include "VulkanSubmissionRuntime.h"
#include "VulkanRHITrace.h"
#include "log/LogSystem.h"
#include "rhi/GPUEventStream.h"

#include <cassert>
#include <type_traits>
#include <thread>

namespace Moer::Render {

class VulkanInterruptRuntime::InterruptRunnable : public Runnable {
public:
    explicit InterruptRunnable(VulkanInterruptRuntime& in_owner) : m_owner(in_owner) {}

    uint32_t Run() override {
        m_owner.RunInterruptThread();
        return 0;
    }

    void Init() override {}
    void Stop() override {
        m_owner.Stop();
    }
    void Exit() override {}
    ThreadIndex GetIndex() override {
        return EThread::UNKNOWN_THREAD;
    }

private:
    VulkanInterruptRuntime& m_owner;
};

SubmissionCompletionCommon::SubmissionCompletionCommon(
    uint64        in_op_seq,
    GraphEventRef in_completion_event
) :
    op_seq(in_op_seq),
    completion_event(std::move(in_completion_event)),
    pending_since(Clock::now()) {
    assert(completion_event != nullptr && "completion task must have a completion event");
}

SubmissionQueueCompletionPayload::SubmissionQueueCompletionPayload(
    VkCommandQueue&                 in_queue,
    VulkanQueueRuntimeSubmitResult&& in_result
) :
    queue(&in_queue),
    completion(in_result.completion),
    timeline_value(in_result.timeline_value),
    allocator(std::move(in_result.allocator)),
    callbacks(std::move(in_result.callbacks)),
    signal_events(std::move(in_result.signal_events)) {}

SubmissionCopyQueueCompletionPayload::SubmissionCopyQueueCompletionPayload(
    VkCopyQueue&                     in_queue,
    VulkanCopyQueueRuntimeSubmitResult&& in_result
) :
    queue(&in_queue),
    completion(in_result.completion),
    timeline_value(in_result.timeline_value),
    allocator(std::move(in_result.allocator)),
    callbacks(std::move(in_result.callbacks)),
    signal_events(std::move(in_result.signal_events)) {}

SubmissionPresentCompletionPayload::SubmissionPresentCompletionPayload(
    SubmissionPresentContext& in_context,
    SubmissionHostFence       in_host_fence,
    SubmissionPresentResult&& in_result
) :
    context(&in_context),
    host_fence_device(in_host_fence.device),
    host_fence(in_host_fence.handle),
    owns_host_fence(in_host_fence.owned),
    completion(in_result.completion),
    timeline_value(in_result.timeline_value),
    presentor(std::move(in_result.presentor)) {}

VulkanInterruptRuntime::VulkanInterruptRuntime() :
    interrupt_runnable(MoerNew(InterruptRunnable)(*this)),
    interrupt_thread(RunnableThread::Create(
        interrupt_runnable,
        ThreadAttributes{.affinity = Affinity{}, .name = MOER_ASCII_TEXT("InterruptThread")}
    )) {}

VulkanInterruptRuntime::~VulkanInterruptRuntime() {
    Shutdown();
}

void VulkanInterruptRuntime::EnqueueTask(SubmissionCompletionTask&& task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        task_queue.emplace_back(std::move(task));
    }
    queue_cv.notify_one();
}

void VulkanInterruptRuntime::Shutdown() {
    Stop();
    if (interrupt_thread != nullptr) {
        MoerDelete(interrupt_thread);
        interrupt_thread = nullptr;
    }
    if (interrupt_runnable != nullptr) {
        MoerDelete(interrupt_runnable);
        interrupt_runnable = nullptr;
    }
}

void VulkanInterruptRuntime::Stop() {
    bool expected_running = true;
    if (!running.compare_exchange_strong(expected_running, false)) {
        return;
    }
    Array<SubmissionCompletionTask> abandoned_tasks{};
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
    while (true) {
        SubmissionCompletionTask task{};
        if (!TryAcquireReadyTask(task)) {
            return;
        }
        ProcessTask(task);
        UnlockTaskCompletion(task);
    }
}

bool VulkanInterruptRuntime::TryAcquireReadyTask(SubmissionCompletionTask& task) {
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
                SubmissionCompletionTask candidate = std::move(task_queue.front());
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

bool VulkanInterruptRuntime::IsTaskReady(SubmissionCompletionTask& task) {
    return std::visit(
        [this, &task](auto& payload) -> bool {
            using PayloadType = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<PayloadType, SubmissionQueueCompletionPayload>) {
                assert(payload.queue != nullptr && "queue completion task must have a queue");
                return PollTimelineCompletion(
                    payload.completion.timeline_handle,
                    payload.completion.value,
                    task.common.op_seq,
                    "queue completion",
                    task.common.pending_since,
                    task.common.pending_warn_count
                );
            } else if constexpr (std::is_same_v<PayloadType, SubmissionCopyQueueCompletionPayload>) {
                assert(payload.queue != nullptr && "copy completion task must have a queue");
                return PollTimelineCompletion(
                    payload.completion.handle,
                    payload.completion.timeline,
                    task.common.op_seq,
                    "copy completion",
                    task.common.pending_since,
                    task.common.pending_warn_count
                );
            } else {
                assert(payload.context != nullptr && "present completion task must have a context");
                return PollPresentFence(task, payload);
            }
        },
        task.payload
    );
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
            MOER_TEXT("Interrupt task still pending, kind={}, op_seq={}, timeline={}, elapsed_ms={}"),
            task_name,
            op_seq,
            timeline_value,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        ++pending_warn_count;
    }
    return false;
}

bool VulkanInterruptRuntime::PollPresentFence(
    SubmissionCompletionTask&          task,
    SubmissionPresentCompletionPayload& payload
) {
    if (!PollTimelineCompletion(
            payload.completion.timeline_handle,
            payload.completion.value,
            task.common.op_seq,
            "present submit completion",
            task.common.pending_since,
            task.common.pending_warn_count
        )) {
        return false;
    }

    if (payload.host_fence == VK_NULL_HANDLE || payload.host_fence_device == VK_NULL_HANDLE) {
        return true;
    }

    const VkResult result = vkGetFenceStatus(payload.host_fence_device, payload.host_fence);
    if (result == VK_SUCCESS) {
        return true;
    }
    if (result != VK_NOT_READY) {
        LOG_ERROR(
            MOER_TEXT("Present fence query failed, op_seq={}, result={}"),
            task.common.op_seq,
            int(result)
        );
        payload.host_fence_failed = true;
        return true;
    }

    const auto now     = std::chrono::steady_clock::now();
    const auto elapsed = now - task.common.pending_since;
    constexpr auto warn_step = std::chrono::seconds(2);
    if (elapsed >= warn_step * (task.common.pending_warn_count + 1)) {
        LOG_WARNING(
            MOER_TEXT("Present fence still pending, op_seq={}, elapsed_ms={}"),
            task.common.op_seq,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        ++task.common.pending_warn_count;
    }
    return false;
}

void VulkanInterruptRuntime::ProcessTask(SubmissionCompletionTask& task) {
    std::visit(
        [](auto& payload) {
            using PayloadType = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<PayloadType, SubmissionQueueCompletionPayload>) {
                assert(payload.queue != nullptr && "queue completion task must have a queue");
                payload.queue->ResolveAllocatorCompletion(
                    std::move(payload.allocator), payload.timeline_value
                );
                for (auto& callback : payload.callbacks) {
                    callback();
                }
                for (const auto& evt : payload.signal_events) {
                    auto* fence = reinterpret_cast<VulkanFence*>(evt.timeline_handle);
                    if (fence != nullptr) {
                        fence->Notify(evt.value);
                    }
                }
                payload.queue->MarkExecutionComplete(payload.timeline_value);
                GPUEventStream::Get().ResolveCompleted(payload.completion);
                GPUEventStream::Get().FlushToProfiler();
            } else if constexpr (std::is_same_v<PayloadType, SubmissionCopyQueueCompletionPayload>) {
                assert(payload.queue != nullptr && "copy completion task must have a queue");
                payload.queue->ResolveAllocatorCompletion(
                    std::move(payload.allocator), payload.timeline_value
                );
                for (auto& callback : payload.callbacks) {
                    callback();
                }
                for (const auto& evt : payload.signal_events) {
                    auto* fence = reinterpret_cast<VulkanFence*>(evt.handle);
                    if (fence != nullptr) {
                        fence->Notify(evt.timeline);
                    }
                }
                payload.queue->MarkExecutionComplete(payload.timeline_value);
                GPUEventStream::Get().ResolveCompleted(
                    WaitEvent{payload.completion.handle, payload.completion.timeline}
                );
                GPUEventStream::Get().FlushToProfiler();
            } else {
                assert(payload.context != nullptr && "present completion task must have a context");
                if (!payload.host_fence_failed) {
                    payload.context->ResolvePresentCompletion(
                        std::move(payload.presentor), payload.timeline_value
                    );
                }
                if (payload.owns_host_fence &&
                    payload.host_fence_device != VK_NULL_HANDLE &&
                    payload.host_fence != VK_NULL_HANDLE) {
                    vkDestroyFence(
                        payload.host_fence_device,
                        payload.host_fence,
                        VK_NULL_HANDLE
                    );
                }
            }
        },
        task.payload
    );
}

void VulkanInterruptRuntime::UnlockTaskCompletion(SubmissionCompletionTask& task) {
    if (task.common.completion_event) {
        task.common.completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    }
}

} // namespace Moer::Render
