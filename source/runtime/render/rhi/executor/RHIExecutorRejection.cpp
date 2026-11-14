#include "rhi/executor/RHIExecutorInternal.h"

#include "log/LogSystem.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <utility>

namespace Moer::Render::RHIExecutorDetail {

namespace {
void PublishPayloadCompletionErrors(
    std::span<RHISubmissionRequest>         _submits,
    std::span<PreparedCommandListRejection> _prepared,
    std::string_view                        _reason,
    GpuCompletionPublishBatch               _batch
) noexcept {
    for (RHISubmissionRequest& entry : _submits) {
        const GpuCompletionPublishBatch entry_batch =
            entry.submit.gpu_completion_publish_batch.Valid() ?
                entry.submit.gpu_completion_publish_batch :
                _batch;
        entry.submit.PublishPendingGpuCompletionErrors(
            _reason, entry_batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->PublishPendingGpuCompletionErrors(
                _reason, _batch
            );
        }
    }
}

void PublishPayloadQueryErrors(
    std::span<RHISubmissionRequest>         _submits,
    std::span<PreparedCommandListRejection> _prepared,
    std::string_view                        _reason,
    QueryPublishBatch                       _batch
) noexcept {
    for (RHISubmissionRequest& entry : _submits) {
        const QueryPublishBatch entry_batch =
            entry.submit.query_publish_batch.Valid() ?
                entry.submit.query_publish_batch :
                _batch;
        entry.submit.PublishPendingQueryErrors(_reason, entry_batch);
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->PublishPendingQueryErrors(
                _reason, _batch
            );
        }
    }
}

void NotifyPayloadCompletions(
    std::span<RHISubmissionRequest>         _submits,
    std::span<PreparedCommandListRejection> _prepared,
    GpuCompletionPublishBatch               _batch
) noexcept {
    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.NotifyPendingGpuCompletions(
            entry.submit.gpu_completion_publish_batch.Valid() ?
                entry.submit.gpu_completion_publish_batch :
                _batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->NotifyPendingGpuCompletions(
                _batch
            );
        }
    }
}

void NotifyPayloadQueries(
    std::span<RHISubmissionRequest>         _submits,
    std::span<PreparedCommandListRejection> _prepared,
    QueryPublishBatch                       _batch
) noexcept {
    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.NotifyPendingQueries(
            entry.submit.query_publish_batch.Valid() ?
                entry.submit.query_publish_batch :
                _batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->NotifyPendingQueries(_batch);
        }
    }
}

void RejectPayloadSignals(
    std::span<RHISubmissionRequest>         _submits,
    std::span<PreparedCommandListRejection> _prepared
) noexcept {
    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.RejectPendingSignals();
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->RejectPendingSignals();
        }
    }
}

void InvokeCallbacksNoThrow(
    Array<std::function<void()>>& _callbacks,
    std::string_view              _context
) noexcept {
    for (std::function<void()>& callback : _callbacks) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (const std::exception& exception) {
            try {
                LOG_ERROR("{} cleanup callback threw: {}", _context, exception.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR("{} cleanup callback threw", _context);
            } catch (...) {
            }
        }
    }
}

size_t CleanupPreparedRejections(
    std::span<PreparedCommandListRejection> _prepared,
    std::string_view                        _context
) noexcept {
    size_t callback_count = 0;
    for (PreparedCommandListRejection& source : _prepared) {
        if (!source.payload) {
            continue;
        }
        callback_count += source.payload->callbacks.size();
        InvokeCallbacksNoThrow(source.payload->callbacks, _context);
        source.payload->callbacks.clear();
        source.payload->success_callbacks.clear();
    }
    return callback_count;
}

