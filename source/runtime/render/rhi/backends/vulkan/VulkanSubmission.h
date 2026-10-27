#pragma once

#include "VulkanCommandContext.h"
#include "VulkanDescriptor.h"
#include "VulkanFault.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIThreadOwnership.h"

#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace Moer::Render {

// Owning handoff records shared by Recorder, Submission and Completion.
// None of these records depends on a queue wrapper or owns a worker thread.

// Immutable source-order program built before native
// recording. Bindless accesses remain symbolic until the Submission owner can
// evaluate them against the last natively accepted descriptor membership.
struct VulkanPresentationSourceStateDelta {
    TextureRef texture;
    bool       ready{false};
};

enum class EVulkanPresentationSourceStateEventType : uint8 {
    TextureState,
    BindlessAccess,
    BindlessTextureMembership,
};

struct VulkanPresentationSourceStateEvent {
    EVulkanPresentationSourceStateEventType type{EVulkanPresentationSourceStateEventType::TextureState};
    TextureRef                              texture{};
    BindlessArrayRef                        bindless_array{};
    uint                                    bindless_slot{0};
    int32                                   membership_delta{0};
    bool                                    ready{false};
};

struct VulkanPresentationSourceStateProgram {
    Array<VulkanPresentationSourceStateEvent> events{};
};

// Result returned to the upper submission runtime.  A queue timeline is only
// publishable as a dependency when vkQueueSubmit/SubmitEmpty actually accepted
// the work; retry/recreate/rejected paths deliberately carry no completion.
struct VulkanSubmissionResult {
    VulkanOperationResult    outcome{};
    std::optional<WaitEvent> completion{};
    bool                     recoverable_rejection{false};

    [[nodiscard]] bool WasSubmitted() const {
        return completion.has_value();
    }

    [[nodiscard]] bool IsHardFailure() const {
        return outcome.status == EVulkanOperationStatus::Faulted ||
               (outcome.status == EVulkanOperationStatus::Rejected && !recoverable_rejection);
    }

    [[nodiscard]] bool IsRecoverableRejection() const {
        return outcome.status == EVulkanOperationStatus::Rejected && recoverable_rejection;
    }
};

using VulkanRuntimePreCompletionCallback = void (*)(void*, const VulkanSubmissionResult&) noexcept;

struct VulkanRuntimePreCompletionHook {
    void*                              context{nullptr};
    VulkanRuntimePreCompletionCallback callback{nullptr};
};

struct VulkanCommandContextBatch {
    Array<UniquePtr<VulkanCommandContext>> submitted;
    Array<UniquePtr<VulkanCommandContext>> abandoned;
};

// A runtime backend batch may retire through several logical Completion
// owners. Query/Fence waiters used by an earlier source callback must not
// block the only owner capable of publishing a later source. Every
// executable source therefore publishes its terminal state independently,
// then arrives here without waiting. The last Completion owner releases all
// user notifications in stable source order.
class VulkanBatchCompletionGroup final {
public:
    struct Participant {
        std::shared_ptr<const RHICommandLifetime> lifetime;
        Array<GpuCompletionToken>                 gpu_completion_tokens;
        GpuCompletionPublishBatch                 gpu_completion_batch{};
        Array<QueryToken>                         query_tokens;
        QueryPublishBatch                         query_batch{};
        Array<std::function<void()>>              callbacks;
        Array<std::function<void()>>              success_callbacks;
        Array<RHIResource*>                       deferred_releases;
        VulkanDevice*                             device{nullptr};
        bool                                      gpu_success{false};
        bool                                      release_safe{false};
    };

    explicit VulkanBatchCompletionGroup(size_t _participant_count);

    VulkanBatchCompletionGroup(const VulkanBatchCompletionGroup&)            = delete;
    VulkanBatchCompletionGroup& operator=(const VulkanBatchCompletionGroup&) = delete;

    void Arrive(size_t _participant_index, Participant&& _participant) noexcept;
    void WaitUntilSettled();

private:
    std::mutex                        mutex{};
    std::condition_variable           settled_cv{};
    Array<std::optional<Participant>> participants{};
    size_t                            remaining{0};
    bool                              released{false};
    bool                              settled{false};
};

struct VulkanBatchCompletionTicket {
    std::shared_ptr<VulkanBatchCompletionGroup> group{};
    size_t                                      participant_index{std::numeric_limits<size_t>::max()};

    [[nodiscard]] bool Valid() const noexcept {
        return group != nullptr && participant_index != std::numeric_limits<size_t>::max();
    }
};

struct VulkanBatchCompletionSettlement {
    uint64                                    retirement_serial{0};
    std::weak_ptr<VulkanBatchCompletionGroup> group{};
};

