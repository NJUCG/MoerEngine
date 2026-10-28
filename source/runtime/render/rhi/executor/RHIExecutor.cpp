#include "rhi/executor/RHIExecutorInternal.h"

#include "log/LogSystem.h"
#include "misc/RAII.h"
#include "rhi/RHI.h"
#include "rhi/RHIThreadOwnership.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Moer::Render {
using namespace RHIExecutorDetail;
namespace {

bool RejectOwnedThreadBlockingCall(std::string_view _operation) {
    if (IsRHIBlockingCallAllowedOnCurrentThread()) {
        return false;
    }
    const ERHIThreadRole role = GetCurrentRHIThreadRole();
    LOG_ERROR(
        "[RHIExecutor] rejected blocking {} from {} owner thread to prevent a self-join",
        _operation,
        RHIThreadRoleName(role)
    );
    if (role == ERHIThreadRole::RecordWorker) {
        throw std::logic_error(
            "blocking RHI lifecycle call is forbidden from a record worker"
        );
    }
    return true;
}

bool BatchHasWork(const RHISubmissionBatch& _batch) {
    return !_batch.submits.empty() || _batch.present.has_value();
}

struct SubmissionRequestPayload {
    Array<RHISubmissionRequest>      submits{};
    ERHIExecSubmitFlags              flags{ERHIExecSubmitFlags::FlushGPU};
    std::optional<RHIPresentRequest> present{};

    RHIPresentRequest* GetPresent() noexcept {
        return present ? &*present : nullptr;
    }
};

class ExecutorSyncCompletion final {
public:
    void Signal(std::exception_ptr _failure = {}) noexcept {
        {
            std::lock_guard lock(mutex);
            failure  = std::move(_failure);
            complete = true;
        }
        cv.notify_all();
    }

    void Wait() {
        std::exception_ptr wait_failure{};
        std::unique_lock   lock(mutex);
        cv.wait(lock, [this] {
            return complete;
        });
        wait_failure = failure;
        lock.unlock();
        if (wait_failure) {
            std::rethrow_exception(wait_failure);
        }
    }

private:
    std::mutex              mutex;
    std::condition_variable cv;
    std::exception_ptr      failure{};
    bool                    complete{false};
};

RHISubmissionTopologyPlan BuildBatchTopology(const RHISubmissionBatch& _batch) {
    Array<RHISourceSubmitDescription> descriptions{};
    descriptions.reserve(_batch.submits.size());
    for (const RHISubmissionRequest& entry : _batch.submits) {
        descriptions.emplace_back(RHISourceSubmitDescription{
            .root_queue       = entry.queue,
            .command_count    = entry.submit.cmds.size(),
            .segments         = entry.submit.segments,
            .execution_class  = entry.submit.translate_execution_class,
            .has_side_effects = SubmitHasWork(entry.submit),
        });
    }
    return BuildRHISubmissionTopology(descriptions, _batch.sequence);
}

} // namespace

void RHIExecutorDetail::ReportExecutorFailure(
    std::string_view          _operation,
    std::string_view          _stage,
    const std::exception_ptr& _failure
) noexcept {
    try {
        std::rethrow_exception(_failure);
    } catch (const std::exception& exception) {
        try {
            LOG_ERROR("[RHIExecutor] {} failed during {}: {}", _stage, _operation, exception.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor] {} failed during {}", _stage, _operation);
        } catch (...) {
        }
    }
}

// Owns one detached batch until the backend takes responsibility. On failure
// the batch retains its unconsumed suffix; rejection runs only after releasing locks.
struct RHIExecutor::PendingBatchPublication {
    struct Failure {
        std::string_view   stage;
        std::exception_ptr exception;
    };

    std::shared_ptr<RHIBackendExecutor> backend{};
    RHISubmissionBatch                  batch{};
    std::optional<Failure>              failure{};

