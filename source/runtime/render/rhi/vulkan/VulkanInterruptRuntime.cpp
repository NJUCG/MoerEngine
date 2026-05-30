#include "VulkanInterruptRuntime.h"

#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanSubmissionRuntime.h"
#include "VulkanRHITrace.h"
#include "VulkanThreadHeartbeat.h"
#include "log/LogSystem.h"
#include "rhi/GPUEventStream.h"

#include <cassert>
#include <thread>

namespace Moer::Render {

namespace {

void UnlockGraphEventOnTaskGraph(const GraphEventRef& event) {
    if (!event) {
        return;
    }
    GraphTask<EmptyGraphTask>::Create(EThread::AnyThread_NormalPri)
        .Next(event)
        .Dispatch(EThread::AnyThread_NormalPri);
}

void ExecuteCompletionEvents(Array<GraphEventRef>& completion_events) {
    for (const auto& event : completion_events) {
        UnlockGraphEventOnTaskGraph(event);
    }
    completion_events.clear();
}

} // namespace

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

VulkanInterruptRuntime::VulkanInterruptRuntime() :
    interrupt_runnable(MoerNew(InterruptRunnable)(*this)),
    interrupt_thread(RunnableThread::Create(
        interrupt_runnable,
        ThreadAttributes{.affinity = Affinity{}, .name = MOER_ASCII_TEXT("InterruptThread")}
    )) {}

VulkanInterruptRuntime::~VulkanInterruptRuntime() {
    Shutdown();
}

void VulkanInterruptRuntime::EnqueuePayload(UniquePtr<VulkanSubmitPayload>&& payload) {
    assert(payload != nullptr && "interrupt payload must be valid");
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        task_queue.emplace_back(std::move(payload));
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
    Array<UniquePtr<VulkanSubmitPayload>> abandoned_tasks{};
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        abandoned_tasks.reserve(task_queue.size());
        while (!task_queue.empty()) {
            abandoned_tasks.emplace_back(std::move(task_queue.front()));
            task_queue.pop_front();
        }
    }
    for (auto& payload : abandoned_tasks) {
        if (payload) {
            UnlockPayloadCompletion(*payload);
        }
    }
    queue_cv.notify_all();
}

void VulkanInterruptRuntime::RunInterruptThread() {
    auto& thread_heartbeat = VulkanThreadHeartbeat::Get();
    auto  heartbeat_handle =
        thread_heartbeat.Register(MOER_TEXT("InterruptThread"), MOER_TEXT("AcquireReadyTask"));
    while (true) {
        thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("AcquireReadyTask"));
        UniquePtr<VulkanSubmitPayload> payload{};
        if (!TryAcquireReadyPayload(payload)) {
            thread_heartbeat.Unregister(heartbeat_handle);
            return;
        }
        thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("ProcessTask"));
        ProcessPayload(*payload);
        thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("UnlockTaskCompletion"));
        UnlockPayloadCompletion(*payload);
    }
}

