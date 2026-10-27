#include "rhi/executor/RHIExecutorInternal.h"

#include "log/LogSystem.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <utility>

namespace Moer::Render::RHIExecutorDetail {

namespace {
void InvokeCleanupCallbacks(
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
    _callbacks.clear();
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
    size_t callback_count       = 0;
    size_t success_callback_cnt = 0;
    size_t signal_count         = 0;
    for (const RHISubmissionRequest& entry : _submits) {
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

    // Terminalize every host observation in the rejected batch before
    // invoking Fence::Reject or any user callback. A Fence rejection hook may
    // synchronously wait on a Future from this batch.
    // A callback from an earlier submit is allowed to inspect or
    // wait on a query from a later submit without depending on callback order.
    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    for (RHISubmissionRequest& entry : _submits) {
        const GpuCompletionPublishBatch entry_batch =
            entry.submit.gpu_completion_publish_batch.Valid() ?
                entry.submit.gpu_completion_publish_batch :
                completion_batch;
        entry.submit.PublishPendingGpuCompletionErrors(
            _reason, entry_batch
        );
    }
    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (RHISubmissionRequest& entry : _submits) {
        const QueryPublishBatch entry_batch =
            entry.submit.query_publish_batch.Valid() ?
                entry.submit.query_publish_batch :
                query_batch;
        entry.submit.PublishPendingQueryErrors(_reason, entry_batch);
    }

    // Publication wakes Wait/Get but deliberately does not release callbacks.
    // Signals must be terminal before callback notification, while publication
    // first prevents a synchronous Fence::Reject hook from self-deadlocking.
    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.RejectPendingSignals();
    }

    // A Present receipt is a public observation boundary for the complete
    // frontend batch. Publish every sibling Signal and Query terminal state
    // before waking its waiter, but resolve it before Query/ordinary callback
    // notification so user code can safely wait for the receipt.
    ResolveRejectedPresent(_present);

    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.NotifyPendingGpuCompletions(
            entry.submit.gpu_completion_publish_batch.Valid() ?
                entry.submit.gpu_completion_publish_batch :
                completion_batch
        );
    }
    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.NotifyPendingQueries(
            entry.submit.query_publish_batch.Valid() ?
                entry.submit.query_publish_batch :
                query_batch
        );
    }

    for (RHISubmissionRequest& entry : _submits) {
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

    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    for (const GpuCompletionCancellationView& cancellation :
         _completion_cancellations) {
        (void)cancellation.PublishCancellation(
            _reason, completion_batch
        );
    }
    for (RHISubmissionRequest& entry : _submits) {
        const GpuCompletionPublishBatch entry_batch =
            entry.submit.gpu_completion_publish_batch.Valid() ?
                entry.submit.gpu_completion_publish_batch :
                completion_batch;
        entry.submit.PublishPendingGpuCompletionErrors(
            _reason, entry_batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->PublishPendingGpuCompletionErrors(
                _reason, completion_batch
            );
        }
    }

    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (const QueryCancellationView& cancellation :
         _query_cancellations) {
        (void)cancellation.PublishCancellation(_reason, query_batch);
    }
    for (RHISubmissionRequest& entry : _submits) {
        const QueryPublishBatch entry_batch =
            entry.submit.query_publish_batch.Valid() ?
                entry.submit.query_publish_batch :
                query_batch;
        entry.submit.PublishPendingQueryErrors(_reason, entry_batch);
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->PublishPendingQueryErrors(
                _reason, query_batch
            );
        }
    }