    // Caller holds dispatch_mutex, but must have released submit_mutex.
    void Publish() noexcept {
        if (failure || !BatchHasWork(batch)) {
            return;
        }
        try {
            batch.topology = BuildBatchTopology(batch);
            backend->Enqueue(std::move(batch));
        } catch (...) {
            // Enqueue preserves the not-yet-consumed suffix in batch on failure.
            failure.emplace(Failure{"backend publication", std::current_exception()});
        }
    }

    void Flush() {
        if (!failure && backend) {
            backend->Flush();
        }
    }

    // May invoke user callbacks. Call only outside both executor locks.
    bool RejectIfFailed(std::string_view _operation) noexcept {
        if (!failure) {
            return false;
        }
        ReportExecutorFailure(_operation, failure->stage, failure->exception);
        FinalizeRejectedBackendBatch(std::move(batch), failure->stage);
        return true;
    }
};

RHIExecutor& RHIExecutor::Get() {
    static RHIExecutor executor;
    return executor;
}

void RHIExecutor::StartUp() {
    StartUp(RHISubmissionPipelinePolicy::DefaultBatchWindow);
}

void RHIExecutor::StartUp(uint32 _submission_batch_window) {
    if (RejectOwnedThreadBlockingCall("StartUp")) {
        return;
    }
    RHIExecutor&     executor = Get();
    std::unique_lock submit_lock(executor.submit_mutex);
    executor.lifecycle_cv.wait(submit_lock, [&executor] {
        return executor.lifecycle_state != ELifecycleState::Stopping;
    });
    if (executor.lifecycle_state == ELifecycleState::Running) {
        LOG_WARNING("[RHIExecutor] duplicate StartUp ignored");
        return;
    }
    assert(!executor.backend_executor && "RHIExecutor backend survived device shutdown");
    assert(executor.pending_submits.empty() && !executor.pending_present.has_value());
    assert(executor.active_sync_calls == 0 && "RHIExecutor sync survived device shutdown");
    executor.next_batch_sequence = 1;
    executor.submission_batch_window =
        RHISubmissionPipelinePolicy::ClampBatchWindow(
            _submission_batch_window
        );
    executor.work_queue.Start();
    executor.lifecycle_state = ELifecycleState::Running;
}

std::shared_ptr<RHIBackendExecutor> RHIExecutor::GetOrCreateBackendExecutorLocked() {
    if (!backend_executor) {
        std::shared_ptr<RHIBackendExecutor> created =
            CreateBackendExecutor(submission_batch_window);
        if (!created) {
            throw std::runtime_error("RHI backend factory returned no executor");
        }
        backend_executor = std::move(created);
    }
    return backend_executor;
}

RHISubmissionBatch RHIExecutor::TakePendingSubmissionBatchLocked() {
    RHISubmissionBatch batch{};
    batch.submits = std::move(pending_submits);
    batch.present = std::move(pending_present);
    pending_submits.clear();
    pending_present.reset();
    if (BatchHasWork(batch)) {
        batch.sequence = next_batch_sequence++;
    }
    return batch;
}

RHIExecutor::PendingBatchPublication RHIExecutor::PreparePendingBatchLocked() {
    PendingBatchPublication publication{};
    if (pending_submits.empty() && !pending_present) {
        publication.backend = backend_executor;
        return publication;
    }
    try {
        // Create the backend before transferring executor-owned requests.
        publication.backend = GetOrCreateBackendExecutorLocked();
    } catch (...) {
        publication.failure.emplace(PendingBatchPublication::Failure{"backend creation", std::current_exception()});
    }
    // Both success and failure transfer one complete batch to the caller.
    publication.batch = TakePendingSubmissionBatchLocked();
    return publication;
}

void RHIExecutor::Submit(
    EQueueType          _queue,
    CmdSubmit&&         _submit,
    ERHIExecSubmitFlags _flags,
    RHIPresentRequest*  _present
) {
    Array<RHISubmissionRequest> submits;
    submits.emplace_back(_queue, std::move(_submit));
    Submit(std::move(submits), _flags, _present);
}