void PrepareUnsubmittedCommandLists(
    std::span<CommandList>                                _command_lists,
    std::span<CommandList::RecordingRejectionPreparation> _rejection_preparations,
    std::span<PreparedCommandListRejection>               _prepared,
    std::span<const size_t>                               _materialized_source_indices
) noexcept {
    size_t materialized_cursor = 0;
    for (size_t source_index = 0;
         source_index < _command_lists.size();
         ++source_index) {
        if (materialized_cursor < _materialized_source_indices.size() &&
            _materialized_source_indices[materialized_cursor] ==
                source_index) {
            ++materialized_cursor;
            continue;
        }
        _prepared[source_index].payload.emplace(
            _command_lists[source_index].PrepareRecordingRejectionDrain(
                std::move(
                    _rejection_preparations[source_index]
                )
            )
        );
    }
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

} // namespace

// A frontend rejection can only happen for invalid input or after the executor
// has stopped accepting work.  Normal completion callbacks still own cleanup
// responsibilities, while success callbacks must never run for work that did
// not reach a GPU queue.  Keep this path backend-neutral and, importantly, run
// user code only after all executor locks have been released.
void FinalizeRejectedSubmissions(
    Array<RHISubmissionRequest>&& _submits,
    std::string_view              _reason,
    RHIPresentRequest*            _present
) noexcept {
    size_t callback_count         = 0;
    size_t success_callback_count = 0;
    size_t signal_count           = 0;
    for (const RHISubmissionRequest& entry : _submits) {
        callback_count += entry.submit.callbacks.size();
        success_callback_count += entry.submit.success_callbacks.size();
        signal_count += entry.submit.signal_events.size();
    }

    try {
        LOG_ERROR(
            "[RHIExecutor] rejected {} frontend submit(s): {}; callbacks={} "
            "success_callbacks_skipped={} signal_events_rejected={}",
            _submits.size(),
            _reason,
            callback_count,
            success_callback_count,
            signal_count
        );
    } catch (...) {
    }

    // Terminalize every host observation in the rejected batch before
    // invoking Fence::Reject or any user callback. A Fence rejection hook may
    // synchronously wait on a Future from this batch.
    // A callback from an earlier submit is allowed to inspect or
    // wait on a query from a later submit without depending on callback order.
    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    PublishPayloadCompletionErrors(_submits, {}, _reason, completion_batch);
    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    PublishPayloadQueryErrors(_submits, {}, _reason, query_batch);

    // Publication wakes Wait/Get but deliberately does not release callbacks.
    // Signals must be terminal before callback notification, while publication
    // first prevents a synchronous Fence::Reject hook from self-deadlocking.
    RejectPayloadSignals(_submits, {});

    // A Present receipt is a public observation boundary for the complete
    // frontend batch. Publish every sibling Signal and Query terminal state
    // before waking its waiter, but resolve it before Query/ordinary callback
    // notification so user code can safely wait for the receipt.
    ResolveRejectedPresent(_present);

    NotifyPayloadCompletions(_submits, {}, completion_batch);
    NotifyPayloadQueries(_submits, {}, query_batch);

    for (RHISubmissionRequest& entry : _submits) {
        InvokeCallbacksNoThrow(entry.submit.callbacks, "[RHIExecutor] rejected submit");
    }
}

