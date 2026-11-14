#include "rhi/executor/RHIExecutorWorkQueue.h"

#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "profile/ProfileScope.h"
#include "rhi/RHIThreadOwnership.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <stdexcept>
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

std::optional<ERHIRecordingStatus> RHIRecordingGate::Wait(std::stop_token _stop_token) const noexcept {
    std::unique_lock lock(mutex);
    const bool       completed = cv.wait(lock, _stop_token, [this] {
        return status != ERHIRecordingStatus::Pending;
    });
    if (!completed) {
        return std::nullopt;
    }
    return status;
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
    if (std::any_of(_work.prerequisites.begin(), _work.prerequisites.end(), [](const RHIRecordingGateRef& gate) {
            return !gate;
        })) {
        throw std::invalid_argument("executor work contains a missing prerequisite gate");
    }
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
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Executor);

    while (true) {
        RHIExecutorWork work{};
        if (!WaitAndPopWork(work, _stop_token)) {
            break;
        }

        MOER_PROFILE_SCOPE("RHI.Executor.Work");
        const ERHIExecutorWorkAction action =
            WaitForPrerequisites(work.prerequisites, _stop_token);
        InvokeCallbackNoThrow(work, action);

        {
            std::lock_guard lock(mutex);
            worker_busy = false;
        }
        idle_cv.notify_all();

        if (action == ERHIExecutorWorkAction::Cancel) {
            break;
        }
    }

    CancelPendingWork();
}

bool RHIExecutorWorkQueue::WaitAndPopWork(
    RHIExecutorWork& _work,
    std::stop_token  _stop_token
) {
    std::unique_lock lock(mutex);
    work_cv.wait(lock, _stop_token, [this] {
        return !pending.empty();
    });
    if (pending.empty()) {
        assert(_stop_token.stop_requested());
        return false;
    }
    _work = std::move(pending.front());
    pending.pop_front();
    worker_busy = true;
    return true;
}

ERHIExecutorWorkAction RHIExecutorWorkQueue::WaitForPrerequisites(
    std::span<const RHIRecordingGateRef> _prerequisites,
    std::stop_token                      _stop_token
) {
    MOER_PROFILE_SCOPE("RHI.Executor.WaitForPrerequisites");
    ERHIExecutorWorkAction action = ERHIExecutorWorkAction::Execute;
    for (const RHIRecordingGateRef& prerequisite : _prerequisites) {
        assert(prerequisite);
        ERHIRecordingStatus status = prerequisite->Status();
        if (status == ERHIRecordingStatus::Pending) {
            const std::optional<ERHIRecordingStatus> result = prerequisite->Wait(_stop_token);
            if (!result) {
                return ERHIExecutorWorkAction::Cancel;
            }
            status = *result;
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
    MOER_PROFILE_SCOPE("RHI.Executor.CancelPendingWork");
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
    MOER_PROFILE_SCOPE("RHI.Executor.WorkCallback");
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
