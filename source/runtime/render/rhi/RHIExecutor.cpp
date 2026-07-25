#include "rhi/RHIExecutor.h"

#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHIThreadOwnership.h"
#include "vulkan/VulkanSubmissionExecutor.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Moer::Render {
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

template<typename F>
class ScopeExit {
public:
    explicit ScopeExit(F&& _callback) : callback(std::forward<F>(_callback)) {}
    ~ScopeExit() noexcept {
        callback();
    }

    ScopeExit(const ScopeExit&)            = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F callback;
};

GraphEventRef MakeCompletedBackendEvent() {
    GraphEventRef event = GraphEvent::CreateGraphEvent();
    event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return event;
}

bool SubmitHasWork(const CmdSubmit& _submit) {
    return !_submit.cmds.empty() || !_submit.callbacks.empty() ||
           !_submit.success_callbacks.empty() || !_submit.wait_events.empty() ||
           !_submit.signal_events.empty() || _submit.b_sync ||
           _submit.b_tick_profiling || _submit.b_delete_resources;
}

bool IsSubmissionQueue(EQueueType _queue) {
    return _queue == EQueueType::Graphics || _queue == EQueueType::Compute ||
           _queue == EQueueType::Copy;
}

bool BatchHasWork(const RHIBackendSubmissionBatch& _batch) {
    return !_batch.submits.empty() || _batch.present.has_value();
}

void ResolveRejectedPresent(RHIPresentRequest* _present) noexcept {
    if (_present == nullptr || !_present->receipt) {
        return;
    }
    try {
        _present->receipt->Resolve(false);
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor] rejected Present receipt resolver threw");
        } catch (...) {
        }
    }
}

// A frontend rejection can only happen for invalid input or after the executor
// has stopped accepting work.  Normal completion callbacks still own cleanup
// responsibilities, while success callbacks must never run for work that did
// not reach a GPU queue.  Keep this path backend-neutral and, importantly, run
// user code only after all executor locks have been released.
void FinalizeRejectedSubmissions(
    Array<RHIBackendSubmissionBatchEntry>&& _submits,
    std::string_view                         _reason
) noexcept {
    size_t callback_count       = 0;
    size_t success_callback_cnt = 0;
    size_t signal_count         = 0;
    for (const RHIBackendSubmissionBatchEntry& entry : _submits) {
        callback_count += entry.submit.callbacks.size();
        success_callback_cnt += entry.submit.success_callbacks.size();
        signal_count += entry.submit.signal_events.size();
    }

    try {
        LOG_ERROR(
            "[RHIExecutor] rejected {} frontend submit(s): {}; callbacks={} "
            "success_callbacks_skipped={} signal_events_rejected={}",
            _submits.size(),
            _reason,
            callback_count,
            success_callback_cnt,
            signal_count
        );
    } catch (...) {
    }

    // A rejected producer must publish a terminal failure before ordinary
    // callbacks run. Otherwise a later submission can wait forever for a
    // timeline value which this frontend path silently discarded.
    for (RHIBackendSubmissionBatchEntry& entry : _submits) {
        for (const SignalEvent& signal : entry.submit.signal_events) {
            auto* fence = reinterpret_cast<Fence*>(signal.timeline_handle);
            if (fence == nullptr) {
                continue;
            }
            try {
                fence->Reject(signal.value);
            } catch (const std::exception& exception) {
                try {
                    LOG_ERROR(
                        "[RHIExecutor] rejected-submit signal terminalization "
                        "threw: {}",
                        exception.what()
                    );
                } catch (...) {
                }
            } catch (...) {
                try {
                    LOG_ERROR(
                        "[RHIExecutor] rejected-submit signal terminalization threw"
                    );
                } catch (...) {
                }
            }
        }
    }

    for (RHIBackendSubmissionBatchEntry& entry : _submits) {
        for (std::function<void()>& callback : entry.submit.callbacks) {
            if (!callback) {
                continue;
            }
            try {
                callback();
            } catch (const std::exception& exception) {
                try {
                    LOG_ERROR(
                        "[RHIExecutor] rejected-submit completion callback threw: {}",
                        exception.what()
                    );
                } catch (...) {
                }
            } catch (...) {
                try {
                    LOG_ERROR("[RHIExecutor] rejected-submit completion callback threw");
                } catch (...) {
                }
            }
        }
    }
}

void RejectRecordingSignalMetadata(
    const Array<RHIRecordingSource>& _sources
) noexcept {
    // Metadata is prepared source-by-source and preparation can allocate.
    // If any step throws, the entire immutable recording transaction is
    // rejected, including producer values not yet copied into CmdSubmit.
    // Fence rejection is idempotent, so already-materialized values may be
    // terminalized again by FinalizeRejectedSubmissions below.
    for (const RHIRecordingSource& source : _sources) {
        for (const RHIRecordingFencePoint& point :
             source.submit_metadata.signal_fences) {
            if (!point.fence.IsValid() || point.value == 0) {
                continue;
            }
            try {
                point.fence->Reject(point.value);
            } catch (...) {
            }
        }
    }
}

void FinalizeRejectedBackendBatch(
    RHIBackendSubmissionBatch&& _batch,
    std::string_view             _reason
) noexcept {
    RHIPresentRequest* present =
        _batch.present.has_value() ? &*_batch.present : nullptr;
    ResolveRejectedPresent(present);
    FinalizeRejectedSubmissions(std::move(_batch.submits), _reason);
}