// Value-owned CommandList batches are one frontend transaction just like
// asynchronously recorded source groups. If sealing one member fails, both
// already-materialized submits and the untouched suffix must become terminal
// before any Query or ordinary cleanup callback is allowed to run.
void FinalizeRejectedCommandListGroup(
    Array<CommandList>&                                _command_lists,
    const Array<GpuCompletionCancellationView>&        _completion_cancellations,
    const Array<QueryCancellationView>&                _query_cancellations,
    Array<CommandList::RecordingRejectionPreparation>& _rejection_preparations,
    Array<PreparedCommandListRejection>&               _prepared,
    const Array<size_t>&                               _materialized_source_indices,
    Array<RHISubmissionRequest>&&                      _submits,
    RHIPresentRequest*                                 _present,
    std::string_view                                   _reason
) noexcept {
    assert(
        _completion_cancellations.size() == _command_lists.size() &&
        _query_cancellations.size() == _command_lists.size() &&
        _rejection_preparations.size() == _command_lists.size() &&
        _prepared.size() == _command_lists.size()
    );
    PrepareUnsubmittedCommandLists(
        _command_lists, _rejection_preparations, _prepared, _materialized_source_indices
    );

    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    for (const GpuCompletionCancellationView& cancellation :
         _completion_cancellations) {
        (void)cancellation.PublishCancellation(
            _reason, completion_batch
        );
    }
    PublishPayloadCompletionErrors(_submits, _prepared, _reason, completion_batch);

    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (const QueryCancellationView& cancellation :
         _query_cancellations) {
        (void)cancellation.PublishCancellation(_reason, query_batch);
    }
    PublishPayloadQueryErrors(_submits, _prepared, _reason, query_batch);

    // Every sibling host state is terminal before synchronous Fence rejection.
    // Callback tickets remain retained until the notification phase below.
    RejectPayloadSignals(_submits, _prepared);

    // Query state publication is distinct from callback notification. Resolve
    // Present only after the complete group is terminal, then release Query
    // callbacks so they may synchronously observe or wait for that receipt.
    ResolveRejectedPresent(_present);

    for (const GpuCompletionCancellationView& cancellation :
         _completion_cancellations) {
        cancellation.NotifyCancellation(completion_batch);
        cancellation.NotifyPublishedCancellation();
    }
    NotifyPayloadCompletions(_submits, _prepared, completion_batch);

    for (const QueryCancellationView& cancellation :
         _query_cancellations) {
        cancellation.NotifyCancellation(query_batch);
        cancellation.NotifyPublishedCancellation();
    }
    NotifyPayloadQueries(_submits, _prepared, query_batch);

    FinalizeRejectedSubmissions(std::move(_submits), _reason);

    const size_t cleanup_callback_count =
        CleanupPreparedRejections(_prepared, "[RHIExecutor] rejected CommandList group");
    try {
        LOG_ERROR(
            "[RHIExecutor] rejected {} CommandList source(s): {}; "
            "cleanup_callbacks={}",
            _command_lists.size(),
            _reason,
            cleanup_callback_count
        );
    } catch (...) {
    }
}

