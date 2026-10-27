#include "rhi/executor/RHIExecutorInternal.h"

#include "log/LogSystem.h"
#include "rhi/executor/testing/RHIExecutorTestHooksInternal.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace Moer::Render {
using namespace RHIExecutorDetail;
namespace {

struct RecordingSourcePayload {
    Array<RHIRecordingSource> sources{};
    Array<CommandList::RecordingRejectionPreparation>
        rejection_preparations{};
    Array<PreparedCommandListRejection>
                        prepared_rejections{};
    Array<uint8>        terminal_snapshot{};
    ERHIExecSubmitFlags flags{ERHIExecSubmitFlags::FlushGPU};
};

void ApplyRecordingMetadata(
    CmdSubmit&                        _submit,
    const RHIRecordingSubmitMetadata& _metadata,
    size_t&                           _transferred_signal_count
) {
    if (_metadata.debug_label) {
        _submit.DebugLabel(*_metadata.debug_label, _metadata.debug_label_color);
    }
    if (_metadata.profiling_phase) {
        _submit.SetProfilingPhase(*_metadata.profiling_phase);
    }
    if (_metadata.translate_execution_class) {
        // SerialControl is a scheduling floor. Graph-declared
        // sources also carry it on the CommandList, so a
        // publisher may add a stronger metadata policy but
        // cannot weaken the immutable source frontier.
        if (_submit.translate_execution_class !=
                ERHITranslateExecutionClass::SerialControl ||
            *_metadata.translate_execution_class ==
                ERHITranslateExecutionClass::SerialControl) {
            _submit.SetTranslateExecutionClass(
                *_metadata.translate_execution_class
            );
        }
    }
    _submit.async_queue_scope = _metadata.async_queue_scope;

    auto sync_keepalive = std::make_shared<Array<FenceRef>>();
    sync_keepalive->reserve(
        _metadata.wait_fences.size() +
        _metadata.signal_fences.size()
    );
    _submit.wait_events.reserve(
        _submit.wait_events.size() +
        _metadata.wait_fences.size()
    );
    _submit.signal_events.reserve(
        _submit.signal_events.size() +
        _metadata.signal_fences.size()
    );
    _submit.signal_rejection_keepalives.reserve(
        _submit.signal_rejection_keepalives.size() +
        _metadata.signal_fences.size()
    );
    _submit.callbacks.reserve(
        _submit.callbacks.size() + 1
    );
    for (const RHIRecordingFencePoint& point :
         _metadata.wait_fences) {
        if (!point.fence.IsValid() || point.value == 0) {
            throw std::invalid_argument(
                "recording wait fence point is invalid"
            );
        }
        _submit.wait_events.emplace_back(
            uint64(point.fence.Get()), point.value
        );
        sync_keepalive->emplace_back(point.fence);
    }
    for (const RHIRecordingFencePoint& point :
         _metadata.signal_fences) {
        if (!point.fence.IsValid() || point.value == 0) {
            throw std::invalid_argument(
                "recording signal fence point is invalid"
            );
        }
        _submit.signal_rejection_keepalives.emplace_back(
            point.fence
        );
        sync_keepalive->emplace_back(point.fence);
        _submit.signal_events.emplace_back(
            uint64(point.fence.Get()), point.value
        );
        ++_transferred_signal_count;
    }
    if (!sync_keepalive->empty()) {
        // CmdSubmit callbacks are retained by the native
        // completion packet on success and rejection alike.
        _submit.callbacks.emplace_back([sync_keepalive] {});
    }
}

std::optional<Array<RHISubmissionRequest>> MaterializeRecordingSources(RecordingSourcePayload& _payload) {
    Array<RHISubmissionRequest> submits{};
    Array<size_t>               materialized_source_indices{};
    Array<size_t>               metadata_signal_transfer_prefixes{};
    try {
        submits.reserve(_payload.sources.size());
        materialized_source_indices.reserve(
            _payload.sources.size()
        );
        metadata_signal_transfer_prefixes.resize(
            _payload.sources.size(), size_t{0}
        );
        // Validate the entire immutable group before transferring the
        // first source. An unclosed query must not run rejection
        // callbacks while a later source in the transaction is still
        // Pending.
        for (const RHIRecordingSource& source : _payload.sources) {
            const EQueueType queue =
                source.command_list->GetQueueType();
            if (!IsSubmissionQueue(queue)) {
                throw std::runtime_error(
                    "recorded source changed to an invalid queue"
                );
            }
            if (source.command_list->HasOpenQueries()) {
                throw std::logic_error(
                    "recorded source contains an unclosed query"
                );
            }
        }

        // First seal every source. Only after the whole recording
        // group owns immutable CmdSubmit payloads may metadata be
        // applied. If a later metadata copy fails, the existing
        // frontend rejection path can then terminalize callbacks and
        // rollback payload for the complete materialized group.
        for (size_t source_index = 0; source_index < _payload.sources.size();
             ++source_index) {
            RHIRecordingSource& source = _payload.sources[source_index];
            const EQueueType    queue  = source.command_list->GetQueueType();
            const bool          metadata_queue_work =
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
                _payload.sources[materialized_source_indices[submit_index]];
            ApplyRecordingMetadata(
                submits[submit_index].submit,
                source.submit_metadata,
                metadata_signal_transfer_prefixes[materialized_source_indices[submit_index]]
            );
        }
    } catch (const std::exception& exception) {
        FinalizeFailedRecordingSources(
            std::move(_payload.sources),
            "recording source finalization failed",
            _payload.rejection_preparations,
            _payload.prepared_rejections,
            _payload.terminal_snapshot,
            std::move(submits),
            metadata_signal_transfer_prefixes
        );
        try {
            LOG_ERROR(
                "[RHIExecutor] failed to materialize recording sources: {}",
                exception.what()
            );
        } catch (...) {
        }
        return std::nullopt;
    } catch (...) {
        FinalizeFailedRecordingSources(
            std::move(_payload.sources),
            "recording source finalization failed",
            _payload.rejection_preparations,
            _payload.prepared_rejections,
            _payload.terminal_snapshot,
            std::move(submits),
            metadata_signal_transfer_prefixes
        );
        try {
            LOG_ERROR(
                "[RHIExecutor] failed to materialize recording sources"
            );
        } catch (...) {
        }
        return std::nullopt;
    }

    _payload.sources.clear();
    return std::move(submits);
}

} // namespace

