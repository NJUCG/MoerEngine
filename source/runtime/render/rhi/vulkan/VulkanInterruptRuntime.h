#pragma once

#include "VulkanSubmissionShared.h"
#include "taskgraph/ThreadManager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace Moer::Render {

class SubmissionPresentContext;
struct SubmissionPresentResult;

class VulkanInterruptRuntime {
public:
    VulkanInterruptRuntime();
    ~VulkanInterruptRuntime();

    VulkanInterruptRuntime(const VulkanInterruptRuntime&)                = delete;
    VulkanInterruptRuntime& operator=(const VulkanInterruptRuntime&)     = delete;
    VulkanInterruptRuntime(VulkanInterruptRuntime&&) noexcept            = delete;
    VulkanInterruptRuntime& operator=(VulkanInterruptRuntime&&) noexcept = delete;

    void          EnqueuePayload(UniquePtr<VulkanSubmitPayload>&& payload);
    void          Shutdown();

private:
    class InterruptRunnable;

    void   Stop();
    void   RunInterruptThread();
    bool   TryAcquireReadyPayload(UniquePtr<VulkanSubmitPayload>& payload);
    bool   IsPayloadReady(VulkanSubmitPayload& payload);
    bool   PollHostFence(
               VulkanSubmitPayload&                       payload,
               const char*                                task_name
           );
    void   ProcessPayload(VulkanSubmitPayload& payload);
    void   UnlockPayloadCompletion(VulkanSubmitPayload& payload);

private:
    std::atomic_bool     running{true};

    std::mutex                       queue_mutex{};
    std::condition_variable          queue_cv{};
    std::deque<UniquePtr<VulkanSubmitPayload>> task_queue{};
    std::chrono::microseconds        idle_poll_interval{100};
    InterruptRunnable*               interrupt_runnable{nullptr};
    RunnableThread*                  interrupt_thread{nullptr};
};

} // namespace Moer::Render