void ReportBackendCreationFailure(
    std::string_view          _operation,
    const std::exception_ptr& _failure
) noexcept {
    try {
        if (_failure) {
            std::rethrow_exception(_failure);
        }
    } catch (const std::exception& exception) {
        try {
            LOG_ERROR(
                "[RHIExecutor] backend creation failed during {}: {}",
                _operation,
                exception.what()
            );
        } catch (...) {
        }
        return;
    } catch (...) {
    }

    try {
        LOG_ERROR(
            "[RHIExecutor] backend creation failed during {} with an unknown exception",
            _operation
        );
    } catch (...) {
    }
}

void FinalizeCancelledRecordingSources(
    Array<RHIRecordingSource>&& _sources,
    std::string_view             _reason
) {
    RejectRecordingSignalMetadata(_sources);
    // A source protected by an incomplete recording gate must remain opaque:
    // even reading IsEmpty(), let alone draining callbacks, races its producer.
    // A terminal gate, however, is the ownership handoff point and its ordinary
    // callbacks still own CPU/resource cleanup even when shutdown cancels the
    // FIFO before the source can be materialized. Success-only callbacks must
    // never run because no GPU submission was published.
    size_t terminal_source_count = 0;
    size_t opaque_source_count   = 0;
    size_t cleanup_callback_count = 0;
    for (RHIRecordingSource& source : _sources) {
        const bool terminal =
            source.completion &&
            source.completion.Status() != ERHIRecordingStatus::Pending;
        if (!terminal) {
            ++opaque_source_count;
            continue;
        }

        ++terminal_source_count;
        if (!source.command_list) {
            continue;
        }
        try {
            auto source_callbacks =
                source.command_list->DrainOrdinaryCallbacksForRejection();
            cleanup_callback_count += source_callbacks.size();
            for (std::function<void()>& callback : source_callbacks) {
                if (!callback) {
                    continue;
                }
                try {
                    callback();
                } catch (const std::exception& exception) {
                    LOG_ERROR(
                        "[RHIExecutor] cancelled-recording cleanup callback threw: {}",
                        exception.what()
                    );
                } catch (...) {
                    LOG_ERROR("[RHIExecutor] cancelled-recording cleanup callback threw");
                }
            }
        } catch (const std::exception& exception) {
            LOG_ERROR(
                "[RHIExecutor] failed to drain cancelled recording source cleanup: {}",
                exception.what()
            );
        } catch (...) {
            LOG_ERROR("[RHIExecutor] failed to drain cancelled recording source cleanup");
        }
    }

    LOG_WARNING(
        "[RHIExecutor] rejected {} recording source(s): {}; "
        "terminal_sources={} opaque_sources={} cleanup_callbacks={}",
        _sources.size(),
        _reason,
        terminal_source_count,
        opaque_source_count,
        cleanup_callback_count
    );
    _sources.clear();
}

void FinalizeFailedRecordingSources(
    Array<RHIRecordingSource>&& _sources,
    std::string_view             _reason
) {
    RejectRecordingSignalMetadata(_sources);
    // Reject is emitted only after every producer gate is terminal. This is
    // the ownership point at which failed/partial CommandLists are immutable
    // and may be destructively drained for their ordinary cleanup callbacks.
    // DrainOrdinaryCallbacksForRejection produces no CmdSubmit, drops success
    // callbacks, and guarantees that no partial commands reach a backend.
    const bool all_terminal = std::all_of(
        _sources.begin(),
        _sources.end(),
        [](const RHIRecordingSource& _source) {
            return _source.completion &&
                   _source.completion.Status() != ERHIRecordingStatus::Pending;
        }
    );
    if (!all_terminal) {
        LOG_ERROR(
            "[RHIExecutor] failed recording group resolved before every producer became terminal"
        );
        FinalizeCancelledRecordingSources(
            std::move(_sources),
            "failed recording group still has a mutable producer"
        );
        return;
    }

    const size_t source_count = _sources.size();
    size_t       cleanup_callback_count = 0;
    for (RHIRecordingSource& source : _sources) {
        if (!source.command_list) {
            continue;
        }
        try {
            auto source_callbacks =
                source.command_list->DrainOrdinaryCallbacksForRejection();
            cleanup_callback_count += source_callbacks.size();
            for (std::function<void()>& callback : source_callbacks) {
                if (!callback) {
                    continue;
                }
                try {
                    callback();
                } catch (const std::exception& exception) {
                    LOG_ERROR(
                        "[RHIExecutor] rejected-recording cleanup callback threw: {}",
                        exception.what()
                    );
                } catch (...) {
                    LOG_ERROR("[RHIExecutor] rejected-recording cleanup callback threw");
                }
            }
        } catch (const std::exception& exception) {
            LOG_ERROR(
                "[RHIExecutor] failed to drain rejected recording source cleanup: {}",
                exception.what()
            );
        } catch (...) {
            LOG_ERROR("[RHIExecutor] failed to drain rejected recording source cleanup");
        }
    }
    _sources.clear();
    LOG_ERROR(
        "[RHIExecutor] rejected {} completed recording source(s): {}; cleanup_callbacks={}",
        source_count,
        _reason,
        cleanup_callback_count
    );
}

