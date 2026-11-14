#pragma once

#include "rhi/executor/RHIExecutor.h"

#include <exception>
#include <span>
#include <string_view>

// Implementation details shared by request preparation, publication and rejection.
namespace Moer::Render::RHIExecutorDetail {

inline bool IsSubmissionQueue(EQueueType _queue) {
    return _queue == EQueueType::Graphics || _queue == EQueueType::Compute || _queue == EQueueType::Copy;
}

inline bool SubmitHasWork(const CmdSubmit& _submit) {
    return _submit.lifetime || !_submit.cmds.empty() || !_submit.callbacks.empty() ||
           !_submit.success_callbacks.empty() ||
           !_submit.gpu_completion_tokens.empty() ||
           !_submit.query_tokens.empty() ||
           !_submit.wait_events.empty() ||
           !_submit.signal_events.empty() || _submit.b_sync ||
           _submit.b_tick_profiling || _submit.b_delete_resources;
}

struct PreparedCommandListRejection {
    std::optional<CmdSubmit> payload{};
};

// Per-source cancellation progress: acquire the producer generation, reject its
// metadata and payload signals, then allow callback notification.
enum class ERecordingRejectionState : uint8 {
    ProducerOwned,
    SignalRejectionPending,
    SignalRejectionComplete,
};

void ReportExecutorFailure(
    std::string_view          _operation,
    std::string_view          _stage,
    const std::exception_ptr& _failure
) noexcept;

std::shared_ptr<RHIBackendExecutor> CreateBackendExecutor(uint32 _submission_batch_window);

void FinalizeRejectedSubmissions(
    Array<RHISubmissionRequest>&& _submits,
    std::string_view              _reason,
    RHIPresentRequest*            _present = nullptr
) noexcept;

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
) noexcept;

void FinalizeCancelledRecordingSources(
    Array<RHIRecordingSource>&& _sources,
    std::string_view            _reason,
    Array<CommandList::RecordingRejectionPreparation>&
                                         _rejection_preparations,
    Array<PreparedCommandListRejection>& _prepared,
    Array<ERecordingRejectionState>&     _rejection_states
) noexcept;

void FinalizeFailedRecordingSources(
    Array<RHIRecordingSource>&&                        _sources,
    std::string_view                                   _reason,
    Array<CommandList::RecordingRejectionPreparation>& _rejection_preparations,
    Array<PreparedCommandListRejection>&               _prepared,
    Array<ERecordingRejectionState>&                   _rejection_states,
    Array<RHISubmissionRequest>                        _materialized_submits              = {},
    std::span<const size_t>                            _metadata_signal_transfer_prefixes = {}
) noexcept;

void FinalizeRejectedBackendBatch(RHISubmissionBatch&& _batch, std::string_view _reason) noexcept;

} // namespace Moer::Render::RHIExecutorDetail