void RHIExecutor::Submit(
    Array<RHISubmissionRequest>&& _submits,
    ERHIExecSubmitFlags           _flags,
    RHIPresentRequest*            _present
) {
    const bool present_valid = _present == nullptr ||
                               (_present->swapchain && _present->source.texture != nullptr);
    const bool queues_valid  = std::all_of(_submits.begin(), _submits.end(), [](const RHISubmissionRequest& entry) {
        return IsSubmissionQueue(entry.queue);
    });
    if (!present_valid || !queues_valid) {
        FinalizeRejectedSubmissions(
            std::move(_submits),
            present_valid ? "invalid queue" : "invalid Present request",
            _present
        );
        return;
    }

    auto payload     = std::make_shared<SubmissionRequestPayload>();
    payload->submits = std::move(_submits);
    payload->flags   = _flags;
    if (_present != nullptr) {
        payload->present.emplace(std::move(*_present));
    }

    try {
        RHIExecutorWork work{
            .prerequisites = {},
            .callback      = [this, payload](ERHIExecutorWorkAction _action) {
                RHIPresentRequest* present = payload->GetPresent();
                if (_action != ERHIExecutorWorkAction::Execute) {
                    FinalizeRejectedSubmissions(
                        std::move(payload->submits),
                        "executor work queue is stopped",
                        present
                    );
                    return;
                }
                ProcessSubmissionRequests(std::move(payload->submits), payload->flags, present);
            },
        };
        work_queue.Enqueue(std::move(work));
    } catch (...) {
        RHIPresentRequest* present = payload->GetPresent();
        FinalizeRejectedSubmissions(
            std::move(payload->submits),
            "executor work queue admission failed",
            present
        );
    }
}

void RHIExecutor::AppendPendingRequestsLocked(
    Array<RHISubmissionRequest>& _submits,
    RHIPresentRequest*           _present
) {
    // Allocate before moving any request. The remaining moves
    // cannot throw, so rejection never sees a half-staged group.
    static_assert(std::is_nothrow_move_constructible_v<RHISubmissionRequest>);
    static_assert(std::is_nothrow_move_constructible_v<RHIPresentRequest>);
    pending_submits.reserve(pending_submits.size() + _submits.size());
    for (RHISubmissionRequest& entry : _submits) {
        pending_submits.emplace_back(std::move(entry));
    }
    if (_present) {
        pending_present.emplace(std::move(*_present));
    }
}

void RHIExecutor::ProcessSubmissionRequests(
    Array<RHISubmissionRequest>&& _submits,
    ERHIExecSubmitFlags           _flags,
    RHIPresentRequest*            _present
) {
    const bool legacy_multi_segment_source = RenderDevice::Get().GetRHIType() != ERHIType::Vulkan &&
                                             std::any_of(_submits.begin(), _submits.end(), [](const RHISubmissionRequest& entry) {
                                                 return entry.submit.segments.size() > 1;
                                             });
    if (legacy_multi_segment_source) {
        FinalizeRejectedSubmissions(std::move(_submits), "legacy backend does not support multi-segment sources", _present);
        return;
    }

    // Reject a stopped Present before waiting for the publication gate.
    if (_present) {
        bool running = false;
        {
            std::lock_guard lock(submit_mutex);
            running = lifecycle_state == ELifecycleState::Running;
        }
        if (!running) {
            FinalizeRejectedSubmissions(std::move(_submits), "executor is stopping or stopped", _present);
            return;
        }
    }

    const bool              flush_gpu = HasRHIExecSubmitFlag(_flags, ERHIExecSubmitFlags::FlushGPU);
    PendingBatchPublication publication{};
    std::exception_ptr      staging_failure{};
    bool                    rejected = false;
    {
        // A Present ends the batch. Keep its admission and publication under
        // one dispatch gate so Flush/Sync/Shutdown cannot overtake it.
        std::unique_lock dispatch_lock(dispatch_mutex, std::defer_lock);
        if (_present) {
            dispatch_lock.lock();
        }
        {
            std::lock_guard lock(submit_mutex);
            rejected = lifecycle_state != ELifecycleState::Running || pending_present.has_value();
            if (!rejected) {
                try {
                    AppendPendingRequestsLocked(_submits, _present);
                } catch (...) {
                    staging_failure = std::current_exception();
                }
                if (!staging_failure && _present) {
                    publication = PreparePendingBatchLocked();
                }
            }
        }
        if (_present) {
            publication.Publish();
            if (flush_gpu) {
                publication.Flush();
            }
        }
    }

    if (rejected) {
        FinalizeRejectedSubmissions(std::move(_submits), "executor stopped or has an unpublished Present", _present);
        return;
    }
    if (staging_failure) {
        ReportExecutorFailure("Submit", "request staging", staging_failure);
        FinalizeRejectedSubmissions(std::move(_submits), "allocation failed while staging submission", _present);
        return;
    }
    if (publication.RejectIfFailed("Present")) {
        return;
    }
    if (!_present && flush_gpu) {
        ProcessFlushRequest();
    }
}