struct ReadySubmissionPayload {
    Array<RHIBackendSubmissionBatchEntry> submits{};
    ERHIExecSubmitFlags                    flags{ERHIExecSubmitFlags::FlushGPU};
    std::optional<RHIPresentRequest>       present{};
};

struct RecordingSubmissionPayload {
    Array<RHIRecordingSource> sources{};
    ERHIExecSubmitFlags       flags{ERHIExecSubmitFlags::FlushGPU};
};

class HandoffSyncCompletion final {
public:
    void Signal() noexcept {
        {
            std::lock_guard lock(mutex);
            complete = true;
        }
        cv.notify_all();
    }

    void Wait() noexcept {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return complete; });
    }

private:
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    complete{false};
};

// D3D12 has not adopted the Vulkan submission runtime yet. Keep its old queue
// ownership path isolated instead of weakening the Vulkan topology contract.
class LegacyQueueBackendExecutor final : public RHIBackendExecutor {
public:
    void Enqueue(RHIBackendSubmissionBatch&& _batch) override {
        std::optional<EQueueType> last_queue{};
        uint64                    ordered_copy_timeline = 0;
        {
            std::lock_guard lock(state_mutex);
            last_queue             = ordered_tail_queue;
            ordered_copy_timeline = ordered_tail_copy_timeline;
        }

        for (RHIBackendSubmissionBatchEntry& entry : _batch.submits) {
            if (!SubmitHasWork(entry.submit)) {
                continue;
            }
            if (last_queue.has_value() && *last_queue != entry.queue) {
                WaitForQueue(*last_queue, ordered_copy_timeline);
            }
            switch (entry.queue) {
                case EQueueType::Graphics:
                case EQueueType::Compute:
                    RenderDevice::Get().GetCommandQueue(entry.queue).Execute(std::move(entry.submit));
                    ordered_copy_timeline = 0;
                    break;
                case EQueueType::Copy: {
                    const IOWaitEvt completion =
                        RenderDevice::Get().GetCopyQueue().Execute(std::move(entry.submit));
                    ordered_copy_timeline = completion.timeline;
                    std::lock_guard lock(state_mutex);
                    last_copy_timeline = std::max(last_copy_timeline, completion.timeline);
                    break;
                }
                case EQueueType::Ignore:
                case EQueueType::Num:
                default:
                    LOG_ERROR(
                        "[RHIExecutor] batch {} rejected invalid queue {}",
                        _batch.sequence,
                        static_cast<uint32>(entry.queue)
                    );
                    assert(false && "RHI backend batch contains an invalid queue");
                    continue;
            }
            {
                std::lock_guard lock(state_mutex);
                used_queues[static_cast<size_t>(entry.queue)] = true;
            }
            last_queue = entry.queue;
        }

        if (_batch.present.has_value()) {
            if (last_queue.has_value() && *last_queue != EQueueType::Graphics) {
                WaitForQueue(*last_queue, ordered_copy_timeline);
            }
            RHIPresentRequest& present = *_batch.present;
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics)
                .Present(
                    std::move(present.swapchain),
                    present.source,
                    std::move(present.receipt)
                );
            last_queue             = EQueueType::Graphics;
            ordered_copy_timeline = 0;
            std::lock_guard lock(state_mutex);
            used_queues[static_cast<size_t>(EQueueType::Graphics)] = true;
            present_seen = true;
        }

        {
            std::lock_guard lock(state_mutex);
            ordered_tail_queue         = last_queue;
            ordered_tail_copy_timeline = ordered_copy_timeline;
        }
    }

    GraphEventRef Sync(ERHISyncDepth _depth) override {
        StaticArray<bool, static_cast<size_t>(EQueueType::Num)> queues{};
        uint64 copy_timeline = 0;
        {
            std::lock_guard lock(state_mutex);
            queues       = used_queues;
            copy_timeline = last_copy_timeline;
        }

        if (queues[static_cast<size_t>(EQueueType::Graphics)] ||
            _depth == ERHISyncDepth::Present) {
            RenderDevice::Get().GetCommandQueue(EQueueType::Graphics).Sync();
        }
        if (queues[static_cast<size_t>(EQueueType::Compute)]) {
            RenderDevice::Get().GetCommandQueue(EQueueType::Compute).Sync();
        }
        if (copy_timeline != 0) {
            RenderDevice::Get().GetCopyQueue().Sync(copy_timeline);
        }
        return MakeCompletedBackendEvent();
    }

    void Flush(ERHIFlushDepth) override {
        // Queue::Execute has already crossed into the existing RHI FIFO.
    }

    void ShutDown() override {
        GraphEventRef completion = Sync(ERHISyncDepth::Present);
        if (completion) {
            completion->Wait(EThread::UNKNOWN_THREAD);
        }
    }

private:
    static void WaitForQueue(EQueueType _queue, uint64 _copy_timeline) {
        switch (_queue) {
            case EQueueType::Graphics:
            case EQueueType::Compute:
                RenderDevice::Get().GetCommandQueue(_queue).Sync();
                return;
            case EQueueType::Copy:
                if (_copy_timeline != 0) {
                    RenderDevice::Get().GetCopyQueue().Sync(_copy_timeline);
                }
                return;
            case EQueueType::Ignore:
            case EQueueType::Num:
            default:
                assert(false && "Legacy RHI executor has an invalid ordered tail queue");
                return;
        }
    }

    std::mutex state_mutex;
    StaticArray<bool, static_cast<size_t>(EQueueType::Num)> used_queues{};
    uint64                    last_copy_timeline{0};
    bool                      present_seen{false};
    std::optional<EQueueType> ordered_tail_queue{};
    uint64                    ordered_tail_copy_timeline{0};
};