namespace {

void RejectRecordingSignalMetadata(
    const RHIRecordingSource& _source,
    size_t                    _transferred_signal_prefix
) noexcept {
    // The transferred prefix belongs to an immutable CmdSubmit, whose rejection
    // handles those signals. Reject only the remaining source-owned metadata so
    // transferred entries are not rejected twice and published metadata stays immutable.
    const auto& signal_fences = _source.submit_metadata.signal_fences;
    for (size_t signal_index = std::min(_transferred_signal_prefix, signal_fences.size());
         signal_index < signal_fences.size();
         ++signal_index) {
        const RHIRecordingFencePoint& point = signal_fences[signal_index];
        if (!point.fence.IsValid() || point.value == 0) {
            continue;
        }
        try {
            point.fence->Reject(point.value);
        } catch (...) {
        }
    }
}

bool CanDrainRecordingSource(
    const RHIRecordingSource& _source
) noexcept {
    if (!_source.completion ||
        _source.completion.Status() ==
            ERHIRecordingStatus::Pending) {
        return false;
    }
    if (_source.commit &&
        _source.commit.Status() ==
            ERHIRecordingStatus::Pending) {
        return false;
    }
    // The recording and optional commit gates are the synchronization handoff
    // for the producer-owned, non-atomic lease counter. Never inspect it while
    // either gate remains Pending: RenderGraph releases the lease immediately
    // before signaling its transaction gate.
    return !_source.command_list ||
           !_source.command_list->HasManagedRecordingLease();
}

bool PrepareNewlyDrainableSources(
    std::span<RHIRecordingSource>                         _sources,
    std::span<CommandList::RecordingRejectionPreparation> _rejection_preparations,
    std::span<PreparedCommandListRejection>               _prepared,
    std::span<ERecordingRejectionState>                   _rejection_states,
    size_t&                                               _drained_source_count,
    size_t&                                               _cleanup_callback_count
) noexcept {
    bool prepared_any = false;
    for (size_t source_index = 0;
         source_index < _sources.size();
         ++source_index) {
        if (_rejection_states[source_index] != ERecordingRejectionState::ProducerOwned) {
            continue;
        }
        RHIRecordingSource& source = _sources[source_index];
        if (!CanDrainRecordingSource(source)) {
            continue;
        }

        _rejection_states[source_index] = ERecordingRejectionState::SignalRejectionPending;
        ++_drained_source_count;
        prepared_any = true;
        if (!source.command_list) {
            continue;
        }
        _prepared[source_index].payload.emplace(
            source.command_list->PrepareRecordingRejectionDrain(
                std::move(
                    _rejection_preparations[source_index]
                )
            )
        );
        _cleanup_callback_count +=
            _prepared[source_index].payload->callbacks.size();
    }
    return prepared_any;
}

// Future cancellation is one transaction across the complete recording group.
// Completion/Query errors are published before any Fence rejection. A later
// pass notifies only sources whose ownership and signal rejection are complete.
// Mutable sources publish Error so Wait/Get remain bounded during shutdown,
// but defer callback notification until their CommandList is safely owned.
void PublishRecordingErrorsAndNotifyOwnedSources(
    Array<RHIRecordingSource>&                _sources,
    std::string_view                          _reason,
    std::span<const ERecordingRejectionState> _rejection_states,
    std::span<PreparedCommandListRejection>   _prepared
) noexcept {
    // Notification follows recorded rejection progress. Never resample producer
    // gates between signal rejection and callback notification.
    assert(_rejection_states.size() == _sources.size());

    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    for (RHIRecordingSource& source : _sources) {
        (void)source.gpu_completion_cancellation.PublishCancellation(_reason, completion_batch);
    }
    PublishPayloadCompletionErrors({}, _prepared, _reason, completion_batch);

    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (RHIRecordingSource& source : _sources) {
        (void)source.query_cancellation.PublishCancellation(
            _reason, query_batch
        );
    }
    PublishPayloadQueryErrors({}, _prepared, _reason, query_batch);
    for (size_t source_index = 0; source_index < _sources.size();
         ++source_index) {
        if (_rejection_states[source_index] != ERecordingRejectionState::ProducerOwned) {
            _sources[source_index].gpu_completion_cancellation.NotifyCancellation(
                completion_batch
            );
            _sources[source_index].gpu_completion_cancellation.NotifyPublishedCancellation();
        }
    }
    NotifyPayloadCompletions({}, _prepared, completion_batch);
    for (size_t source_index = 0; source_index < _sources.size();
         ++source_index) {
        if (_rejection_states[source_index] == ERecordingRejectionState::ProducerOwned) {
            continue;
        }
        _sources[source_index].query_cancellation.NotifyCancellation(
            query_batch
        );
        _sources[source_index].query_cancellation.NotifyPublishedCancellation();
    }
    NotifyPayloadQueries({}, _prepared, query_batch);
}

} // namespace

void FinalizeRejectedBackendBatch(RHISubmissionBatch&& _batch, std::string_view _reason) noexcept {
    RHIPresentRequest* present =
        _batch.present.has_value() ? &*_batch.present : nullptr;
    FinalizeRejectedSubmissions(
        std::move(_batch.submits), _reason, present
    );
}

