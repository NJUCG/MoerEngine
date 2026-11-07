#pragma once

#include "VulkanCommandContext.h"
#include "VulkanDescriptor.h"
#include "VulkanFault.h"
#include "VulkanResourceStateValidation.h"
#include "misc/Types.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIThreadOwnership.h"

#include <optional>
#include <utility>

namespace Moer::Render {

// Reports the submission decision to Executor.
// Accepted GPU work includes a completion wait point.
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

class VulkanTimeline;

// Owns work, recording outputs and leases until the submission decision.
// Transfers cleanup and notification duties to VulkanSubmissionCompletion.
struct VulkanPendingSubmission {
    explicit VulkanPendingSubmission(
        CmdSubmit&&            _submit,
        VulkanOperationContext _context  = {},
        uint64                 _timeline = 0
    ) noexcept :
        submit(std::move(_submit)),
        context(_context),
        timeline(_timeline) {}

    CmdSubmit                           submit;
    VulkanOperationContext              context;
    uint64                              timeline{0};
    UniquePtr<VulkanCommandContext>     command_context;
    bool                                has_native_commands{false};
    Array<VkSemaphoreSubmitInfo>        native_waits;
    Array<VkSemaphoreSubmitInfo>        native_signals;
    VulkanDescriptorBufferLease         descriptor_lease;
    Array<RHIResource*>                 deferred_releases;
    VulkanResourceStateValidator::Program resource_state_program;
    RHITransferableOwnershipGate::Lease execution_lease{};
    // Set when recording or submission determines the outcome.
    std::optional<VulkanOperationResult> submission_outcome;
    bool                                 recoverable_rejection{false};
    bool                                 completion_enqueued{false};
};

// Publishes results and runs callbacks, waiting for GPU work when submitted.
// Retains resources through callbacks and releases storage when safe.
struct VulkanSubmissionCompletion {
    VulkanSubmissionCompletion(
        uint64                            _timeline,
        uint64                            _retirement_serial,
        VulkanOperationResult             _outcome,
        VulkanOperationContext            _context,
        bool                              _gpu_submitted,
        bool                              _owns_queue_timeline,
        CmdSubmit&&                       _submit,
        UniquePtr<VulkanCommandContext>&& _command_context,
        VulkanDescriptorBufferLease&&     _descriptor_lease,
        Array<RHIResource*>&&             _deferred_releases
    ) noexcept :
        timeline(_timeline),
        retirement_serial(_retirement_serial),
        outcome(_outcome),
        context(_context),
        gpu_submitted(_gpu_submitted),
        owns_queue_timeline(_owns_queue_timeline),
        submit(std::move(_submit)),
        command_context(std::move(_command_context)),
        descriptor_lease(std::move(_descriptor_lease)),
        deferred_releases(std::move(_deferred_releases)) {}

    void Complete(VulkanDevice& _device, VulkanTimeline& _timeline);

    uint64                          timeline;
    uint64                          retirement_serial;
    VulkanOperationResult           outcome;
    VulkanOperationContext          context;
    bool                            gpu_submitted{false};
    bool                            owns_queue_timeline{false};
    CmdSubmit                       submit;
    UniquePtr<VulkanCommandContext> command_context;
    VulkanDescriptorBufferLease     descriptor_lease;
    Array<RHIResource*>             deferred_releases;
    bool                            gpu_success{false};
    bool                            release_safe{false};

private:
    VkResult ResolveGpuCompletion(VulkanDevice& _device, VulkanTimeline& _timeline);
    void     PublishResults(VulkanDevice& _device, VkResult _result);
};

} // namespace Moer::Render