std::shared_ptr<RHIBackendExecutor> CreateBackendExecutor() {
    switch (RenderDevice::Get().GetRHIType()) {
        case ERHIType::Vulkan:
            return std::make_shared<VulkanSubmissionExecutor>();
        case ERHIType::D3D12:
            LOG_WARNING(
                "[RHIExecutor] D3D12 uses the legacy queue adapter; upper Vulkan topology is unavailable"
            );
            return std::make_shared<LegacyQueueBackendExecutor>();
    }
    LOG_ERROR("[RHIExecutor] unsupported RHI type");
    assert(false && "Unsupported RHI type");
    return {};
}

RHISubmissionTopologyPlan BuildBatchTopology(const RHIBackendSubmissionBatch& _batch) {
    Array<RHISourceSubmitDescription> descriptions{};
    descriptions.reserve(_batch.submits.size());
    for (const RHIBackendSubmissionBatchEntry& entry : _batch.submits) {
        descriptions.emplace_back(RHISourceSubmitDescription{
            .root_queue       = entry.queue,
            .command_count    = entry.submit.cmds.size(),
            .segments         = entry.submit.segments,
            .execution_class = entry.submit.translate_execution_class,
            .has_side_effects = SubmitHasWork(entry.submit),
        });
    }
    return BuildRHISubmissionTopology(descriptions, _batch.sequence);
}

} // namespace

RHIExecutor& RHIExecutor::Get() {
    static RHIExecutor executor;
    return executor;
}

void RHIExecutor::StartUp() {
    if (RejectOwnedThreadBlockingCall("StartUp")) {
        return;
    }
    RHIExecutor& executor = Get();
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
    executor.recording_handoff.Start();
    executor.lifecycle_state     = ELifecycleState::Running;
}

std::shared_ptr<RHIBackendExecutor> RHIExecutor::GetBackendExecutorLocked() {
    if (!backend_executor) {
        std::shared_ptr<RHIBackendExecutor> created = CreateBackendExecutor();
        if (!created) {
            throw std::runtime_error("RHI backend factory returned no executor");
        }
        backend_executor = std::move(created);
    }
    return backend_executor;
}

RHIBackendSubmissionBatch RHIExecutor::TakePendingBatchLocked() {
    RHIBackendSubmissionBatch batch{};
    batch.submits  = std::move(pending_submits);
    batch.present  = std::move(pending_present);
    pending_submits.clear();
    pending_present.reset();
    if (!batch.submits.empty() || batch.present.has_value()) {
        batch.sequence = next_batch_sequence++;
    }
    return batch;
}

void RHIExecutor::Submit(
    EQueueType          _queue,
    CmdSubmit&&         _submit,
    ERHIExecSubmitFlags _flags,
    RHIPresentRequest*  _present
) {
    Array<RHIBackendSubmissionBatchEntry> submits;
    submits.emplace_back(_queue, std::move(_submit));
    Submit(std::move(submits), _flags, _present);
}

void RHIExecutor::Submit(
    Array<RHIBackendSubmissionBatchEntry>&& _submits,
    ERHIExecSubmitFlags                      _flags,
    RHIPresentRequest*                       _present
) {
    const bool present_valid = _present == nullptr ||
                               (_present->swapchain && _present->source.texture != nullptr);
    const bool queues_valid = std::all_of(
        _submits.begin(),
        _submits.end(),
        [](const RHIBackendSubmissionBatchEntry& entry) {
            return IsSubmissionQueue(entry.queue);
        }
    );
    if (!present_valid || !queues_valid) {
        ResolveRejectedPresent(_present);
        FinalizeRejectedSubmissions(
            std::move(_submits),
            present_valid ? "invalid queue" : "invalid Present request"
        );
        return;
    }

    auto payload     = std::make_shared<ReadySubmissionPayload>();
    payload->submits = std::move(_submits);
    payload->flags   = _flags;
    if (_present != nullptr) {
        payload->present.emplace(std::move(*_present));
    }

    recording_handoff.RouteReady(RHIExecutorRecordingHandoffWork{
        .prerequisites = {},
        .resolve = [this, payload](ERHIRecordingHandoffResult _result) mutable {
            RHIPresentRequest* present = payload->present.has_value() ?
                                             &*payload->present :
                                             nullptr;
            if (_result != ERHIRecordingHandoffResult::Consume) {
                ResolveRejectedPresent(present);
                FinalizeRejectedSubmissions(
                    std::move(payload->submits),
                    "recording handoff is stopped"
                );
                return;
            }
            SubmitReady(std::move(payload->submits), payload->flags, present);
        },
    });
}

