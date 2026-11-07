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

// 提交决定的返回值：供 Executor 判断本次操作的结果，以及后续工作能否依赖它。
// completion 仅在原生提交接受工作后有效，表示一个可等待的 GPU 完成点。
// GPU 是否执行完毕、资源何时回收，由 VulkanSubmissionCompletion 继续确认和处理。
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

// 待提交请求的所有者：统一承载 Graphics、Compute、Copy 的工作描述和录制产物。
// 从预留开始，跨越 Translate 录制与 Submission 提交决定，持有资源及执行租约。
// 准备失败、提交被拒绝也沿此路径收尾；Executor 在决定确定后先公布受影响请求的失败，
// 再将资源和通知责任移交给 VulkanSubmissionCompletion，结束录制与提交阶段的占用。
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
    // Empty until recording or submission determines an outcome. Rejection
    // before vkQueueSubmit2 is also a decision, never a GPU completion.
    std::optional<VulkanOperationResult> submission_outcome;
    bool                                 recoverable_rejection{false};
    bool                                 completion_enqueued{false};
};

// 完成与回收记录：接收待提交请求交来的资源，以及结果发布、回调和递延释放责任。
// 对已接受的工作等待 GPU；对未提交的工作发布失败结果，并分别确认各项资源的回收条件。
// 原生存储满足复用条件后归还，资源保活引用则覆盖所有用户回调。
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
