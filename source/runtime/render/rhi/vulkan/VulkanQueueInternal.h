#pragma once

#include "VulkanQueue.h"
#include "VulkanBindlessArray.h"

namespace Moer::Render {

// Shared queue implementation operations. Ownership remains with the queue,
// recorded submission and its existing completion records.
struct VulkanPresentationSourceMembershipCommit {
    BindlessArrayRef bindless_array{};
    VulkanBindlessArray::PresentationSourceMembershipSnapshot
        membership{};
};

struct VulkanPresentationSourceStateTransaction {
    Array<VulkanPresentationSourceStateDelta>       delta{};
    Array<VulkanPresentationSourceMembershipCommit> membership_commits{};
    std::unique_lock<std::mutex>                     submission_lock{};
};

void FinalizeRejectedBindlessUpdates(const CmdSubmit& _submit);

void InvokeCallbacksNoexcept(
    Array<std::function<void()>>& _callbacks,
    std::string_view              _context
) noexcept;

bool WaitForSubmittedDependencies(
    VulkanDevice&           _device,
    const Array<WaitEvent>& _wait_events,
    EQueueType               _queue,
    const std::atomic_bool* _continue_waiting = nullptr
);

VkResult GetRejectedSubmitResult(VulkanDevice& _device);

VulkanOperationResult MakeUnsubmittedOutcome(
    VkResult _result,
    bool     _recoverable
);

void MarkSubmissionAccepted(
    VulkanTimeline* _queue_timeline, uint64 _timeline, const Array<SignalEvent>& _signal_events
);

VulkanPresentationSourceStateProgram
BuildPresentationSourceStateProgram(
    const CmdSubmit& _submit,
    EQueueType       _queue
);

VulkanPresentationSourceStateTransaction
EvaluatePresentationSourceStateProgram(
    const VulkanPresentationSourceStateProgram& _program
);

void CommitAcceptedPresentationSourceStateTransaction(
    VulkanPresentationSourceStateTransaction& _transaction
) noexcept;

void TerminalizeUnsubmittedSignal(
    VulkanDevice&                _device,
    VulkanTimeline*                 _fence,
    uint64                       _value,
    const VulkanOperationResult& _outcome
);

void TerminalizeUnsubmittedSignals(
    VulkanDevice&                _device,
    const Array<SignalEvent>&    _signal_events,
    const VulkanOperationResult& _outcome
);

QueryPublishBatch PublishUnsubmittedQueryErrors(
    CmdSubmit&                 _submit,
    VulkanCommandContextBatch* _command_contexts,
    std::string_view           _reason
) noexcept;

[[nodiscard]] bool SupportsVulkanCompiledCommands(const CmdSubmit& submit, EQueueType queue);

void RecordVulkanCopyCommands(
    VulkanDevice& device, VulkanCommandContext& vk_command_context,
    CmdSubmit& submit, bool suppress_timestamp_queries
);

} // namespace Moer::Render