void FinalizeCancelledRecordingSources(
    Array<RHIRecordingSource>&& _sources,
    std::string_view            _reason,
    Array<CommandList::RecordingRejectionPreparation>&
                                         _rejection_preparations,
    Array<PreparedCommandListRejection>& _prepared,
    Array<ERecordingRejectionState>&     _rejection_states
) noexcept {
    // A source protected by an incomplete recording gate must remain opaque:
    // even reading IsEmpty(), let alone draining callbacks, races its producer.
    // Once both gates are terminal and the managed lease is released, ordinary
    // callbacks still own CPU/resource cleanup even when shutdown cancels the
    // FIFO before the source can be materialized. Success-only callbacks must
    // never run because no GPU submission was published.
    assert(
        _rejection_preparations.size() == _sources.size() &&
        _prepared.size() == _sources.size() &&
        _rejection_states.size() == _sources.size()
    );
    std::fill(
        _rejection_states.begin(),
        _rejection_states.end(),
        ERecordingRejectionState::ProducerOwned
    );
    size_t drained_source_count   = 0;
    size_t cleanup_callback_count = 0;
    // Publish host Errors for the complete group before invoking any
    // Fence::Reject hook. Initially every rejection state is ProducerOwned, so
    // no callback is released. The fixed-point loop below discovers and detaches
    // drainable sources, while opaque sources remain callback-deferred.
    PublishRecordingErrorsAndNotifyOwnedSources(
        _sources, _reason, _rejection_states, _prepared
    );

    // Detach every newly drainable source before its metadata or
    // payload Fence rejection. A rejection hook may synchronously complete
    // another producer/commit gate; the next iteration detaches that newly
    // owned generation before touching any of its re-entrant observers.
    while (PrepareNewlyDrainableSources(
        _sources, _rejection_preparations, _prepared, _rejection_states, drained_source_count, cleanup_callback_count
    )) {
        for (size_t source_index = 0; source_index < _sources.size(); ++source_index) {
            if (_rejection_states[source_index] == ERecordingRejectionState::SignalRejectionPending) {
                RejectRecordingSignalMetadata(_sources[source_index], 0);
            }
        }
        for (size_t source_index = 0;
             source_index < _prepared.size();
             ++source_index) {
            if (_rejection_states[source_index] != ERecordingRejectionState::SignalRejectionPending) {
                continue;
            }
            if (_prepared[source_index].payload) {
                _prepared[source_index].payload->RejectPendingSignals();
            }
            _rejection_states[source_index] = ERecordingRejectionState::SignalRejectionComplete;
        }
    }

    // Sources still absent from the fixed point remain opaque and receive
    // Error publication without callback release.
    const size_t opaque_source_count =
        _sources.size() - drained_source_count;
    PublishRecordingErrorsAndNotifyOwnedSources(
        _sources, _reason, _rejection_states, _prepared
    );
    CleanupPreparedRejections(_prepared, "[RHIExecutor] cancelled recording");

    try {
        LOG_WARNING(
            "[RHIExecutor] rejected {} recording source(s): {}; "
            "terminal_sources={} opaque_sources={} cleanup_callbacks={}",
            _sources.size(),
            _reason,
            drained_source_count,
            opaque_source_count,
            cleanup_callback_count
        );
    } catch (...) {
    }
}

