#include "rhi/RHIExecutorRecordingHandoff.h"

#include "log/LogSystem.h"

#include <cassert>
#include <exception>
#include <utility>

namespace Moer::Render {

std::shared_ptr<RHIRecordingGate> RHIRecordingGate::Create(bool _initially_complete) {
    // The constructor is deliberately private so a gate always has shared
    // ownership while it is registered with the handoff worker.
    return std::shared_ptr<RHIRecordingGate>(new RHIRecordingGate(_initially_complete));
}

bool RHIRecordingGate::Signal() noexcept {
    return Complete(ERHIRecordingStatus::Succeeded);
}

bool RHIRecordingGate::Fail() noexcept {
    return Complete(ERHIRecordingStatus::Failed);
}

bool RHIRecordingGate::Complete(ERHIRecordingStatus _status) noexcept {
    assert(_status != ERHIRecordingStatus::Pending);
    {
        std::lock_guard lock(mutex);
        if (status != ERHIRecordingStatus::Pending) {
            return false;
        }
        status = _status;
    }
    cv.notify_all();
    return true;
}

bool RHIRecordingGate::IsComplete() const noexcept {
    return Status() != ERHIRecordingStatus::Pending;
}

ERHIRecordingStatus RHIRecordingGate::Status() const noexcept {
    std::lock_guard lock(mutex);
    return status;
}

ERHIRecordingStatus RHIRecordingGate::Wait() const noexcept {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return status != ERHIRecordingStatus::Pending; });
    return status;
}

ERHIRecordingStatus RHIRecordingGate::Wait(std::stop_token _stop_token) const noexcept {
    std::unique_lock lock(mutex);
    const bool completed = cv.wait(lock, _stop_token, [this] {
        return status != ERHIRecordingStatus::Pending;
    });
    return completed ? status : ERHIRecordingStatus::Pending;
}

RHIExecutorRecordingHandoffQueue::~RHIExecutorRecordingHandoffQueue() {
    ShutDown();
}

void RHIExecutorRecordingHandoffQueue::Start() {
    std::lock_guard lock(mutex);
    if (accepting) {
        return;
    }
    assert(!worker.joinable());
    assert(pending.empty());
    assert(inline_dispatches == 0);
    assert(!worker_busy);
    accepting = true;
    worker    = std::jthread([this](std::stop_token _stop_token) { Run(_stop_token); });
}

void RHIExecutorRecordingHandoffQueue::EnqueueRecording(
    RHIExecutorRecordingHandoffWork&& _work
) {
    bool reject = false;
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            reject = true;
        } else {
            pending.emplace_back(std::move(_work));
        }
    }
    if (reject) {
        ResolveNoThrow(_work, ERHIRecordingHandoffResult::Cancel);
        return;
    }
    work_cv.notify_all();
}

void RHIExecutorRecordingHandoffQueue::RouteReady(
    RHIExecutorRecordingHandoffWork&& _work
) {
    bool run_inline = false;
    bool reject     = false;
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            reject = true;
        } else if (worker_busy || inline_dispatches != 0 || !pending.empty()) {
            pending.emplace_back(std::move(_work));
        } else {
            // Mark the direct operation active before releasing the lock. A
            // concurrent recording handoff is then queued behind it and the
            // worker waits for this count to return to zero.
            ++inline_dispatches;
            run_inline = true;
        }
    }

    if (reject) {
        ResolveNoThrow(_work, ERHIRecordingHandoffResult::Cancel);
        return;
    }
    if (!run_inline) {
        work_cv.notify_all();
        return;
    }

    ResolveNoThrow(_work, ERHIRecordingHandoffResult::Consume);
    {
        std::lock_guard lock(mutex);
        assert(inline_dispatches > 0);
        --inline_dispatches;
    }
    idle_cv.notify_all();
    work_cv.notify_all();
}

void RHIExecutorRecordingHandoffQueue::ShutDown() {
    {
        std::lock_guard lock(mutex);
        accepting = false;
    }

    if (worker.joinable()) {
        worker.request_stop();
        work_cv.notify_all();
        worker.join();
    }

    std::unique_lock lock(mutex);
    idle_cv.wait(lock, [this] {
        return inline_dispatches == 0 && !worker_busy && pending.empty();
    });
}

bool RHIExecutorRecordingHandoffQueue::IsIdle() const noexcept {
    std::lock_guard lock(mutex);
    return pending.empty() && inline_dispatches == 0 && !worker_busy;
}

void RHIExecutorRecordingHandoffQueue::Run(std::stop_token _stop_token) noexcept {
    for (;;) {
        RHIExecutorRecordingHandoffWork work{};
        {
            std::unique_lock lock(mutex);
            work_cv.wait(lock, _stop_token, [this] {
                return inline_dispatches == 0 && !pending.empty();
            });
            if (_stop_token.stop_requested()) {
                break;
            }
            work = std::move(pending.front());
            pending.pop_front();
            worker_busy = true;
        }

        bool ready = true;
        for (const RHIRecordingGateRef& prerequisite : work.prerequisites) {
            if (!prerequisite) {
                ready = false;
                continue;
            }
            const ERHIRecordingStatus status = prerequisite->Wait(_stop_token);
            if (status != ERHIRecordingStatus::Succeeded) {
                ready = false;
            }
            // A producer failure does not make the remaining CommandLists
            // safe to inspect. Keep waiting until every gate is terminal. A
            // lifecycle cancellation is different: stop must remain bounded.
            if (_stop_token.stop_requested()) {
                break;
            }
        }
        const bool cancelled = _stop_token.stop_requested();

        ResolveNoThrow(
            work,
            cancelled ? ERHIRecordingHandoffResult::Cancel :
            ready     ? ERHIRecordingHandoffResult::Consume :
                        ERHIRecordingHandoffResult::Reject
        );

        {
            std::lock_guard lock(mutex);
            worker_busy = false;
        }
        idle_cv.notify_all();

        if (cancelled) {
            break;
        }
    }

    std::deque<RHIExecutorRecordingHandoffWork> rejected{};
    {
        std::lock_guard lock(mutex);
        rejected.swap(pending);
        worker_busy = false;
    }
    idle_cv.notify_all();

    for (RHIExecutorRecordingHandoffWork& work : rejected) {
        ResolveNoThrow(work, ERHIRecordingHandoffResult::Cancel);
    }
}

void RHIExecutorRecordingHandoffQueue::ResolveNoThrow(
    RHIExecutorRecordingHandoffWork& _work,
    ERHIRecordingHandoffResult       _result
) noexcept {
    if (!_work.resolve) {
        return;
    }
    try {
        _work.resolve(_result);
    } catch (const std::exception& exception) {
        LOG_ERROR("[RHIExecutor] recording handoff resolver threw: {}", exception.what());
    } catch (...) {
        LOG_ERROR("[RHIExecutor] recording handoff resolver threw");
    }
}

} // namespace Moer::Render
