#include "VulkanCommandRecorder.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanQueueInternal.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "profile/ProfileScope.h"
#include "rhi/RHIThreadOwnership.h"

#include <algorithm>
#include <mutex>

namespace Moer::Render {

void VulkanCommandQueue::CompletionThreadMain() {
    Platform::SetCurrentThreadName(
        queue.GetType() == EQueueType::Graphics ? "Vulkan Gfx Completion" : "Vulkan Compute Completion"
    );
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Completion);

    while (true) {
        std::optional<VulkanSubmissionCompletion> completion;
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            queue_cv.wait(lock, [this]() {
                return !completion_worker_running || !event_queue.empty();
            });
            if (!completion_worker_running && event_queue.empty()) {
                break;
            }
            if (event_queue.empty()) {
                continue;
            }

            completion.emplace(std::move(event_queue.front()));
            event_queue.pop_front();
        }

        MOER_PROFILE_SCOPE("Vulkan.ProcessQueueCompletion");
        ProcessCompletion(*completion);
    }
}

VkResult VulkanSubmissionCompletion::ResolveGpuCompletion(VulkanDevice& _device, VulkanTimeline& _timeline) {
    // Only skipped presentation requests have this explicit safe-release path.
    release_safe = !gpu_submitted && context.queue_type != EQueueType::Copy &&
                   (outcome.status == EVulkanOperationStatus::Retry || outcome.status == EVulkanOperationStatus::Recreate);
    VkResult result = outcome.result;
    if (result == VK_SUCCESS && !gpu_submitted) {
        result = VK_ERROR_UNKNOWN;
    }
    const bool recoverable_rejection = !gpu_submitted && outcome.status == EVulkanOperationStatus::Rejected &&
                                       !_device.IsFaulted() && result != VK_ERROR_DEVICE_LOST;
    if (gpu_submitted) {
        if (_timeline.Wait(timeline) && !_device.IsFaulted()) {
            gpu_success  = true;
            release_safe = true;
            result       = VK_SUCCESS;
            if (command_context) {
                result = command_context->PrepareQueriesAfterGpuCompletion(context);
                if (result != VK_SUCCESS) {
                    gpu_success  = false;
                    release_safe = false;
                }
            }
            if (gpu_success && _device.IsFaulted()) {
                result       = _device.GetFirstFaultResult();
                gpu_success  = false;
                release_safe = false;
            }
            if (!gpu_success && owns_queue_timeline) {
                _timeline.Fail(result);
            }
        } else {
            result = _timeline.QueryGpuCompletion(timeline);
            if (owns_queue_timeline) {
                _timeline.Fail(result);
            }
        }
    } else if (owns_queue_timeline) {
        if (recoverable_rejection) {
            _timeline.Reject(timeline);
        } else {
            _timeline.Fail(result);
        }
    }
    return result;
}

void VulkanSubmissionCompletion::PublishResults(VulkanDevice& _device, VkResult _result) {
    const bool recoverable_rejection = !gpu_submitted && outcome.status == EVulkanOperationStatus::Rejected &&
                                       !_device.IsFaulted() && _result != VK_ERROR_DEVICE_LOST;
    for (const SignalEvent& event : submit.signal_events) {
        auto* fence = reinterpret_cast<VulkanTimeline*>(event.timeline_handle);
        if (fence == nullptr) {
            continue;
        }
        if (gpu_success) {
            fence->RecordGpuCompletion(event.value);
        } else if (recoverable_rejection) {
            fence->Reject(event.value);
        } else {
            fence->Fail(_result);
        }
    }

    const QueryPublishBatch query_batch          = submit.query_publish_batch.Valid() ?
                                                       submit.query_publish_batch :
                                                       QueryBackendAccess::BeginPublishBatch();
    const std::string_view  query_failure_reason = gpu_success ?
                                                       "Vulkan query did not resolve after GPU completion" :
                                                       "Vulkan submission did not reach successful GPU completion";
    if (command_context) {
        command_context->PublishQueriesAfterGpuCompletion(query_batch, gpu_success, query_failure_reason);
    }
    submit.PublishPendingQueryErrors(query_failure_reason, query_batch);
    const GpuCompletionPublishBatch gpu_completion_batch = submit.gpu_completion_publish_batch.Valid() ?
                                                               submit.gpu_completion_publish_batch :
                                                               GpuCompletionBackendAccess::BeginPublishBatch();
    submit.PublishPendingGpuCompletionOutcome(
        gpu_success, "Vulkan submission did not reach successful GPU completion", gpu_completion_batch
    );
}

