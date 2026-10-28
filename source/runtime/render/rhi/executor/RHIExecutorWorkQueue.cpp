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
    cv.wait(lock, [this] {
        return status != ERHIRecordingStatus::Pending;
    });
    return status;
}

ERHIRecordingStatus RHIRecordingGate::Wait(std::stop_token _stop_token) const noexcept {
    std::unique_lock lock(mutex);
    const bool       completed = cv.wait(lock, _stop_token, [this] {
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
    worker    = std::jthread([this](std::stop_token _stop_token) {
        Run(_stop_token);
    });
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
    idle_cv.wait(lock, [this] {
        return !worker_busy && pending.empty();
    });
}

bool RHIExecutorWorkQueue::IsIdle() const noexcept {
    std::lock_guard lock(mutex);
    return pending.empty() && !worker_busy;
}

void RHIExecutorWorkQueue::Run(std::stop_token _stop_token) noexcept {
    Platform::SetCurrentThreadName("Moer RHI Executor");
    RHIThreadRoleScope      owner_scope(ERHIThreadRole::Executor);
    RHIThreadHeartbeatScope heartbeat(
        ERHIThreadRole::Executor,
        ERHIHeartbeatDomain::General,
        ERHIHeartbeatStage::Dispatch
    );

    while (true) {
        RHIExecutorWork work{};
        if (!WaitAndPopWork(work, _stop_token, heartbeat)) {
            break;
        }

        const ERHIExecutorWorkAction action =
            WaitForPrerequisites(work.prerequisites, _stop_token, heartbeat);
        InvokeCallbackNoThrow(work, action);

        {
            std::lock_guard lock(mutex);
            worker_busy = false;
        }
        idle_cv.notify_all();

        if (action == ERHIExecutorWorkAction::Cancel) {
            heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
            break;
        }
    }

    CancelPendingWork();
    heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
}

bool RHIExecutorWorkQueue::WaitAndPopWork(
    RHIExecutorWork&         _work,
    std::stop_token          _stop_token,
    RHIThreadHeartbeatScope& _heartbeat
) {
    _heartbeat.Pulse(ERHIHeartbeatStage::Dispatch);
    std::unique_lock           lock(mutex);
    RHIThreadHeartbeatWaitLock wait_lock(lock, _heartbeat, ERHIHeartbeatStage::Dispatch);
    work_cv.wait(wait_lock, _stop_token, [this] {
        return !pending.empty();
    });
    if (pending.empty()) {
        assert(_stop_token.stop_requested());
        _heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
        return false;
    }
    _work = std::move(pending.front());
    pending.pop_front();
    worker_busy = true;
    return true;
}

ERHIExecutorWorkAction RHIExecutorWorkQueue::WaitForPrerequisites(
    std::span<const RHIRecordingGateRef> _prerequisites,
    std::stop_token                      _stop_token,
    RHIThreadHeartbeatScope&             _heartbeat
) {
    ERHIExecutorWorkAction action = ERHIExecutorWorkAction::Execute;
    _heartbeat.Pulse(ERHIHeartbeatStage::Dispatch);
    for (const RHIRecordingGateRef& prerequisite : _prerequisites) {
        if (!prerequisite) {
            action = ERHIExecutorWorkAction::Reject;
            continue;
        }
        ERHIRecordingStatus status = prerequisite->Status();
        if (status == ERHIRecordingStatus::Pending) {
            _heartbeat.Pulse(ERHIHeartbeatStage::WaitForDependency);
            status = prerequisite->Wait(_stop_token);
            _heartbeat.Pulse(ERHIHeartbeatStage::Dispatch);
        }
        if (status == ERHIRecordingStatus::Pending) {
            return ERHIExecutorWorkAction::Cancel;
        }
        // A failed producer does not make its siblings safe to inspect.
        // Wait for every producer unless shutdown interrupts the wait.
        if (status == ERHIRecordingStatus::Failed) {
            action = ERHIExecutorWorkAction::Reject;
        }
    }
    return action;
}

void RHIExecutorWorkQueue::CancelPendingWork() {
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