void RHIExecutor::SubmitReady(
    Array<RHIBackendSubmissionBatchEntry>&& _submits,
    ERHIExecSubmitFlags                      _flags,
    RHIPresentRequest*                       _present
) {
    const bool legacy_multi_segment_source =
        RenderDevice::Get().GetRHIType() != ERHIType::Vulkan &&
        std::any_of(
            _submits.begin(),
            _submits.end(),
            [](const RHIBackendSubmissionBatchEntry& entry) {
                return entry.submit.segments.size() > 1;
            }
        );
    if (legacy_multi_segment_source) {
        ResolveRejectedPresent(_present);
        FinalizeRejectedSubmissions(
            std::move(_submits),
            "legacy backend does not support multi-segment sources"
        );
        return;
    }

    // A Present is a batch terminator.  Publish it while holding the same
    // dispatch gate used by Flush/Sync/Shutdown so no later batch can overtake
    // it.  `_flags` controls the backend flush depth, not whether the sealed
    // batch becomes visible to the backend FIFO.
    if (_present != nullptr) {
        bool reject_before_dispatch = false;
        {
            std::lock_guard lock(submit_mutex);
            reject_before_dispatch = lifecycle_state != ELifecycleState::Running;
        }
        if (reject_before_dispatch) {
            ResolveRejectedPresent(_present);
            FinalizeRejectedSubmissions(
                std::move(_submits),
                "executor is stopping or stopped"
            );
            return;
        }

        RHIBackendSubmissionBatch          batch{};
        RHIBackendSubmissionBatch          failed_batch{};
        std::shared_ptr<RHIBackendExecutor> backend{};
        std::exception_ptr                  backend_creation_failure{};
        bool                                rejected = false;
        {
            std::lock_guard dispatch_lock(dispatch_mutex);
            {
                std::lock_guard lock(submit_mutex);
                rejected = lifecycle_state != ELifecycleState::Running ||
                           pending_present.has_value();
                if (!rejected) {
                    pending_submits.reserve(pending_submits.size() + _submits.size());
                    for (RHIBackendSubmissionBatchEntry& entry : _submits) {
                        pending_submits.emplace_back(std::move(entry));
                    }
                    pending_present.emplace(std::move(*_present));
                    try {
                        // Construct the backend before detaching the accepted
                        // payload from executor ownership. If construction
                        // fails, move that payload into an explicit rejection
                        // batch instead of letting a resolver exception destroy
                        // callbacks and the Present receipt silently.
                        backend = GetBackendExecutorLocked();
                        batch   = TakePendingBatchLocked();
                    } catch (...) {
                        backend_creation_failure = std::current_exception();
                        failed_batch             = TakePendingBatchLocked();
                    }
                }
            }
            if (!rejected && !backend_creation_failure) {
                batch.topology = BuildBatchTopology(batch);
                backend->Enqueue(std::move(batch));
                if (HasRHIExecSubmitFlag(_flags, ERHIExecSubmitFlags::FlushGPU)) {
                    backend->Flush(ERHIFlushDepth::SubmitGPU);
                }
            }
        }
        if (backend_creation_failure) {
            ReportBackendCreationFailure("Present", backend_creation_failure);
            FinalizeRejectedBackendBatch(
                std::move(failed_batch), "backend creation failed while publishing Present"
            );
            return;
        }
        if (rejected) {
            ResolveRejectedPresent(_present);
            FinalizeRejectedSubmissions(
                std::move(_submits),
                "executor stopped while publishing Present"
            );
        }
        return;
    }

    bool rejected = false;
    {
        std::lock_guard lock(submit_mutex);
        rejected = lifecycle_state != ELifecycleState::Running ||
                   pending_present.has_value();
        if (!rejected) {
            pending_submits.reserve(pending_submits.size() + _submits.size());
            for (RHIBackendSubmissionBatchEntry& entry : _submits) {
                pending_submits.emplace_back(std::move(entry));
            }
        }
    }
    if (rejected) {
        FinalizeRejectedSubmissions(
            std::move(_submits),
            "executor is stopping, stopped, or has an unpublished Present"
        );
        return;
    }
    if (HasRHIExecSubmitFlag(_flags, ERHIExecSubmitFlags::FlushGPU)) {
        FlushReady(ERHIFlushDepth::SubmitGPU);
    }
}

void RHIExecutor::Submit(
    Array<CommandList>&& _command_lists,
    ERHIExecSubmitFlags  _flags,
    RHIPresentRequest*   _present
) {
    Array<RHIBackendSubmissionBatchEntry> submits{};
    submits.reserve(_command_lists.size());
    for (CommandList& command_list : _command_lists) {
        if (command_list.IsEmpty()) {
            continue;
        }
        const EQueueType queue = command_list.GetQueueType();
        submits.emplace_back(queue, command_list.Submit());
    }
    Submit(std::move(submits), _flags, _present);
}

void RHIExecutor::SubmitRecording(
    SharedPtr<CommandList> _command_list,
    RHIRecordingGateRef    _completion,
    ERHIExecSubmitFlags    _flags
) {
    Array<SharedPtr<CommandList>> command_lists{};
    command_lists.emplace_back(std::move(_command_list));
    SubmitRecording(std::move(command_lists), std::move(_completion), _flags);
}

void RHIExecutor::SubmitRecording(
    Array<SharedPtr<CommandList>>&& _command_lists,
    RHIRecordingGateRef             _batch_completion,
    ERHIExecSubmitFlags             _flags
) {
    Array<RHIRecordingSource> sources{};
    sources.reserve(_command_lists.size());
    for (SharedPtr<CommandList>& command_list : _command_lists) {
        if (!command_list) {
            continue;
        }
        sources.emplace_back(RHIRecordingSource{
            .command_list = std::move(command_list),
            .completion   = _batch_completion,
        });
    }
    SubmitRecording(std::move(sources), _flags);
}

