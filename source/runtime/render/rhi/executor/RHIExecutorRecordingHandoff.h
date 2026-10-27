#pragma once

#include "RenderAPI.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace Moer::Render {

enum class ERHIRecordingStatus : uint8_t {
    Pending,
    Succeeded,
    Failed,
};

// One-shot CPU publication gate used to hand a CommandList to the RHI
// executor before its producer has finished recording it. Signal() publishes
// all writes performed by the recorder before the call; the handoff worker
// acquires them before consuming the CommandList.
class RENDER_API RHIRecordingGate final {
public:
    static std::shared_ptr<RHIRecordingGate> Create(bool _initially_complete = false);

    // Idempotent. Returns true only for the transition to the complete state.
    bool Signal() noexcept;
    bool Fail() noexcept;
    [[nodiscard]] bool IsComplete() const noexcept;
    [[nodiscard]] ERHIRecordingStatus Status() const noexcept;
    // Blocking producer-side join used at explicit RDG wave boundaries.
    // Unlike the worker overload, this wait is not cancellable and always
    // returns a terminal Succeeded/Failed status.
    [[nodiscard]] ERHIRecordingStatus Wait() const noexcept;

private:
    friend class RHIExecutorRecordingHandoffQueue;

    explicit RHIRecordingGate(bool _initially_complete) :
        status(
            _initially_complete ? ERHIRecordingStatus::Succeeded :
                                  ERHIRecordingStatus::Pending
        ) {}
    bool Complete(ERHIRecordingStatus _status) noexcept;
    [[nodiscard]] ERHIRecordingStatus Wait(std::stop_token _stop_token) const noexcept;

    mutable std::mutex              mutex;
    mutable std::condition_variable_any cv;
    ERHIRecordingStatus             status{ERHIRecordingStatus::Pending};
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

enum class ERHIRecordingHandoffResult : uint8_t {
    Consume,
    // Every prerequisite reached a terminal state, but at least one producer
    // failed. The resolver may safely inspect every protected CommandList for
    // frontend cleanup, but must never publish its commands to a GPU queue.
    Reject,
    // The queue stopped before all prerequisites became terminal. Protected
    // CommandLists remain opaque because a producer may still be mutating one.
    Cancel,
};

// A work item can depend on one gate per source CommandList. The worker waits
// for the complete set in input order and resolves the item exactly once.
struct RHIExecutorRecordingHandoffWork {
    std::vector<RHIRecordingGateRef> prerequisites{};
    std::function<void(ERHIRecordingHandoffResult)> resolve{};
};

// Small FIFO used by RHIExecutor to bridge asynchronously recorded sources to
// the existing backend submission path. Ready operations are routed through
// the same ordering domain while a recording handoff is active, so Submit,
// Present, Flush and Sync cannot overtake an unfinished recording source.
//
// A normal failed group waits for every source gate to become terminal before
// resolving Reject, so its resolver can safely drain frontend cleanup. ShutDown
// never waits for a gate: requesting stop interrupts the current gate wait and
// resolves that work plus every later FIFO entry as Cancel.
class RENDER_API RHIExecutorRecordingHandoffQueue final {
public:
    RHIExecutorRecordingHandoffQueue() = default;
    ~RHIExecutorRecordingHandoffQueue();

    RHIExecutorRecordingHandoffQueue(const RHIExecutorRecordingHandoffQueue&) = delete;
    RHIExecutorRecordingHandoffQueue& operator=(const RHIExecutorRecordingHandoffQueue&) = delete;

    void Start();

    // Establishes the asynchronous FIFO even when all prerequisites are
    // already complete. Used by SubmitRecording.
    void EnqueueRecording(RHIExecutorRecordingHandoffWork&& _work);

    // Always enters the Executor worker FIFO, including when no recording
    // prerequisite is active. This gives accepted Submit/Present/Flush/Sync
    // work one stable thread owner and prevents callback affinity from
    // depending on whether a recording handoff happened to be pending.
    void RouteReady(RHIExecutorRecordingHandoffWork&& _work);

    void ShutDown();
    [[nodiscard]] bool IsIdle() const noexcept;

private:
    void Run(std::stop_token _stop_token) noexcept;
    static void ResolveNoThrow(
        RHIExecutorRecordingHandoffWork& _work,
        ERHIRecordingHandoffResult       _result
    ) noexcept;

    mutable std::mutex                        mutex;
    std::condition_variable_any               work_cv;
    std::condition_variable                   idle_cv;
    std::deque<RHIExecutorRecordingHandoffWork> pending{};
    std::jthread                              worker{};
    bool                                      accepting{false};
    bool                                      worker_busy{false};
};

} // namespace Moer::Render