void RHIExecutor::Present(RHIPresentRequest&& _present, bool _flush) {
    Array<RHISubmissionRequest> submits{};
    Submit(
        std::move(submits),
        _flush ? ERHIExecSubmitFlags::FlushGPU : ERHIExecSubmitFlags::None,
        &_present
    );
}

void RHIExecutor::Flush() {
    work_queue.Enqueue(RHIExecutorWork{
        .prerequisites = {},
        .callback      = [this](ERHIExecutorWorkAction _action) {
            if (_action == ERHIExecutorWorkAction::Execute) {
                ProcessFlushRequest();
            }
        },
    });
}

void RHIExecutor::ProcessFlushRequest() {
    {
        std::lock_guard lock(submit_mutex);
        if (lifecycle_state != ELifecycleState::Running) {
            return;
        }
    }

    PendingBatchPublication publication{};
    {
        std::lock_guard dispatch_lock(dispatch_mutex);
        {
            std::lock_guard lock(submit_mutex);
            publication = PreparePendingBatchLocked();
        }
        publication.Publish();
        publication.Flush();
    }
    publication.RejectIfFailed("Flush");
}

void RHIExecutor::Sync(ERHISyncDepth _depth) {
    if (RejectOwnedThreadBlockingCall("Sync")) {
        throw std::logic_error("Sync cannot block on an RHI owner thread");
    }
    auto completion = std::make_shared<ExecutorSyncCompletion>();
    work_queue.Enqueue(RHIExecutorWork{
        .prerequisites = {},
        .callback      = [this, _depth, completion](ERHIExecutorWorkAction _action) {
            try {
                if (_action == ERHIExecutorWorkAction::Execute) {
                    ProcessSyncRequest(_depth);
                } else {
                    throw std::runtime_error("Sync cancelled before backend publication");
                }
                completion->Signal();
            } catch (...) {
                completion->Signal(std::current_exception());
            }
        },
    });
    completion->Wait();
}

void RHIExecutor::ProcessSyncRequest(ERHISyncDepth _depth) {
    PendingBatchPublication publication{};
    bool                    sync_registered   = false;
    auto                    finish_sync_guard = OnScopeExit([this, &publication, &sync_registered] {
        // Release unconsumed requests and the old backend before allowing
        // Shutdown/StartUp to proceed.
        publication.batch = {};
        publication.backend.reset();
        if (sync_registered) {
            {
                std::lock_guard lock(submit_mutex);
                assert(active_sync_calls > 0);
                --active_sync_calls;
            }
            lifecycle_cv.notify_all();
        }
    });
    {
        std::lock_guard dispatch_lock(dispatch_mutex);
        {
            std::lock_guard lock(submit_mutex);
            if (lifecycle_state != ELifecycleState::Running) {
                throw std::runtime_error("Sync rejected while RHIExecutor is not running");
            }
            publication = PreparePendingBatchLocked();
            if (publication.backend) {
                ++active_sync_calls;
                sync_registered = true;
            }
        }
        publication.Publish();
        publication.Flush();
    }
    if (publication.RejectIfFailed("Sync")) {
        std::rethrow_exception(publication.failure->exception);
    }

    if (publication.backend) {
        publication.backend->Sync(_depth);
    }
}

