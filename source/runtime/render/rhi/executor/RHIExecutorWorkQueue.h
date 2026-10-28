#pragma once

#include "RenderAPI.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace Moer::Render {

class RHIThreadHeartbeatScope;

enum class ERHIRecordingStatus : uint8_t {
    Pending,
    Succeeded,
    Failed,
};

// One-shot CPU publication gate used to hand a CommandList to the RHI
// executor before its producer has finished recording it. Signal() publishes
// all writes performed by the recorder before the call; the Executor worker
// acquires them before consuming the CommandList.
class RENDER_API RHIRecordingGate final {
public:
    static std::shared_ptr<RHIRecordingGate> Create(bool _initially_complete = false);

    // Idempotent. Returns true only for the transition to the complete state.
    bool                              Signal() noexcept;
    bool                              Fail() noexcept;
    [[nodiscard]] bool                IsComplete() const noexcept;
    [[nodiscard]] ERHIRecordingStatus Status() const noexcept;
    // Blocking producer-side join used at explicit RDG wave boundaries.
    // Unlike the worker overload, this wait is not cancellable and always
    // returns a terminal Succeeded/Failed status.
    [[nodiscard]] ERHIRecordingStatus Wait() const noexcept;

private:
    friend class RHIExecutorWorkQueue;

    explicit RHIRecordingGate(bool _initially_complete) :
        status(
            _initially_complete ? ERHIRecordingStatus::Succeeded :
                                  ERHIRecordingStatus::Pending
        ) {}
    bool                              Complete(ERHIRecordingStatus _status) noexcept;
    [[nodiscard]] ERHIRecordingStatus Wait(std::stop_token _stop_token) const noexcept;

    mutable std::mutex                  mutex;
    mutable std::condition_variable_any cv;
    ERHIRecordingStatus                 status{ERHIRecordingStatus::Pending};
};

using RHIRecordingGateRef = std::shared_ptr<RHIRecordingGate>;

// Read-only publication capability. Producers retain RHIRecordingGateRef and
// therefore the only Signal/Fail authority; setup/publisher extensions and the
// RHI consumer may observe status but cannot complete a gate early.
class RENDER_API RHIRecordingGateView final {
public:
    RHIRecordingGateView() = default;
    RHIRecordingGateView(const RHIRecordingGateRef& _gate) : gate(_gate) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(gate);
    }

    [[nodiscard]] bool IsComplete() const noexcept {
        return gate && gate->IsComplete();
    }

    [[nodiscard]] ERHIRecordingStatus Status() const noexcept {
        return gate ? gate->Status() : ERHIRecordingStatus::Pending;
    }

    friend bool operator==(
        const RHIRecordingGateView& _lhs,
        const RHIRecordingGateView& _rhs
    ) noexcept {
        return _lhs.gate == _rhs.gate;
    }

private:
    friend class RHIExecutor;
    RHIRecordingGateRef gate{};
};

enum class ERHIExecutorWorkAction : uint8_t {
    // All prerequisites succeeded; the callback may process the work normally.
    Execute,
    // Every prerequisite reached a terminal state, but at least one producer
    // failed. The callback may safely inspect every protected CommandList for
    // frontend cleanup, but must never publish its commands to a GPU queue.
    Reject,
    // The work was cancelled during shutdown or enqueued while stopped.
    // Prerequisites may still be pending; protected CommandLists remain opaque.
    Cancel,
};

// Optional CPU recording prerequisites and one callback for normal processing
// or rejection/cancellation cleanup. An empty prerequisite list still respects FIFO.
struct RHIExecutorWork {
    std::vector<RHIRecordingGateRef>            prerequisites{};
    std::function<void(ERHIExecutorWorkAction)> callback{};
};

// Owns the Executor worker and its FIFO. Submit, SubmitRecording, Present, Flush
// and Sync share this queue so ready work cannot overtake pending recording.
// A failed group waits for every prerequisite before receiving Reject.
// Shutdown drains ready work; an interrupted prerequisite wait cancels that
// item and the remaining FIFO without waiting for producers to finish.
class RENDER_API RHIExecutorWorkQueue final {
public:
    RHIExecutorWorkQueue() = default;
    ~RHIExecutorWorkQueue();

    RHIExecutorWorkQueue(const RHIExecutorWorkQueue&)            = delete;
    RHIExecutorWorkQueue& operator=(const RHIExecutorWorkQueue&) = delete;

    void Start();

    // Accepted work always runs on the Executor worker, even without prerequisites.
    // If stopped, invokes the callback with Cancel on the calling thread.
    // If insertion throws, ownership stays with the caller for cleanup.
    void Enqueue(RHIExecutorWork&& _work);

    void               ShutDown();
    [[nodiscard]] bool IsIdle() const noexcept;

private:
    void Run(std::stop_token _stop_token) noexcept;
    bool WaitAndPopWork(
        RHIExecutorWork&         _work,
        std::stop_token          _stop_token,
        RHIThreadHeartbeatScope& _heartbeat
    );
    static ERHIExecutorWorkAction WaitForPrerequisites(
        std::span<const RHIRecordingGateRef> _prerequisites,
        std::stop_token                      _stop_token,
        RHIThreadHeartbeatScope&             _heartbeat
    );
    void        CancelPendingWork();
    static void InvokeCallbackNoThrow(
        RHIExecutorWork&       _work,
        ERHIExecutorWorkAction _action
    ) noexcept;

    mutable std::mutex          mutex;
    std::condition_variable_any work_cv;
    std::condition_variable     idle_cv;
    std::deque<RHIExecutorWork> pending{};
    std::jthread                worker{};
    bool                        accepting{false};
    bool                        worker_busy{false};
};

} // namespace Moer::Render
