#include "VulkanCommandRecorder.h"
#include "VulkanCommandProfiler.h"
#include "VulkanDevice.h"
#include "VulkanQueueInternal.h"

#include <exception>

namespace Moer::Render {

VulkanCommandRecorder::VulkanCommandRecorder(VulkanDevice& _device, EQueueType _queue_type) :
    device(_device),
    queue_type(_queue_type) {
    if (queue_type != EQueueType::Copy) {
        profiler = MakeUnique<VulkanCommandProfiler>(device, queue_type);
    }
}

VulkanCommandRecorder::~VulkanCommandRecorder() {
    // Device destroys recorders only after every queue has drained Completion.
    Array<VulkanCommandContext*> recycled_contexts;
    available_contexts.PopAll(recycled_contexts);
    for (auto* context : recycled_contexts) {
        MoerDelete(context);
    }
    quarantined_contexts.clear();
}

void VulkanCommandRecorder::Record(VulkanRecordedSubmission& _pending) noexcept {
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Translate);
    assert(queue_type != EQueueType::Copy && queue_type == _pending.context.queue_type);
    assert(_pending.execution_lease.OwnsGate());
    if (_pending.submission_outcome.has_value()) {
        ResetProfiling();
        return;
    }
    try {
        RecordCommands(_pending);
    } catch (const std::exception& error) {
        RejectRecording(_pending, {EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN}, error.what());
    } catch (...) {
        RejectRecording(
            _pending, {EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN}, "unexpected command recording failure"
        );
    }
}

void VulkanCommandRecorder::ResetProfiling() noexcept {
    if (profiler) {
        profiler->ResetSplitFrame();
    }
}

ProfileData VulkanCommandRecorder::GetProfilerEntry() {
    return profiler ? profiler->GetProfilerEntry() : ProfileData{};
}

void VulkanCommandRecorder::RejectRecording(
    VulkanRecordedSubmission&    _pending,
    const VulkanOperationResult& _outcome,
    std::string_view             _reason
) noexcept {
    LogQueueErrorNoexcept(
        "[VulkanCommandRecorder] recording rejected: timeline={} reason={} VkResult={}",
        _pending.timeline,
        _reason,
        static_cast<int32>(_outcome.result)
    );
    ResetProfiling();
    _pending.submission_outcome    = _outcome;
    _pending.recoverable_rejection = _outcome.status == EVulkanOperationStatus::Rejected && !device.IsFaulted() &&
                                     _outcome.result != VK_ERROR_DEVICE_LOST;
    device.RecordRejectedSubmit();
}

UniquePtr<VulkanCommandContext> VulkanCommandRecorder::AcquireContext() {
    // Completion publishes only successfully completed and reset command contexts
    // into this pool. Pool membership is therefore the reuse gate; if
    // Completion is delayed by unrelated CPU callbacks, allocate overflow
    // storage instead of stalling the Translate owner.
    UniquePtr<VulkanCommandContext> command_context(available_contexts.Pop());
    if (command_context) {
        return command_context;
    }
    return MakeUnique<VulkanCommandContext>(&device, queue_type);
}

Array<RHIResource*> VulkanCommandRecorder::TakeDeferredReleases() {
    if (queue_type != EQueueType::Graphics) {
        return {};
    }

    Array<RHIResource*> deleted_resources;
    // Recorded and submitted commands retain their resources through
    // RHICommandLifetime until Completion finishes all callback tiers.
    device.deferred_release_queue.PopAll(deleted_resources);

    return deleted_resources;
}

void VulkanCommandRecorder::RecycleContexts(VulkanCommandContextBatch& _contexts, bool _can_recycle_submitted) {
    if (_can_recycle_submitted) {
        for (auto& command_context : _contexts.submitted) {
            _can_recycle_submitted = command_context->Reset() && _can_recycle_submitted;
        }
    }
    if (_can_recycle_submitted && !device.IsFaulted()) {
        for (auto& command_context : _contexts.submitted) {
            available_contexts.Push(command_context.release());
        }
    } else {
        for (auto& command_context : _contexts.submitted) {
            if (!command_context) {
                continue;
            }
            device.RecordAllocatorQuarantine();
            device.RecordSkippedCommandPoolReset();
            quarantined_contexts.emplace_back(std::move(command_context));
        }
    }
    for (auto& command_context : _contexts.abandoned) {
        if (!device.IsFaulted() && command_context->ResetAbandoned()) {
            available_contexts.Push(command_context.release());
        } else if (command_context) {
            device.RecordAllocatorQuarantine();
            device.RecordSkippedCommandPoolReset();
            quarantined_contexts.emplace_back(std::move(command_context));
        }
    }
}

} // namespace Moer::Render