void RHIExecutor::SubmitRecording(
    Array<RHIRecordingSource>&& _sources,
    ERHIExecSubmitFlags         _flags
) {
    _sources.erase(
        std::remove_if(
            _sources.begin(),
            _sources.end(),
            [](const RHIRecordingSource& _source) {
                return !_source.command_list;
            }
        ),
        _sources.end()
    );

    if (_sources.empty()) {
        Array<RHIBackendSubmissionBatchEntry> submits{};
        Submit(std::move(submits), _flags, nullptr);
        return;
    }

    const bool sources_valid = std::all_of(
        _sources.begin(),
        _sources.end(),
        [](const RHIRecordingSource& _source) {
            return _source.completion &&
                   IsSubmissionQueue(_source.command_list->GetQueueType());
        }
    );
    if (!sources_valid) {
        FinalizeCancelledRecordingSources(
            std::move(_sources),
            "missing completion gate or invalid queue"
        );
        return;
    }

    auto payload     = std::make_shared<RecordingSubmissionPayload>();
    payload->sources = std::move(_sources);
    payload->flags   = _flags;

    std::vector<RHIRecordingGateRef> prerequisites{};
    prerequisites.reserve(payload->sources.size() * 2);
    for (const RHIRecordingSource& source : payload->sources) {
        prerequisites.emplace_back(source.completion.gate);
        if (source.commit &&
            std::find(
                prerequisites.begin(),
                prerequisites.end(),
                source.commit.gate
            ) == prerequisites.end()) {
            prerequisites.emplace_back(source.commit.gate);
        }
    }

    recording_handoff.EnqueueRecording(RHIExecutorRecordingHandoffWork{
        .prerequisites = std::move(prerequisites),
        .resolve = [this, payload](ERHIRecordingHandoffResult _result) mutable {
            if (_result == ERHIRecordingHandoffResult::Cancel) {
                FinalizeCancelledRecordingSources(
                    std::move(payload->sources),
                    "completion gate cancelled during shutdown"
                );
                return;
            }
            if (_result == ERHIRecordingHandoffResult::Reject) {
                FinalizeFailedRecordingSources(
                    std::move(payload->sources),
                    "recording completion gate failed"
                );
                return;
            }

            Array<RHIBackendSubmissionBatchEntry> submits{};
            Array<size_t> materialized_source_indices{};
            submits.reserve(payload->sources.size());
            materialized_source_indices.reserve(payload->sources.size());
            try {
                // First seal every source. Only after the whole recording
                // group owns immutable CmdSubmit payloads may metadata be
                // applied. If a later metadata copy fails, the existing
                // frontend rejection path can then terminalize callbacks and
                // rollback payload for the complete materialized group.
                for (size_t source_index = 0; source_index < payload->sources.size();
                     ++source_index) {
                    RHIRecordingSource& source = payload->sources[source_index];
                    // Queue type is stable for a CommandList, but validate it
                    // again at the ownership transition before materializing.
                    const EQueueType queue = source.command_list->GetQueueType();
                    if (!IsSubmissionQueue(queue)) {
                        throw std::runtime_error("recorded source changed to an invalid queue");
                    }
                    const bool metadata_queue_work =
                        !source.submit_metadata.wait_fences.empty() ||
                        !source.submit_metadata.signal_fences.empty();
                    if (source.command_list->IsEmpty() && !metadata_queue_work) {
                        continue;
                    }
                    submits.emplace_back(queue, source.command_list->Submit());
                    materialized_source_indices.emplace_back(source_index);
                }

                for (size_t submit_index = 0; submit_index < submits.size(); ++submit_index) {
                    RHIRecordingSource& source =
                        payload->sources[materialized_source_indices[submit_index]];
                    auto& submit   = submits[submit_index].submit;
                    auto& metadata = source.submit_metadata;
                    if (metadata.debug_label) {
                        submit.DebugLabel(*metadata.debug_label, metadata.debug_label_color);
                    }
                    if (metadata.profiling_phase) {
                        submit.SetProfilingPhase(*metadata.profiling_phase);
                    }
                    if (metadata.translate_execution_class) {
                        submit.SetTranslateExecutionClass(*metadata.translate_execution_class);
                    }
                    submit.async_queue_scope = metadata.async_queue_scope;

                    auto sync_keepalive = std::make_shared<Array<FenceRef>>();
                    sync_keepalive->reserve(
                        metadata.wait_fences.size() +
                        metadata.signal_fences.size()
                    );
                    for (const RHIRecordingFencePoint& point :
                         metadata.wait_fences) {
                        if (!point.fence.IsValid() || point.value == 0) {
                            throw std::invalid_argument(
                                "recording wait fence point is invalid"
                            );
                        }
                        submit.wait_events.emplace_back(
                            uint64(point.fence.Get()), point.value
                        );
                        sync_keepalive->emplace_back(point.fence);
                    }
                    for (const RHIRecordingFencePoint& point :
                         metadata.signal_fences) {
                        if (!point.fence.IsValid() || point.value == 0) {
                            throw std::invalid_argument(
                                "recording signal fence point is invalid"
                            );
                        }
                        submit.signal_events.emplace_back(
                            uint64(point.fence.Get()), point.value
                        );
                        sync_keepalive->emplace_back(point.fence);
                    }
                    if (!sync_keepalive->empty()) {
                        // CmdSubmit callbacks are retained by the native
                        // completion packet on success and rejection alike.
                        submit.callbacks.emplace_back([sync_keepalive] {});
                    }
                }
            } catch (const std::exception& exception) {
                LOG_ERROR(
                    "[RHIExecutor] failed to materialize recording sources: {}",
                    exception.what()
                );
                RejectRecordingSignalMetadata(payload->sources);
                FinalizeRejectedSubmissions(
                    std::move(submits),
                    "recording source finalization failed"
                );
                return;
            } catch (...) {
                LOG_ERROR("[RHIExecutor] failed to materialize recording sources");
                RejectRecordingSignalMetadata(payload->sources);
                FinalizeRejectedSubmissions(
                    std::move(submits),
                    "recording source finalization failed"
                );
                return;
            }

            payload->sources.clear();
            SubmitReady(std::move(submits), payload->flags, nullptr);
        },
    });
}

