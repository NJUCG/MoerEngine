#include "rhi/executor/RHIExecutorWorkQueue.h"

#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHIThreadHeartbeat.h"
#include "rhi/RHIThreadOwnership.h"

#include <cassert>
#include <exception>
#include <utility>

namespace Moer::Render {

std::shared_ptr<RHIRecordingGate> RHIRecordingGate::Create(bool _initially_complete) {
    // The constructor is deliberately private so a gate always has shared
    // ownership while it is registered with the Executor worker.
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

RHIExecutorWorkQueue::~RHIExecutorWorkQueue() {
    ShutDown();
}

void RHIExecutorWorkQueue::Start() {
    std::lock_guard lock(mutex);
    if (accepting) {
        return;
    }
    assert(!worker.joinable());
    assert(pending.empty());
    assert(!worker_busy);
    // Publish admission only after the worker has been created successfully.
    // std::jthread construction can throw (for example when the OS cannot
    // allocate another thread); in that case the queue must remain stopped so
    // callers cannot enqueue work that has no owner to drain it.
    worker = std::jthread([this](std::stop_token _stop_token) { Run(_stop_token); });
    accepting = true;
}

void RHIExecutorWorkQueue::Enqueue(
    RHIExecutorWork&& _work
) {
    bool cancel = false;
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            cancel = true;
        } else {
            pending.emplace_back(std::move(_work));
        }
    }
    if (cancel) {
        InvokeCallbackNoThrow(_work, ERHIExecutorWorkAction::Cancel);
        return;
    }
    work_cv.notify_all();
}

void RHIExecutorWorkQueue::ShutDown() {
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
    idle_cv.wait(lock, [this] { return !worker_busy && pending.empty(); });
}

bool RHIExecutorWorkQueue::IsIdle() const noexcept {
    std::lock_guard lock(mutex);
    return pending.empty() && !worker_busy;
}

void RHIExecutorWorkQueue::Run(std::stop_token _stop_token) noexcept {
    Platform::SetCurrentThreadName("Moer RHI Executor");
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Executor);
    RHIThreadHeartbeatScope heartbeat(
        ERHIThreadRole::Executor,
        ERHIHeartbeatDomain::General,
        ERHIHeartbeatStage::Dispatch
    );

    for (;;) {
        RHIExecutorWork work{};
        {
            heartbeat.Pulse(ERHIHeartbeatStage::Dispatch);
            std::unique_lock lock(mutex);
            RHIThreadHeartbeatWaitLock wait_lock(
                lock, heartbeat, ERHIHeartbeatStage::Dispatch
            );
            work_cv.wait(wait_lock, _stop_token, [this] { return !pending.empty(); });
            if (pending.empty()) {
                assert(_stop_token.stop_requested());
                heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
                break;
            }
            work = std::move(pending.front());
            pending.pop_front();
            worker_busy = true;
        }

        bool ready = true;
        bool cancelled = false;
        heartbeat.Pulse(ERHIHeartbeatStage::Dispatch);
        for (const RHIRecordingGateRef& prerequisite : work.prerequisites) {
            if (!prerequisite) {
                ready = false;
                continue;
            }
            ERHIRecordingStatus status = prerequisite->Status();
            if (status == ERHIRecordingStatus::Pending) {
                heartbeat.Pulse(ERHIHeartbeatStage::WaitForDependency);
                status = prerequisite->Wait(_stop_token);
                heartbeat.Pulse(ERHIHeartbeatStage::Dispatch);
            }
            if (status != ERHIRecordingStatus::Succeeded) {
                ready = false;
            }
            // A producer failure does not make the remaining CommandLists
            // safe to inspect. Keep waiting until every gate is terminal. A
            // lifecycle cancellation is different: stop must remain bounded.
            if (status == ERHIRecordingStatus::Pending) {
                cancelled = true;
                break;
            }
        }

        InvokeCallbackNoThrow(
            work,
            cancelled ? ERHIExecutorWorkAction::Cancel :
            ready     ? ERHIExecutorWorkAction::Execute :
                        ERHIExecutorWorkAction::Reject
        );

        {
            std::lock_guard lock(mutex);
            worker_busy = false;
        }
        idle_cv.notify_all();

        if (cancelled) {
            heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
            break;
        }
    }

    std::deque<RHIExecutorWork> cancelled_work{};
    {
        std::lock_guard lock(mutex);
        cancelled_work.swap(pending);
        worker_busy = false;
    }
    idle_cv.notify_all();

    for (RHIExecutorWork& work : cancelled_work) {
        InvokeCallbackNoThrow(work, ERHIExecutorWorkAction::Cancel);
    }
    heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
}

void RHIExecutorWorkQueue::InvokeCallbackNoThrow(
    RHIExecutorWork&       _work,
    ERHIExecutorWorkAction _action
) noexcept {
    if (!_work.callback) {
        return;
    }
    try {
        _work.callback(_action);
    } catch (const std::exception& exception) {
        try {
            LOG_ERROR(
                "[RHIExecutor] work callback threw: {}",
                exception.what()
            );
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor] work callback threw");
        } catch (...) {
        }
    }
}

} // namespace Moer::Render