    // Every sibling host state is terminal before synchronous Fence rejection.
    // Callback tickets remain retained until the notification phase below.
    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.RejectPendingSignals();
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->RejectPendingSignals();
        }
    }

    // Query state publication is distinct from callback notification. Resolve
    // Present only after the complete group is terminal, then release Query
    // callbacks so they may synchronously observe or wait for that receipt.
    ResolveRejectedPresent(_present);

    for (const GpuCompletionCancellationView& cancellation :
         _completion_cancellations) {
        cancellation.NotifyCancellation(completion_batch);
        cancellation.NotifyPublishedCancellation();
    }
    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.NotifyPendingGpuCompletions(
            entry.submit.gpu_completion_publish_batch.Valid() ?
                entry.submit.gpu_completion_publish_batch :
                completion_batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->NotifyPendingGpuCompletions(
                completion_batch
            );
        }
    }

    for (const QueryCancellationView& cancellation :
         _query_cancellations) {
        cancellation.NotifyCancellation(query_batch);
        cancellation.NotifyPublishedCancellation();
    }
    for (RHISubmissionRequest& entry : _submits) {
        entry.submit.NotifyPendingQueries(
            entry.submit.query_publish_batch.Valid() ?
                entry.submit.query_publish_batch :
                query_batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->NotifyPendingQueries(query_batch);
        }
    }

    FinalizeRejectedSubmissions(std::move(_submits), _reason);

    size_t cleanup_callback_count = 0;
    for (PreparedCommandListRejection& source : _prepared) {
        if (!source.payload) {
            continue;
        }
        cleanup_callback_count += source.payload->callbacks.size();
        InvokeCleanupCallbacks(
            source.payload->callbacks,
            "[RHIExecutor] rejected CommandList group"
        );
        source.payload->success_callbacks.clear();
    }
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
    const Array<RHIRecordingSource>& _sources,
    std::span<const uint8>           _owned_snapshot              = {},
    std::span<const size_t>          _transferred_signal_prefixes = {}
) noexcept {
    // Metadata is prepared source-by-source and preparation can allocate.
    // If any step throws, the entire immutable recording transaction is
    // rejected, including producer values not yet transferred into CmdSubmit.
    // A per-source committed prefix records points now owned by an immutable
    // submit, so this fallback never invokes Fence::Reject twice and the
    // published metadata itself remains immutable.
    for (size_t source_index = 0;
         source_index < _sources.size();
         ++source_index) {
        if (!_owned_snapshot.empty() &&
            (source_index >= _owned_snapshot.size() ||
             _owned_snapshot[source_index] != uint8{1})) {
            continue;
        }
        const RHIRecordingSource& source = _sources[source_index];
        const size_t              transferred_prefix =
            !_transferred_signal_prefixes.empty() &&
                    source_index < _transferred_signal_prefixes.size() ?
                std::min(
                    _transferred_signal_prefixes[source_index],
                    source.submit_metadata.signal_fences.size()
                ) :
                0;
        for (size_t signal_index = transferred_prefix;
             signal_index < source.submit_metadata.signal_fences.size();
             ++signal_index) {
            const RHIRecordingFencePoint& point =
                source.submit_metadata.signal_fences[signal_index];
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

bool RecordingSourceIsTerminal(
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

// Future cancellation is one transaction across the complete recording group.
// Terminal sources reject their declared signal values first, then all
// Completion/Query states publish Error, and only then may callbacks run.
// Mutable sources publish Error so Wait/Get remain bounded during shutdown,
// but defer callback notification until their CommandList is safely owned.
void TerminalizeRecordingSourceQueries(
    Array<RHIRecordingSource>&              _sources,
    std::string_view                        _reason,
    std::span<const uint8>                  _terminal_snapshot = {},
    std::span<PreparedCommandListRejection> _prepared          = {}
) noexcept {
    // Status must be sampled exactly once. A producer may cross Pending ->
    // terminal after this snapshot; treating that source as terminal only in
    // the notification pass could run its Query callbacks before its still
    // opaque CommandList signals have been rejected.
    Array<uint8> local_terminal_snapshot{};
    if (_terminal_snapshot.size() != _sources.size()) {
        try {
            local_terminal_snapshot.resize(
                _sources.size(), uint8{0}
            );
            for (size_t source_index = 0;
                 source_index < _sources.size();
                 ++source_index) {
                local_terminal_snapshot[source_index] =
                    RecordingSourceIsTerminal(
                        _sources[source_index]
                    ) ?
                        uint8{1} :
                        uint8{0};
            }
            _terminal_snapshot = local_terminal_snapshot;
        } catch (...) {
            // Allocation failure is fail-closed: publish every Error below,
            // but release no callback until a later ownership boundary.
            _terminal_snapshot = {};
        }
    }
    const bool snapshot_available =
        _terminal_snapshot.size() == _sources.size();

    const GpuCompletionPublishBatch completion_batch =
        GpuCompletionBackendAccess::BeginPublishBatch();
    for (RHIRecordingSource& source : _sources) {
        (void)source.gpu_completion_cancellation.PublishCancellation(_reason, completion_batch);
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->PublishPendingGpuCompletionErrors(
                _reason, completion_batch
            );
        }
    }

    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (RHIRecordingSource& source : _sources) {
        (void)source.query_cancellation.PublishCancellation(
            _reason, query_batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->PublishPendingQueryErrors(
                _reason, query_batch
            );
        }
    }
    if (snapshot_available) {
        for (size_t source_index = 0; source_index < _sources.size();
             ++source_index) {
            if (_terminal_snapshot[source_index] != 0) {
                _sources[source_index].gpu_completion_cancellation.NotifyCancellation(
                    completion_batch
                );
                _sources[source_index].gpu_completion_cancellation.NotifyPublishedCancellation();
            }
        }
        for (PreparedCommandListRejection& source : _prepared) {
            if (source.payload) {
                source.payload->NotifyPendingGpuCompletions(
                    completion_batch
                );
            }
        }
        for (size_t source_index = 0; source_index < _sources.size();
             ++source_index) {
            if (_terminal_snapshot[source_index] == 0) {
                continue;
            }
            _sources[source_index].query_cancellation.NotifyCancellation(
                query_batch
            );
            _sources[source_index].query_cancellation.NotifyPublishedCancellation();
        }
        for (PreparedCommandListRejection& source : _prepared) {
            if (source.payload) {
                source.payload->NotifyPendingQueries(query_batch);
            }
        }
    }
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
    Array<uint8>&                        _terminal_snapshot
) noexcept {
    // A source protected by an incomplete recording gate must remain opaque:
    // even reading IsEmpty(), let alone draining callbacks, races its producer.
    // A terminal gate, however, is the ownership handoff point and its ordinary
    // callbacks still own CPU/resource cleanup even when shutdown cancels the
    // FIFO before the source can be materialized. Success-only callbacks must
    // never run because no GPU submission was published.
    assert(
        _rejection_preparations.size() == _sources.size() &&
        _prepared.size() == _sources.size() &&
        _terminal_snapshot.size() == _sources.size()
    );
    std::fill(
        _terminal_snapshot.begin(),
        _terminal_snapshot.end(),
        uint8{0}
    );
    size_t terminal_source_count  = 0;
    size_t cleanup_callback_count = 0;
    // Publish host Errors for the complete group before invoking any
    // Fence::Reject hook. The all-zero snapshot intentionally releases no
    // callback: ownership-complete sources are discovered and detached by the
    // fixed-point loop below, while opaque sources remain callback-deferred.
    TerminalizeRecordingSourceQueries(
        _sources, _reason, _terminal_snapshot, _prepared
    );
    const auto prepare_newly_terminal_sources = [&]() {
        bool prepared_any = false;
        for (size_t source_index = 0;
             source_index < _sources.size();
             ++source_index) {
            if (_terminal_snapshot[source_index] != 0) {
                continue;
            }
            RHIRecordingSource& source = _sources[source_index];
            if (!RecordingSourceIsTerminal(source)) {
                continue;
            }

            _terminal_snapshot[source_index] = uint8{1};
            ++terminal_source_count;
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
            cleanup_callback_count +=
                _prepared[source_index].payload->callbacks.size();
        }
        return prepared_any;
    };

    // Sanitize every ownership-complete frontier before its metadata or
    // payload Fence rejection. A rejection hook may synchronously complete
    // another producer/commit gate; the next iteration detaches that newly
    // owned generation before touching any of its re-entrant observers.
    while (prepare_newly_terminal_sources()) {
        RejectRecordingSignalMetadata(
            _sources, _terminal_snapshot
        );
        for (size_t source_index = 0;
             source_index < _prepared.size();
             ++source_index) {
            if (_terminal_snapshot[source_index] != uint8{1}) {
                continue;
            }
            if (_prepared[source_index].payload) {
                _prepared[source_index].payload->RejectPendingSignals();
            }
            _terminal_snapshot[source_index] = uint8{2};
        }
    }

    // Sources still absent from the fixed point remain opaque and receive
    // Error publication without callback release.
    const size_t opaque_source_count =
        _sources.size() - terminal_source_count;
    TerminalizeRecordingSourceQueries(
        _sources, _reason, _terminal_snapshot, _prepared
    );
    for (PreparedCommandListRejection& source : _prepared) {
        if (!source.payload) {
            continue;
        }
        InvokeCleanupCallbacks(
            source.payload->callbacks,
            "[RHIExecutor] cancelled recording"
        );
        source.payload->success_callbacks.clear();
    }

    try {
        LOG_WARNING(
            "[RHIExecutor] rejected {} recording source(s): {}; "
            "terminal_sources={} opaque_sources={} cleanup_callbacks={}",
            _sources.size(),
            _reason,
            terminal_source_count,
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
    Array<uint8>&                                      _terminal_snapshot,
    Array<RHISubmissionRequest>                        _materialized_submits,
    std::span<const size_t>                            _metadata_signal_transfer_prefixes
) noexcept {
    // Reject is emitted only after every producer gate is terminal. This is
    // the ownership point at which failed/partial CommandLists are immutable
    // and may be destructively drained for their ordinary cleanup callbacks.
    // DrainOrdinaryCallbacksForRejection produces no CmdSubmit, drops success
    // callbacks, and guarantees that no partial commands reach a backend.
    const bool all_terminal = std::all_of(
        _sources.begin(),
        _sources.end(),
        [](const RHIRecordingSource& _source) {
            return RecordingSourceIsTerminal(_source);
        }
    );
    if (!all_terminal) {
        try {
            LOG_ERROR(
                "[RHIExecutor] failed recording group resolved before every producer became terminal"
            );
        } catch (...) {
        }
        // Materialization is entered only after the handoff queue observes
        // every prerequisite terminal. Keep this invariant explicit: an
        // immutable prefix combined with an opaque producer cannot be safely
        // notified as one transaction.
        assert(_materialized_submits.empty());
        FinalizeCancelledRecordingSources(
            std::move(_sources),
            "failed recording group still has a mutable producer",
            _rejection_preparations,
            _prepared,
            _terminal_snapshot
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
        _terminal_snapshot.size() == _sources.size()
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
    for (RHISubmissionRequest& entry : _materialized_submits) {
        const GpuCompletionPublishBatch entry_batch =
            entry.submit.gpu_completion_publish_batch.Valid() ?
                entry.submit.gpu_completion_publish_batch :
                completion_batch;
        entry.submit.PublishPendingGpuCompletionErrors(
            _reason, entry_batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->PublishPendingGpuCompletionErrors(
                _reason, completion_batch
            );
        }
    }

    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    for (RHIRecordingSource& source : _sources) {
        (void)source.query_cancellation.PublishCancellation(
            _reason, query_batch
        );
    }
    for (RHISubmissionRequest& entry : _materialized_submits) {
        const QueryPublishBatch entry_batch =
            entry.submit.query_publish_batch.Valid() ?
                entry.submit.query_publish_batch :
                query_batch;
        entry.submit.PublishPendingQueryErrors(_reason, entry_batch);
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->PublishPendingQueryErrors(
                _reason, query_batch
            );
        }
    }

    // Batch phase B: every sibling has installed a fresh generation and every
    // host observation is terminal before synchronous Fence rejection. A
    // prefix may already be an immutable CmdSubmit; its signals remain owned
    // here and must be rejected before callback notification.
    RejectRecordingSignalMetadata(
        _sources, {}, _metadata_signal_transfer_prefixes
    );
    for (RHISubmissionRequest& entry : _materialized_submits) {
        entry.submit.RejectPendingSignals();
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->RejectPendingSignals();
        }
    }

    for (RHIRecordingSource& source : _sources) {
        source.gpu_completion_cancellation.NotifyCancellation(
            completion_batch
        );
        source.gpu_completion_cancellation.NotifyPublishedCancellation();
    }
    for (RHISubmissionRequest& entry : _materialized_submits) {
        entry.submit.NotifyPendingGpuCompletions(
            entry.submit.gpu_completion_publish_batch.Valid() ?
                entry.submit.gpu_completion_publish_batch :
                completion_batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->NotifyPendingGpuCompletions(
                completion_batch
            );
        }
    }
    for (RHIRecordingSource& source : _sources) {
        source.query_cancellation.NotifyCancellation(query_batch);
        source.query_cancellation.NotifyPublishedCancellation();
    }
    for (RHISubmissionRequest& entry : _materialized_submits) {
        entry.submit.NotifyPendingQueries(
            entry.submit.query_publish_batch.Valid() ?
                entry.submit.query_publish_batch :
                query_batch
        );
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (source.payload) {
            source.payload->NotifyPendingQueries(query_batch);
        }
    }

    for (RHISubmissionRequest& entry : _materialized_submits) {
        cleanup_callback_count += entry.submit.callbacks.size();
        InvokeCleanupCallbacks(
            entry.submit.callbacks,
            "[RHIExecutor] rejected materialized recording"
        );
        entry.submit.success_callbacks.clear();
    }
    for (PreparedCommandListRejection& source : _prepared) {
        if (!source.payload) {
            continue;
        }
        InvokeCleanupCallbacks(
            source.payload->callbacks,
            "[RHIExecutor] rejected recording"
        );
        source.payload->success_callbacks.clear();
    }
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