void RHIExecutor::Present(RHIPresentRequest&& _present, bool _flush) {
    Array<RHIBackendSubmissionBatchEntry> submits{};
    Submit(
        std::move(submits),
        _flush ? ERHIExecSubmitFlags::FlushGPU : ERHIExecSubmitFlags::None,
        &_present
    );
}

void RHIExecutor::Flush(ERHIFlushDepth _depth) {
    recording_handoff.RouteReady(RHIExecutorRecordingHandoffWork{
        .prerequisites = {},
        .resolve = [this, _depth](ERHIRecordingHandoffResult _result) {
            if (_result == ERHIRecordingHandoffResult::Consume) {
                FlushReady(_depth);
            }
        },
    });
}

void RHIExecutor::FlushReady(ERHIFlushDepth _depth) {
    {
        std::lock_guard lock(submit_mutex);
        if (lifecycle_state != ELifecycleState::Running) {
            return;
        }
    }

    RHIBackendSubmissionBatch          batch{};
    RHIBackendSubmissionBatch          failed_batch{};
    std::shared_ptr<RHIBackendExecutor> backend;
    std::exception_ptr                  backend_creation_failure{};
    {
        std::lock_guard dispatch_lock(dispatch_mutex);
        {
            std::lock_guard lock(submit_mutex);
            if (lifecycle_state != ELifecycleState::Running) {
                return;
            }
            const bool has_pending_work =
                !pending_submits.empty() || pending_present.has_value();
            if (has_pending_work) {
                try {
                    backend = GetBackendExecutorLocked();
                    batch   = TakePendingBatchLocked();
                } catch (...) {
                    backend_creation_failure = std::current_exception();
                    failed_batch             = TakePendingBatchLocked();
                }
            } else {
                backend = backend_executor;
            }
        }
        if (!backend_creation_failure) {
            if (BatchHasWork(batch)) {
                batch.topology = BuildBatchTopology(batch);
                backend->Enqueue(std::move(batch));
            }
            if (backend) {
                backend->Flush(_depth);
            }
        }
    }
    if (backend_creation_failure) {
        ReportBackendCreationFailure("Flush", backend_creation_failure);
        FinalizeRejectedBackendBatch(
            std::move(failed_batch), "backend creation failed while flushing"
        );
    }
}

void RHIExecutor::Sync(ERHISyncDepth _depth) {
    if (RejectOwnedThreadBlockingCall("Sync")) {
        return;
    }
    auto completion = std::make_shared<HandoffSyncCompletion>();
    recording_handoff.RouteReady(RHIExecutorRecordingHandoffWork{
        .prerequisites = {},
        .resolve = [this, _depth, completion](ERHIRecordingHandoffResult _result) {
            ScopeExit signal_completion([completion] { completion->Signal(); });
            if (_result == ERHIRecordingHandoffResult::Consume) {
                SyncReady(_depth);
            }
        },
    });
    completion->Wait();
}

void RHIExecutor::SyncReady(ERHISyncDepth _depth) {
    {
        std::lock_guard lock(submit_mutex);
        if (lifecycle_state != ELifecycleState::Running) {
            return;
        }
    }

    std::shared_ptr<RHIBackendExecutor> backend{};
    RHIBackendSubmissionBatch           failed_batch{};
    std::exception_ptr                   backend_creation_failure{};
    bool                                sync_registered = false;
    auto finish_sync_call = [this, &sync_registered, &backend] {
        // A zero active-sync count is the shutdown handoff point. Release this
        // call's backend ownership before publishing that point so StartUp can
        // never race a stale queue-ownership lease, even if SyncReady is later
        // called from something other than the joined handoff worker.
        backend.reset();
        if (!sync_registered) {
            return;
        }
        {
            std::lock_guard lock(submit_mutex);
            assert(active_sync_calls > 0);
            --active_sync_calls;
        }
        sync_registered = false;
        lifecycle_cv.notify_all();
    };
    ScopeExit finish_sync_guard(std::move(finish_sync_call));
    {
        std::lock_guard dispatch_lock(dispatch_mutex);
        RHIBackendSubmissionBatch batch{};
        {
            std::lock_guard lock(submit_mutex);
            if (lifecycle_state == ELifecycleState::Running) {
                const bool has_pending_work =
                    !pending_submits.empty() || pending_present.has_value();
                if (has_pending_work) {
                    try {
                        backend = GetBackendExecutorLocked();
                        batch   = TakePendingBatchLocked();
                    } catch (...) {
                        backend_creation_failure = std::current_exception();
                        failed_batch             = TakePendingBatchLocked();
                    }
                } else {
                    backend = backend_executor;
                }
                if (backend) {
                    ++active_sync_calls;
                    sync_registered = true;
                }
            }
        }
        if (!backend_creation_failure) {
            if (BatchHasWork(batch)) {
                batch.topology = BuildBatchTopology(batch);
                backend->Enqueue(std::move(batch));
            }
            if (backend) {
                backend->Flush(ERHIFlushDepth::SubmitGPU);
            }
        }
    }

    if (backend_creation_failure) {
        ReportBackendCreationFailure("Sync", backend_creation_failure);
        FinalizeRejectedBackendBatch(
            std::move(failed_batch), "backend creation failed while synchronizing"
        );
        return;
    }

    GraphEventRef completion = backend ? backend->Sync(_depth) : GraphEventRef{};
    if (completion) {
        completion->Wait(EThread::UNKNOWN_THREAD);
    }
}