void VulkanSubmissionCompletion::Complete(VulkanDevice& _device, VulkanTimeline& _timeline) {
    PublishResults(_device, ResolveGpuCompletion(_device, _timeline));
    const QueryPublishBatch         query_batch          = submit.query_publish_batch;
    const GpuCompletionPublishBatch gpu_completion_batch = submit.gpu_completion_publish_batch;

    bool recycle_context = gpu_success && !_device.IsFaulted();
    if (recycle_context && command_context) {
        recycle_context = command_context->CompleteSuccessCallbacks();
    }
    if (descriptor_lease) {
        // A failed wait does not prove that GPU reads stopped. Unrecycled
        // descriptor ranges remain unavailable until heap teardown.
        if (!gpu_submitted || gpu_success) {
            _device.GetGlobalDescriptorHeap().RecycleDescriptorBufferLease(std::move(descriptor_lease));
        }
        descriptor_lease.reset();
    }
    if (!gpu_submitted) {
        FinalizeRejectedBindlessUpdates(submit);
    }

    Array<std::function<void()>> callbacks         = std::move(submit.callbacks);
    Array<std::function<void()>> success_callbacks = std::move(submit.success_callbacks);
    submit.cmds.clear();
    submit.cached_args.clear();
    submit.segments.clear();
    submit.wait_events.clear();
    submit.signal_events.clear();
    submit.debug_label.clear();

    submit.NotifyPendingGpuCompletions(gpu_completion_batch);
    submit.NotifyPendingQueries(query_batch);
    if (command_context) {
        command_context->NotifyQueriesAfterGpuCompletion(query_batch);
    }
    _device.GetCommandRecorder(context.queue_type).RecycleContext(std::move(command_context), gpu_submitted, recycle_context);
    InvokeCallbacksNoexcept(callbacks, "[VulkanQueue] runtime completion");
    if (gpu_success) {
        InvokeCallbacksNoexcept(success_callbacks, "[VulkanQueue] runtime success completion");
    }
    for (auto* resource : deferred_releases) {
        if (gpu_success || release_safe) {
            MoerDelete(resource);
        } else {
            _device.EnqueueDeferredRelease(resource);
        }
    }
    deferred_releases.clear();
    callbacks.clear();
    success_callbacks.clear();
    submit.lifetime.reset();
}

void VulkanCommandQueue::ProcessCompletion(VulkanSubmissionCompletion& _completion) {
    const uint64 event_timeline    = _completion.timeline;
    const uint64 retirement_serial = _completion.retirement_serial;
    _completion.Complete(vk_device, *timeline.Get());
    {
        // Pair predicate updates with CompleteAll's check-and-wait lock.
        std::lock_guard lock(event_mutex);
        cpu_retired_timeline      = std::max(cpu_retired_timeline, event_timeline);
        retirement_settled_serial = std::max(retirement_settled_serial, retirement_serial);
    }
    queue_cv.notify_all();
}

void VulkanCommandQueue::CompleteAll(uint64 _timeline) {
    std::unique_lock lock(event_mutex);
    const uint64     target_retirement_serial = retirement_enqueued_serial;
    queue_cv.wait(lock, [this, _timeline, target_retirement_serial] {
        return cpu_retired_timeline >= _timeline && retirement_settled_serial >= target_retirement_serial;
    });
}

} // namespace Moer::Render