bool VulkanInterruptRuntime::TryAcquireReadyPayload(UniquePtr<VulkanSubmitPayload>& payload) {
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
                UniquePtr<VulkanSubmitPayload> candidate = std::move(task_queue.front());
                task_queue.pop_front();
                if (candidate && IsPayloadReady(*candidate)) {
                    payload = std::move(candidate);
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

bool VulkanInterruptRuntime::IsPayloadReady(VulkanSubmitPayload& payload) {
    switch (payload.type) {
        case EVulkanSubmitPayloadType::Queue:
            assert(payload.queue_owner != nullptr && "queue completion payload must have a queue owner");
            return PollHostFence(payload, "queue completion");
        case EVulkanSubmitPayloadType::Copy:
            assert(payload.copy_queue_owner != nullptr && "copy completion payload must have a queue owner");
            return PollHostFence(payload, "copy completion");
        case EVulkanSubmitPayloadType::Present:
            assert(payload.present_context != nullptr && "present completion payload must have a context");
            return PollHostFence(payload, "present completion");
    }
    return true;
}

bool VulkanInterruptRuntime::PollHostFence(
    VulkanSubmitPayload&                    payload,
    const char*                             task_name
) {
    if (payload.host_fence == VK_NULL_HANDLE) {
        return true;
    }

    VkDevice device = VK_NULL_HANDLE;
    switch (payload.type) {
        case EVulkanSubmitPayloadType::Queue:
            device = payload.queue_owner->vk_device.GetDevice();
            break;
        case EVulkanSubmitPayloadType::Copy:
            device = payload.copy_queue_owner->GetDevice().GetDevice();
            break;
        case EVulkanSubmitPayloadType::Present:
            if (payload.present_context == nullptr) {
                return true;
            }
            device = static_cast<VulkanDevice*>(RenderDevice::Get().GetImpl())->GetDevice();
            break;
    }
    if (device == VK_NULL_HANDLE) {
        return true;
    }

    const VkResult result = vkGetFenceStatus(device, payload.host_fence);
    if (result == VK_SUCCESS) {
        return true;
    }

    if (result != VK_NOT_READY) {
        LOG_ERROR(
            MOER_TEXT("Completion fence query failed, kind={}, op_seq={}, result={} "),
            task_name,
            payload.op_seq,
            int(result)
        );
        payload.host_fence_failed = true;
        return true;
    }

    const auto now     = std::chrono::steady_clock::now();
    const auto elapsed = now - payload.pending_since;
    constexpr auto warn_step = std::chrono::seconds(2);
    if (elapsed >= warn_step * (payload.pending_warn_count + 1)) {
        LOG_WARNING(
            MOER_TEXT("Interrupt task still pending, kind={}, op_seq={}, elapsed_ms={} "),
            task_name,
            payload.op_seq,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        ++payload.pending_warn_count;
    }
    return false;
}

void VulkanInterruptRuntime::ProcessPayload(VulkanSubmitPayload& payload) {
    for (auto& callback : payload.callbacks) {
        callback();
    }

    switch (payload.type) {
        case EVulkanSubmitPayloadType::Queue:
            if (payload.query_submission.has_value()) {
                payload.queue_owner->ResolveQueryCompletion(std::move(payload.query_submission.value()));
                payload.query_submission.reset();
            }
            payload.queue_owner->ResolveAllocatorCompletion(
                std::move(payload.allocator_owner), payload.timeline_value
            );
            for (const auto& evt : payload.signal_events) {
                auto* fence = reinterpret_cast<VulkanFence*>(evt.timeline_handle);
                if (fence != nullptr) {
                    fence->Notify(evt.value);
                }
            }
            payload.queue_owner->MarkExecutionComplete(payload.timeline_value);
            break;
        case EVulkanSubmitPayloadType::Copy:
            payload.copy_queue_owner->ResolveAllocatorCompletion(
                std::move(payload.allocator_owner), payload.timeline_value
            );
            for (const auto& evt : payload.signal_events) {
                auto* fence = reinterpret_cast<VulkanFence*>(evt.timeline_handle);
                if (fence != nullptr) {
                    fence->Notify(evt.value);
                }
            }
            payload.copy_queue_owner->MarkExecutionComplete(payload.timeline_value);
            break;
        case EVulkanSubmitPayloadType::Present:
            if (!payload.host_fence_failed && payload.present_context != nullptr) {
                payload.present_context->ResolvePresentCompletion(
                    std::move(payload.presentor), payload.timeline_value
                );
            }
            break;
    }

    GPUEventStream::Get().ResolveCompleted(payload.completion);
    GPUEventStream::Get().FlushToProfiler();
    ExecuteCompletionEvents(payload.completion_events);

    if (payload.owns_host_fence && payload.host_fence != VK_NULL_HANDLE) {
        switch (payload.type) {
            case EVulkanSubmitPayloadType::Queue:
                payload.queue_owner->vk_device.RecycleHostFence(payload.host_fence);
                break;
            case EVulkanSubmitPayloadType::Copy:
                payload.copy_queue_owner->GetDevice().RecycleHostFence(payload.host_fence);
                break;
            case EVulkanSubmitPayloadType::Present:
                static_cast<VulkanDevice*>(RenderDevice::Get().GetImpl())->RecycleHostFence(payload.host_fence);
                break;
        }
        payload.host_fence = VK_NULL_HANDLE;
        payload.owns_host_fence = false;
    }
}

void VulkanInterruptRuntime::UnlockPayloadCompletion(VulkanSubmitPayload& payload) {
    if (payload.completion_event) {
        payload.completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    }
}

} // namespace Moer::Render