void RHIExecutor::ShutDown() {
    if (RejectOwnedThreadBlockingCall("ShutDown")) {
        return;
    }
    RHIExecutor& executor = Get();
    uint64       shutdown_to_complete = 0;
    std::shared_ptr<RHIBackendExecutor> backend_to_cancel{};
    std::shared_ptr<RHIBackendExecutor> backend{};

    {
        std::unique_lock lock(executor.submit_mutex);
        if (executor.lifecycle_state == ELifecycleState::Stopped) {
            return;
        }
        if (executor.lifecycle_state == ELifecycleState::Stopping) {
            const uint64 shutdown_to_wait = executor.shutdown_generation;
            executor.lifecycle_cv.wait(lock, [&executor, shutdown_to_wait] {
                return executor.completed_shutdown_generation >= shutdown_to_wait;
            });
            return;
        }
        executor.lifecycle_state = ELifecycleState::Stopping;
        shutdown_to_complete     = ++executor.shutdown_generation;
        backend_to_cancel        = executor.backend_executor;
    }

    // Install lifecycle completion before any operation that can allocate or
    // otherwise throw. In particular, lazy backend construction must never
    // strand concurrent Shutdown/StartUp callers behind a permanent Stopping
    // state.
    auto finish_shutdown =
        [&executor, shutdown_to_complete, &backend, &backend_to_cancel] {
        // StartUp may begin as soon as Stopped is published. Drop every local
        // reference to the old backend first so its queue ownership leases are
        // released before a replacement backend tries to claim them.
        backend.reset();
        backend_to_cancel.reset();
        {
            std::lock_guard lock(executor.submit_mutex);
            executor.completed_shutdown_generation = std::max(
                executor.completed_shutdown_generation,
                shutdown_to_complete
            );
            executor.lifecycle_state = ELifecycleState::Stopped;
        }
        executor.lifecycle_cv.notify_all();
    };
    ScopeExit finish_shutdown_guard(std::move(finish_shutdown));

    // Publish backend cancellation before joining the handoff owner. That
    // owner may currently be inside SyncReady waiting behind a Submission
    // dependency which can no longer acquire a producer during shutdown.
    if (backend_to_cancel) {
        backend_to_cancel->BeginShutdown();
    }

    // Stop accepting handoffs before taking the final pending batch. The
    // worker's stop token interrupts an unfinished recording gate, rejects it
    // and every later FIFO operation, and joins without depending on producer
    // progress. Any callback already inside SubmitReady/SyncReady is allowed
    // to finish before backend ownership is detached below.
    executor.recording_handoff.ShutDown();

    RHIBackendSubmissionBatch           batch{};
    RHIBackendSubmissionBatch           failed_batch{};
    std::exception_ptr                   backend_creation_failure{};
    {
        std::lock_guard dispatch_lock(executor.dispatch_mutex);
        {
            std::lock_guard lock(executor.submit_mutex);
            const bool has_pending_work =
                !executor.pending_submits.empty() ||
                executor.pending_present.has_value();
            if (has_pending_work) {
                try {
                    executor.GetBackendExecutorLocked();
                    batch = executor.TakePendingBatchLocked();
                } catch (...) {
                    backend_creation_failure = std::current_exception();
                    failed_batch = executor.TakePendingBatchLocked();
                }
            }
            backend = std::move(executor.backend_executor);
        }
        if (!backend_creation_failure && BatchHasWork(batch) && backend) {
            batch.topology = BuildBatchTopology(batch);
            backend->Enqueue(std::move(batch));
        }
        if (!backend_creation_failure && backend) {
            backend->Flush(ERHIFlushDepth::SubmitGPU);
        }
    }

    if (backend_creation_failure) {
        ReportBackendCreationFailure("ShutDown", backend_creation_failure);
        FinalizeRejectedBackendBatch(
            std::move(failed_batch), "backend creation failed during shutdown"
        );
    }

    // Cancel backend host waits before waiting for concurrent Sync calls.
    // Otherwise a Sync already queued behind an unpublished dependency would
    // wait for backend ShutDown, while ShutDown waited for that same Sync.
    if (backend) {
        backend->BeginShutdown();
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
    if (backend) {
        backend->ShutDown();
    }
}

} // namespace Moer::Render
