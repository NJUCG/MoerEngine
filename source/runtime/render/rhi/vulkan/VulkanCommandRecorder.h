#pragma once

#include "VulkanSubmission.h"
#include "misc/LockFree.h"

namespace Moer::Render {

class VulkanCommandProfiler;

// Device-owned recording workspace for one logical queue. It has no queue
// handle or submission thread; Executor supplies the reserved submission.
class VulkanCommandRecorder {
public:
    VulkanCommandRecorder(VulkanDevice& _device, EQueueType _queue_type);
    ~VulkanCommandRecorder();
    VulkanCommandRecorder(const VulkanCommandRecorder&)            = delete;
    VulkanCommandRecorder& operator=(const VulkanCommandRecorder&) = delete;

    // Executor reserves a packet before recording. Its lease serializes this
    // workspace through the native submission decision, including rejection.
    // Preparation failures stay in the owning packet for Submission/Completion.
    void Record(VulkanRecordedSubmission& _pending) noexcept;
    void Record(VulkanRecordedCopySubmission& _pending) noexcept;

    UniquePtr<VulkanCommandContext> AcquireContext();
    // Completion calls this after publishing queries and running context callbacks.
    void RecycleContexts(VulkanCommandContextBatch& _contexts, bool _can_recycle_submitted);

    Array<RHIResource*> TakeDeferredReleases();
    void                ResetProfiling() noexcept;
    ProfileData         GetProfilerEntry();

private:
    static bool SupportsCompiledCommands(const CmdSubmit& _submit, EQueueType _queue_type);
    void   RecordCopyCommands(VulkanCommandContext& _context, CmdSubmit& _submit, bool _suppress_timestamp_queries);
    void   RecordCommands(VulkanRecordedSubmission& _pending);
    double RecordPreparedCommands(
        VulkanRecordedSubmission&                                   _pending,
        const std::shared_ptr<const VulkanDescriptorRecordingPlan>& _descriptor_plan
    );
    void RejectRecording(
        VulkanRecordedSubmission&    _pending,
        const VulkanOperationResult& _outcome,
        std::string_view             _reason
    ) noexcept;

    VulkanDevice& device;
    EQueueType    queue_type;
    // Translate acquires; Completion returns only successfully reset contexts.
    LockFreeQueueBase<VulkanCommandContext, false> available_contexts;
    Array<UniquePtr<VulkanCommandContext>>         quarantined_contexts;
    UniquePtr<VulkanCommandProfiler>               profiler;
};

} // namespace Moer::Render