void FinalizeFailedRecordingSources(
    Array<RHIRecordingSource>&&                        _sources,
    std::string_view                                   _reason,
    Array<CommandList::RecordingRejectionPreparation>& _rejection_preparations,
    Array<PreparedCommandListRejection>&               _prepared,
    Array<ERecordingRejectionState>&                   _rejection_states,
    Array<RHISubmissionRequest>                        _materialized_submits,
    std::span<const size_t>                            _metadata_signal_transfer_prefixes
) noexcept {
    // Reject normally follows terminal producer/commit gates and released
    // managed leases. Failed/partial CommandLists are then immutable and may be
    // destructively drained for their ordinary cleanup callbacks.
    // Detach every generation before publishing errors or invoking observers;
    // rejected payloads retain cleanup ownership and never reach a backend.
    const bool all_sources_drainable = std::all_of(
        _sources.begin(),
        _sources.end(),
        [](const RHIRecordingSource& _source) {
            return CanDrainRecordingSource(_source);
        }
    );
    if (!all_sources_drainable) {
        try {
            LOG_ERROR(
                "[RHIExecutor] failed recording group resolved before every producer became terminal"
            );
        } catch (...) {
        }
        // Materialization is entered only after the Executor work queue observes
        // every prerequisite terminal. Keep this invariant explicit: an
        // immutable prefix combined with an opaque producer cannot be safely
        // notified as one transaction.
        assert(_materialized_submits.empty());
        FinalizeCancelledRecordingSources(
            std::move(_sources),
            "failed recording group still has a mutable producer",
            _rejection_preparations,
            _prepared,
            _rejection_states
        );
        if (!_materialized_submits.empty()) {
            FinalizeRejectedSubmissions(
                std::move(_materialized_submits),
                "failed recording group still has a mutable producer"
            );
        }
        return;
    }

    const size_t source_count = _sources.size();
    assert(
        _rejection_preparations.size() == _sources.size() &&
        _prepared.size() == _sources.size() &&
        _rejection_states.size() == _sources.size()
    );
    size_t cleanup_callback_count = 0;
    for (size_t source_index = 0; source_index < _sources.size();
         ++source_index) {
        RHIRecordingSource& source = _sources[source_index];
        if (!source.command_list) {
            continue;
        }
        _prepared[source_index].payload.emplace(
            source.command_list->PrepareRecordingRejectionDrain(
                std::move(
                    _rejection_preparations[source_index]
                )
            )
        );
        cleanup_callback_count +=
            _prepared[source_index].payload->callbacks.size();
    }

    // Publish every source-owned and already-materialized Future before
    // notifying any one of them. A callback from either half may therefore
    // synchronously wait on every sibling without depending on callback order.
    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    for (RHIRecordingSource& source : _sources) {
        (void)source.gpu_completion_cancellation.PublishCancellation(
            _reason, completion_batch
        );
    }
    PublishPayloadCompletionErrors(_materialized_submits, _prepared, _reason, completion_batch);

    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (RHIRecordingSource& source : _sources) {
        (void)source.query_cancellation.PublishCancellation(
            _reason, query_batch
        );
    }
    PublishPayloadQueryErrors(_materialized_submits, _prepared, _reason, query_batch);

    // Batch phase B: every sibling has installed a fresh generation and every
    // host observation is terminal before synchronous Fence rejection. A
    // prefix may already be an immutable CmdSubmit; its signals remain owned
    // here and must be rejected before callback notification.
    for (size_t source_index = 0; source_index < _sources.size(); ++source_index) {
        const size_t transferred_prefix =
            source_index < _metadata_signal_transfer_prefixes.size() ?
                _metadata_signal_transfer_prefixes[source_index] :
                0;
        RejectRecordingSignalMetadata(_sources[source_index], transferred_prefix);
    }
    RejectPayloadSignals(_materialized_submits, _prepared);

    for (RHIRecordingSource& source : _sources) {
        source.gpu_completion_cancellation.NotifyCancellation(
            completion_batch
        );
        source.gpu_completion_cancellation.NotifyPublishedCancellation();
    }
    NotifyPayloadCompletions(_materialized_submits, _prepared, completion_batch);
    for (RHIRecordingSource& source : _sources) {
        source.query_cancellation.NotifyCancellation(query_batch);
        source.query_cancellation.NotifyPublishedCancellation();
    }
    NotifyPayloadQueries(_materialized_submits, _prepared, query_batch);

    for (RHISubmissionRequest& entry : _materialized_submits) {
        cleanup_callback_count += entry.submit.callbacks.size();
        InvokeCallbacksNoThrow(
            entry.submit.callbacks,
            "[RHIExecutor] rejected materialized recording"
        );
        entry.submit.callbacks.clear();
        entry.submit.success_callbacks.clear();
    }
    CleanupPreparedRejections(_prepared, "[RHIExecutor] rejected recording");
    try {
        LOG_ERROR(
            "[RHIExecutor] rejected {} completed recording source(s): {}; "
            "materialized_submits={} cleanup_callbacks={}",
            source_count,
            _reason,
            _materialized_submits.size(),
            cleanup_callback_count
        );
    } catch (...) {
    }
}

} // namespace Moer::Render::RHIExecutorDetail