// Runtime-recorded submissions cross the Submission -> Completion boundary as
// one move-only ownership packet. Keeping the command payload, command context
// retirement, descriptor lease, callbacks, signals, and deferred releases in
// one completion packet makes that handoff atomic: no exception can leave a
// partially published completion sequence behind.
struct VulkanSubmitCompletionBatch {
    VulkanSubmitCompletionBatch(
        VulkanOperationResult         _outcome,
        VulkanOperationContext        _context,
        bool                          _gpu_submitted,
        bool                          _owns_queue_timeline,
        CmdSubmit&&                   _submit,
        VulkanCommandContextBatch&&   _command_contexts,
        VulkanDescriptorBufferLease&& _descriptor_lease,
        Array<RHIResource*>&&         _deferred_releases,
        VulkanBatchCompletionTicket   _batch_completion = {}
    ) noexcept :
        outcome(_outcome),
        context(_context),
        gpu_submitted(_gpu_submitted),
        owns_queue_timeline(_owns_queue_timeline),
        submit(std::move(_submit)),
        command_contexts(std::move(_command_contexts)),
        descriptor_lease(std::move(_descriptor_lease)),
        deferred_releases(std::move(_deferred_releases)),
        batch_completion(std::move(_batch_completion)) {}

    VulkanSubmitCompletionBatch(const VulkanSubmitCompletionBatch&)                = delete;
    VulkanSubmitCompletionBatch& operator=(const VulkanSubmitCompletionBatch&)     = delete;
    VulkanSubmitCompletionBatch(VulkanSubmitCompletionBatch&&) noexcept            = default;
    VulkanSubmitCompletionBatch& operator=(VulkanSubmitCompletionBatch&&) noexcept = default;

    VulkanOperationResult       outcome;
    VulkanOperationContext      context;
    bool                        gpu_submitted{false};
    bool                        owns_queue_timeline{false};
    CmdSubmit                   submit;
    VulkanCommandContextBatch   command_contexts;
    VulkanDescriptorBufferLease descriptor_lease;
    Array<RHIResource*>         deferred_releases;
    VulkanBatchCompletionTicket batch_completion{};
};

// Owns a request from reservation through recording and native submission until
// it is handed to Completion. The packet also retains preparation failures;
// its existence alone does not mean native commands were recorded or submitted.
struct VulkanRecordedSubmission {
    VulkanRecordedSubmission(CmdSubmit&& _submit, const VulkanOperationContext& _context, uint64 _timeline) noexcept :
        submit(std::move(_submit)),
        context(_context),
        timeline(_timeline) {}

    VulkanRecordedSubmission(const VulkanRecordedSubmission&)                = delete;
    VulkanRecordedSubmission& operator=(const VulkanRecordedSubmission&)     = delete;
    VulkanRecordedSubmission(VulkanRecordedSubmission&&) noexcept            = default;
    VulkanRecordedSubmission& operator=(VulkanRecordedSubmission&&) noexcept = default;

    CmdSubmit                            submit;
    VulkanOperationContext               context;
    uint64                               timeline{0};
    Array<VulkanCommandList*>            ordered_cmd_lists;
    Array<VkSemaphoreSubmitInfo>         native_waits;
    Array<VkSemaphoreSubmitInfo>         native_signals;
    VulkanCommandContextBatch            command_contexts;
    VulkanDescriptorBufferLease          descriptor_lease;
    Array<RHIResource*>                  deferred_releases;
    VulkanBatchCompletionTicket          batch_completion{};
    VulkanPresentationSourceStateProgram presentation_source_program;
    RHITransferableOwnershipGate::Lease  execution_lease{};
    // Empty while awaiting a submission decision; a value also represents
    // rejection before vkQueueSubmit2, not just native acceptance.
    std::optional<VulkanOperationResult> submission_outcome;
    bool                                 recoverable_rejection{false};
    bool                                 completion_enqueued{false};
};

struct VulkanRecordedCopySubmission {
    explicit VulkanRecordedCopySubmission(CmdSubmit&& _submit) noexcept : submit(std::move(_submit)) {}

    VulkanRecordedCopySubmission(const VulkanRecordedCopySubmission&)                = delete;
    VulkanRecordedCopySubmission& operator=(const VulkanRecordedCopySubmission&)     = delete;
    VulkanRecordedCopySubmission(VulkanRecordedCopySubmission&&) noexcept            = default;
    VulkanRecordedCopySubmission& operator=(VulkanRecordedCopySubmission&&) noexcept = default;

    CmdSubmit                            submit;
    VulkanOperationContext               context{};
    uint64                               logical_timeline{0};
    VulkanCommandContextBatch            command_contexts{};
    VulkanBatchCompletionTicket          batch_completion{};
    VulkanPresentationSourceStateProgram presentation_source_program;
    RHITransferableOwnershipGate::Lease  execution_lease{};
    VulkanOperationResult                retirement_outcome{EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
    bool                                 recoverable_rejection{false};
    bool                                 native_submit_resolved{false};
    bool                                 completion_committed{false};
};

} // namespace Moer::Render