void RHIExecutor::Submit(
    Array<CommandList>&& _command_lists,
    ERHIExecSubmitFlags  _flags,
    RHIPresentRequest*   _present
) {
    // A managed recording lease is an exclusive mutable-owner capability.
    // There is no cooperative cancellation handoff in this direct value-owned
    // API, so rejection cannot safely inspect, drain, or notify that
    // generation. Fail before capturing views or mutating any sibling; the
    // caller may release the lease and retry the intact transaction.
    if (std::any_of(
            _command_lists.begin(),
            _command_lists.end(),
            [](const CommandList& command_list) {
                return command_list.HasManagedRecordingLease();
            }
        )) {
        throw std::logic_error(
            "RHIExecutor::Submit cannot consume a CommandList group while a managed recording lease is active"
        );
    }

    Array<RHISubmissionRequest>          submits{};
    Array<GpuCompletionCancellationView> completion_cancellations{};
    Array<QueryCancellationView>         query_cancellations{};
    Array<CommandList::RecordingRejectionPreparation>
                                        rejection_preparations{};
    Array<PreparedCommandListRejection> prepared_rejections{};
    Array<size_t>                       materialized_source_indices{};
    try {
        submits.reserve(_command_lists.size());
        completion_cancellations.reserve(_command_lists.size());
        query_cancellations.reserve(_command_lists.size());
        materialized_source_indices.reserve(_command_lists.size());
        rejection_preparations.resize(_command_lists.size());
        prepared_rejections.resize(_command_lists.size());
        for (CommandList& command_list : _command_lists) {
            completion_cancellations.emplace_back(
                command_list.GetGpuCompletionCancellationView()
            );
            query_cancellations.emplace_back(
                command_list.GetQueryCancellationView()
            );
        }
    } catch (const std::exception& exception) {
        try {
            LOG_ERROR(
                "[RHIExecutor] failed to prepare CommandList group: {}",
                exception.what()
            );
        } catch (...) {
        }
        // No CommandList has been touched yet. Propagate the allocation
        // failure with the complete caller-owned transaction intact.
        throw;
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor] failed to prepare CommandList group");
        } catch (...) {
        }
        throw;
    }

    const bool group_valid = std::all_of(
        _command_lists.begin(),
        _command_lists.end(),
        [](const CommandList& command_list) {
            return IsSubmissionQueue(command_list.GetQueueType()) &&
                   !command_list.HasOpenQueries();
        }
    );
    if (!group_valid) {
        FinalizeRejectedCommandListGroup(
            _command_lists,
            completion_cancellations,
            query_cancellations,
            rejection_preparations,
            prepared_rejections,
            materialized_source_indices,
            std::move(submits),
            _present,
            "CommandList group contains an invalid queue or open query"
        );
        return;
    }

    try {
        for (size_t source_index = 0;
             source_index < _command_lists.size();
             ++source_index) {
            CommandList& command_list = _command_lists[source_index];
            if (command_list.IsEmpty()) {
                continue;
            }
            InvokeDirectCommandListMaterializationOverrideForTesting(
                source_index
            );
            const EQueueType queue = command_list.GetQueueType();
            submits.emplace_back(queue, command_list.Submit());
            materialized_source_indices.emplace_back(source_index);
        }
    } catch (const std::exception& exception) {
        try {
            LOG_ERROR(
                "[RHIExecutor] failed to materialize CommandList group: {}",
                exception.what()
            );
        } catch (...) {
        }
        FinalizeRejectedCommandListGroup(
            _command_lists,
            completion_cancellations,
            query_cancellations,
            rejection_preparations,
            prepared_rejections,
            materialized_source_indices,
            std::move(submits),
            _present,
            "CommandList group materialization failed"
        );
        return;
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor] failed to materialize CommandList group");
        } catch (...) {
        }
        FinalizeRejectedCommandListGroup(
            _command_lists,
            completion_cancellations,
            query_cancellations,
            rejection_preparations,
            prepared_rejections,
            materialized_source_indices,
            std::move(submits),
            _present,
            "CommandList group materialization failed"
        );
        return;
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

    for (RHIRecordingSource& source : _sources) {
        // Publication seals the current CommandList generation. Never trust a
        // caller-supplied valid view: it may belong to an earlier Submit() and
        // would leave the actual producer generation Pending on shutdown.
        source.gpu_completion_cancellation =
            source.command_list->GetGpuCompletionCancellationView();
        source.query_cancellation =
            source.command_list->GetQueryCancellationView();
    }

    if (_sources.empty()) {
        Array<RHISubmissionRequest> submits{};
        Submit(std::move(submits), _flags, nullptr);
        return;
    }

    Array<CommandList::RecordingRejectionPreparation>
                                        rejection_preparations{};
    Array<PreparedCommandListRejection> prepared_rejections{};
    Array<uint8>                        terminal_snapshot{};
    rejection_preparations.resize(_sources.size());
    prepared_rejections.resize(_sources.size());
    terminal_snapshot.resize(_sources.size(), uint8{0});

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
            "missing completion gate or invalid queue",
            rejection_preparations,
            prepared_rejections,
            terminal_snapshot
        );
        return;
    }

    std::vector<RHIRecordingGateRef> prerequisites{};
    prerequisites.reserve(_sources.size() * 2);
    for (const RHIRecordingSource& source : _sources) {
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

    auto payload     = std::make_shared<RecordingSourcePayload>();
    payload->sources = std::move(_sources);
    payload->rejection_preparations =
        std::move(rejection_preparations);
    payload->prepared_rejections =
        std::move(prepared_rejections);
    payload->terminal_snapshot =
        std::move(terminal_snapshot);
    payload->flags = _flags;

    try {
        RHIExecutorRecordingHandoffWork work{
            .prerequisites = std::move(prerequisites),
            .resolve       = [this, payload](
                                 ERHIRecordingHandoffResult _result
                             ) {
                if (_result == ERHIRecordingHandoffResult::Cancel) {
                    FinalizeCancelledRecordingSources(
                        std::move(payload->sources),
                        "completion gate cancelled during shutdown",
                        payload->rejection_preparations,
                        payload->prepared_rejections,
                        payload->terminal_snapshot
                    );
                    return;
                }
                if (_result == ERHIRecordingHandoffResult::Reject) {
                    FinalizeFailedRecordingSources(
                        std::move(payload->sources),
                        "recording completion gate failed",
                        payload->rejection_preparations,
                        payload->prepared_rejections,
                        payload->terminal_snapshot
                    );
                    return;
                }

                auto submits = MaterializeRecordingSources(*payload);
                if (!submits) {
                    return;
                }
                ProcessSubmissionRequests(std::move(*submits), payload->flags, nullptr);
            },
        };
        recording_handoff.EnqueueRecording(std::move(work));
    } catch (...) {
        // Work construction/queue admission is still part of preparation.
        // No producer generation has been inspected or sealed, so restore the
        // exact source transaction to the caller and propagate the failure.
        _sources = std::move(payload->sources);
        throw;
    }
}

} // namespace Moer::Render