std::optional<uint64> RHIExecutor::BeginShutdown(
    std::shared_ptr<RHIBackendExecutor>& _backend_to_cancel
) {
    std::unique_lock lock(submit_mutex);
    if (lifecycle_state == ELifecycleState::Stopped) {
        return std::nullopt;
    }
    if (lifecycle_state == ELifecycleState::Stopping) {
        const uint64 generation = shutdown_generation;
        lifecycle_cv.wait(lock, [this, generation] {
            return completed_shutdown_generation >= generation;
        });
        return std::nullopt;
    }
    lifecycle_state = ELifecycleState::Stopping;
    ++shutdown_generation;
    _backend_to_cancel = backend_executor;
    return shutdown_generation;
}

void RHIExecutor::FinishShutdown(uint64 _generation) {
    {
        std::lock_guard lock(submit_mutex);
        completed_shutdown_generation = std::max(completed_shutdown_generation, _generation);
        lifecycle_state               = ELifecycleState::Stopped;
    }
    lifecycle_cv.notify_all();
}

void RHIExecutor::ShutDown() {
    if (RejectOwnedThreadBlockingCall("ShutDown")) {
        return;
    }
    RHIExecutor&                        executor = Get();
    std::shared_ptr<RHIBackendExecutor> backend_to_cancel{};
    PendingBatchPublication             publication{};

    const std::optional<uint64> shutdown_generation = executor.BeginShutdown(backend_to_cancel);
    if (!shutdown_generation) {
        return;
    }
    const uint64 shutdown_to_complete = *shutdown_generation;

    // Install lifecycle completion before any operation that can allocate or
    // otherwise throw. In particular, lazy backend construction must never
    // strand concurrent Shutdown/StartUp callers behind a permanent Stopping
    // state.
    auto finish_shutdown =
        [&executor, shutdown_to_complete, &publication, &backend_to_cancel] {
            // StartUp may begin as soon as Stopped is published. Release the
            // remaining batch before dropping the old backend's queue leases.
            publication.batch = {};
            publication.backend.reset();
            backend_to_cancel.reset();
            executor.FinishShutdown(shutdown_to_complete);
        };
    auto finish_shutdown_guard = OnScopeExit(std::move(finish_shutdown));

    // Publish backend cancellation before joining the Executor worker. That
    // owner may currently be inside ProcessSyncRequest waiting behind a Submission
    // dependency which can no longer acquire a producer during shutdown.
    if (backend_to_cancel) {
        backend_to_cancel->BeginShutdown();
    }

    // Stop accepting work before taking the final pending batch. The
    // worker's stop token interrupts an unfinished recording gate, rejects it
    // and every later FIFO operation, and joins without depending on producer
    // progress. Any callback already inside ProcessSubmissionRequests/ProcessSyncRequest is allowed
    // to finish before backend ownership is detached below.
    executor.work_queue.ShutDown();

    {
        std::lock_guard dispatch_lock(executor.dispatch_mutex);
        {
            std::lock_guard lock(executor.submit_mutex);
            publication         = executor.PreparePendingBatchLocked();
            publication.backend = std::move(executor.backend_executor);
        }
        publication.Publish();
        publication.Flush();
    }
    publication.RejectIfFailed("ShutDown");

    // Cancel backend host waits before waiting for concurrent Sync calls.
    // Otherwise a Sync already queued behind an unpublished dependency would
    // wait for backend ShutDown, while ShutDown waited for that same Sync.
    if (publication.backend) {
        publication.backend->BeginShutdown();
    }

    // Backend shutdown may wait for its worker and GPU queues. The dispatch
    // gate is deliberately released first; Stopping keeps later publishers
    // out while concurrent Shutdown/Sync callers wait on the CV.
    {
        std::unique_lock lock(executor.submit_mutex);
        executor.lifecycle_cv.wait(lock, [&executor] {
            return executor.active_sync_calls == 0;
        });
    }
    if (publication.backend) {
        publication.backend->ShutDown();
    }
}

} // namespace Moer::Render
