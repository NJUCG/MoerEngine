#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanQueueInternal.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHIThreadHeartbeat.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#endif

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace Moer::Render {

namespace {
ERHIHeartbeatDomain HeartbeatDomainForQueue(EQueueType _type) noexcept {
    switch (_type) {
        case EQueueType::Graphics:
            return ERHIHeartbeatDomain::Graphics;
        case EQueueType::Compute:
            return ERHIHeartbeatDomain::Compute;
        case EQueueType::Copy:
            return ERHIHeartbeatDomain::Copy;
        default:
            return ERHIHeartbeatDomain::General;
    }
}

} // namespace

VulkanBatchCompletionGroup::VulkanBatchCompletionGroup(size_t _participant_count) :
    participants(_participant_count),
    remaining(_participant_count) {
    if (_participant_count == 0) {
        throw std::invalid_argument("Vulkan batch Completion group requires a participant");
    }
}

void VulkanBatchCompletionGroup::Arrive(size_t _participant_index, Participant&& _participant) noexcept {
    Array<std::optional<Participant>> ready{};
    {
        std::lock_guard lock(mutex);
        if (released || _participant_index >= participants.size() || participants[_participant_index].has_value() ||
            remaining == 0) {
            // A duplicate or invalid arrival means one ownership packet was
            // replayed or lost. Continuing could release user callbacks while
            // a sibling is still non-terminal.
            std::terminate();
        }
        participants[_participant_index].emplace(std::move(_participant));
        --remaining;
        if (remaining != 0) {
            return;
        }
        released = true;
        ready    = std::move(participants);
    }

    // Every participant has already published its Fence, Completion and Query
    // terminal state. Release Future callbacks across the whole batch before
    // either ordinary tier, and keep resources alive until user code returns.
    // The last arriver is itself a Completion owner.
    for (auto& slot : ready) {
        Participant& participant = *slot;
        if (!participant.gpu_completion_tokens.empty()) {
            GpuCompletionBackendAccess::NotifyTerminals(
                participant.gpu_completion_tokens, participant.gpu_completion_batch
            );
            participant.gpu_completion_tokens.clear();
        }
    }
    for (auto& slot : ready) {
        Participant& participant = *slot;
        if (!participant.query_tokens.empty()) {
            QueryBackendAccess::NotifyTerminals(participant.query_tokens, participant.query_batch);
            participant.query_tokens.clear();
        }
    }
    for (auto& slot : ready) {
        InvokeCallbacksNoexcept(slot->callbacks, "[VulkanBatchCompletion] runtime completion");
    }
    for (auto& slot : ready) {
        if (!slot->gpu_success) {
            continue;
        }
        InvokeCallbacksNoexcept(slot->success_callbacks, "[VulkanBatchCompletion] runtime success completion");
    }
    for (auto& slot : ready) {
        Participant& participant = *slot;
        if (participant.deferred_releases.empty()) {
            continue;
        }
        if (participant.device == nullptr) {
            std::terminate();
        }
        if (participant.gpu_success || participant.release_safe) {
            for (auto* resource : participant.deferred_releases) {
                MoerDelete(resource);
            }
        } else {
            for (auto* resource : participant.deferred_releases) {
                participant.device->EnqueueDeferredRelease(resource);
            }
        }
    }

    ready.clear(); // Release retained owners before publishing settlement.
    {
        std::lock_guard lock(mutex);
        if (!released || settled) {
            std::terminate();
        }
        settled = true;
    }
    settled_cv.notify_all();
}

void VulkanBatchCompletionGroup::WaitUntilSettled() {
    std::unique_lock lock(mutex);
    settled_cv.wait(lock, [this] {
        return settled;
    });
}

void VkCommandQueue::CompletionThreadMain() {
    Platform::SetCurrentThreadName(
        queue.GetType() == EQueueType::Graphics ? "Vulkan Gfx Completion" : "Vulkan Compute Completion"
    );
    RHIThreadRoleScope      owner_scope(ERHIThreadRole::Completion);
    RHIThreadHeartbeatScope heartbeat(
        ERHIThreadRole::Completion, HeartbeatDomainForQueue(queue.GetType()), ERHIHeartbeatStage::PollCompletion
    );

    while (true) {
        heartbeat.Pulse(ERHIHeartbeatStage::PollCompletion);
        std::optional<CompletionEvent> completion;
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            RHIThreadHeartbeatWaitLock   wait_lock(lock, heartbeat, ERHIHeartbeatStage::PollCompletion);
            queue_cv.wait(wait_lock, [this]() {
                return !completion_worker_running || !event_queue.empty();
            });
            if (!completion_worker_running && event_queue.empty()) {
                heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
                break;
            }
            if (event_queue.empty()) {
                continue;
            }

            completion.emplace(std::move(event_queue.front()));
            event_queue.pop_front();
        }

        heartbeat.Pulse(ERHIHeartbeatStage::AwaitCompletion);
        ProcessCompletion(*completion);
    }
}

VkCommandQueue::SubmissionCompletionOutcome
VkCommandQueue::ResolveGpuCompletion(VulkanSubmitCompletionBatch& _batch, uint64 _timeline) {
    bool batch_gpu_success  = false;
    bool batch_release_safe = !_batch.gpu_submitted && (_batch.outcome.status == EVulkanOperationStatus::Retry ||
                                                        _batch.outcome.status == EVulkanOperationStatus::Recreate);

    VkResult completion_result = _batch.outcome.result;
    if (completion_result == VK_SUCCESS && !_batch.gpu_submitted) {
        completion_result = VK_ERROR_UNKNOWN;
    }
    const bool recoverable_rejection = !_batch.gpu_submitted &&
                                       _batch.outcome.status == EVulkanOperationStatus::Rejected &&
                                       !vk_device.IsFaulted() && _batch.outcome.result != VK_ERROR_DEVICE_LOST;

    if (_batch.gpu_submitted) {
        if (timeline->Wait(_timeline) && !vk_device.IsFaulted()) {
            batch_gpu_success  = true;
            batch_release_safe = true;
            completion_result  = VK_SUCCESS;

            for (auto& command_context : _batch.command_contexts.submitted) {
                const VkResult query_result = command_context->PrepareQueriesAfterGpuCompletion(_batch.context);
                if (query_result != VK_SUCCESS) {
                    completion_result  = query_result;
                    batch_gpu_success  = false;
                    batch_release_safe = false;
                    break;
                }
            }
            if (batch_gpu_success && vk_device.IsFaulted()) {
                completion_result  = vk_device.GetFirstFaultResult();
                batch_gpu_success  = false;
                batch_release_safe = false;
            }

            if (!batch_gpu_success && _batch.owns_queue_timeline) {
                timeline->Fail(completion_result);
            }
        } else {
            // Wait failed or the device faulted; read the recorded error.
            completion_result = timeline->QueryGpuCompletion(_timeline);
            if (_batch.owns_queue_timeline) {
                timeline->Fail(completion_result);
            }
        }
    } else if (_batch.owns_queue_timeline) {
        if (recoverable_rejection) {
            timeline->Reject(_timeline);
        } else {
            timeline->Fail(completion_result);
        }
    }

    return {batch_gpu_success, batch_release_safe, completion_result, recoverable_rejection};
}

void VkCommandQueue::RecycleCommandContexts(VulkanCommandContextBatch& _contexts, bool recycle_batch) {
    if (recycle_batch) {
        for (auto& command_context : _contexts.submitted) {
            recycle_batch = command_context->Reset() && recycle_batch;
        }
    }
    if (recycle_batch && !vk_device.IsFaulted()) {
        for (auto& command_context : _contexts.submitted) {
            command_contexts.Push(command_context.release());
        }
    } else {
        for (auto& command_context : _contexts.submitted) {
            if (!command_context) {
                continue;
            }
            vk_device.RecordAllocatorQuarantine();
            vk_device.RecordSkippedCommandPoolReset();
            command_context_quarantine.emplace_back(std::move(command_context));
        }
    }
    for (auto& command_context : _contexts.abandoned) {
        if (!vk_device.IsFaulted() && command_context->ResetAbandoned()) {
            command_contexts.Push(command_context.release());
        } else if (command_context) {
            vk_device.RecordAllocatorQuarantine();
            vk_device.RecordSkippedCommandPoolReset();
            command_context_quarantine.emplace_back(std::move(command_context));
        }
    }
}

void VkCommandQueue::ProcessCompletion(CompletionEvent& _completion) {
    const uint64 event_timeline = _completion.timeline;
    {
        VulkanSubmitCompletionBatch&      batch   = _completion.batch;
        const SubmissionCompletionOutcome outcome = ResolveGpuCompletion(batch, event_timeline);

        for (const SignalEvent& event : batch.submit.signal_events) {
            auto* fence = reinterpret_cast<VulkanTimeline*>(event.timeline_handle);
            if (fence == nullptr) {
                continue;
            }
            if (outcome.gpu_success) {
                fence->RecordGpuCompletion(event.value);
            } else if (outcome.recoverable_rejection) {
                fence->Reject(event.value);
            } else {
                fence->Fail(outcome.result);
            }
        }

        const QueryPublishBatch query_batch          = batch.submit.query_publish_batch.Valid() ?
                                                           batch.submit.query_publish_batch :
                                                           QueryBackendAccess::BeginPublishBatch();
        const std::string_view  query_failure_reason = outcome.gpu_success ?
                                                           "Vulkan query did not resolve after GPU completion" :
                                                           "Vulkan submission did not reach successful GPU completion";
        for (auto& command_context : batch.command_contexts.submitted) {
            command_context->PublishQueriesAfterGpuCompletion(query_batch, outcome.gpu_success, query_failure_reason);
        }
        for (auto& command_context : batch.command_contexts.abandoned) {
            command_context->PublishQueriesAfterGpuCompletion(
                query_batch, false, "Vulkan query recorder was abandoned before native submission"
            );
        }
        // Every CmdSubmit token must become terminal even if a
        // backend bug failed to associate it with a native
        // command context record. Publish the fallback in the same
        // transaction, but defer all notifications.
        batch.submit.PublishPendingQueryErrors(query_failure_reason, query_batch);

        const GpuCompletionPublishBatch gpu_completion_batch = batch.submit.gpu_completion_publish_batch.Valid() ?
                                                                   batch.submit.gpu_completion_publish_batch :
                                                                   GpuCompletionBackendAccess::BeginPublishBatch();
        batch.submit.PublishPendingGpuCompletionOutcome(
            outcome.gpu_success, "Vulkan submission did not reach successful GPU completion", gpu_completion_batch
        );

        bool recycle_batch = outcome.gpu_success && !vk_device.IsFaulted();
        if (recycle_batch) {
            for (auto& command_context : batch.command_contexts.submitted) {
                recycle_batch = command_context->CompleteSuccessCallbacks() && recycle_batch;
            }
        }

        if (batch.descriptor_lease) {
            // A failed completion wait does not prove the GPU stopped
            // reading. Keep those ranges unavailable until heap teardown.
            if (!batch.gpu_submitted || outcome.gpu_success) {
                vk_device.GetGlobalDescriptorHeap().RecycleDescriptorBufferLease(std::move(batch.descriptor_lease));
            }
            batch.descriptor_lease.reset();
        }

        if (!batch.gpu_submitted) {
            FinalizeRejectedBindlessUpdates(batch.submit);
        }
        Array<std::function<void()>>                           callbacks{};
        Array<std::function<void()>>                           success_callbacks{};
        std::optional<VulkanBatchCompletionGroup::Participant> grouped_completion{};
        if (batch.batch_completion.Valid()) {
            grouped_completion.emplace();
            grouped_completion->lifetime              = std::move(batch.submit.lifetime);
            grouped_completion->gpu_completion_tokens = std::move(batch.submit.gpu_completion_tokens);
            grouped_completion->gpu_completion_batch  = gpu_completion_batch;
            grouped_completion->query_tokens          = std::move(batch.submit.query_tokens);
            grouped_completion->query_batch           = query_batch;
            grouped_completion->callbacks             = std::move(batch.submit.callbacks);
            grouped_completion->success_callbacks     = std::move(batch.submit.success_callbacks);
            grouped_completion->deferred_releases     = std::move(batch.deferred_releases);
            grouped_completion->device                = &vk_device;
            grouped_completion->gpu_success           = outcome.gpu_success;
            grouped_completion->release_safe          = outcome.release_safe;
            batch.submit.gpu_completion_publish_batch = {};
            batch.submit.query_publish_batch          = {};
        } else {
            callbacks         = std::move(batch.submit.callbacks);
            success_callbacks = std::move(batch.submit.success_callbacks);
        }
        batch.submit.cmds.clear();
        batch.submit.cached_args.clear();
        batch.submit.segments.clear();
        batch.submit.wait_events.clear();
        batch.submit.signal_events.clear();
        batch.submit.debug_label.clear();

        if (!grouped_completion.has_value()) {
            batch.submit.NotifyPendingGpuCompletions(gpu_completion_batch);
            batch.submit.NotifyPendingQueries(query_batch);
            for (auto& command_context : batch.command_contexts.submitted) {
                command_context->NotifyQueriesAfterGpuCompletion(query_batch);
            }
            for (auto& command_context : batch.command_contexts.abandoned) {
                command_context->NotifyQueriesAfterGpuCompletion(query_batch);
            }
            batch.submit.query_tokens.clear();
        }

        RecycleCommandContexts(batch.command_contexts, recycle_batch);

        if (grouped_completion.has_value()) {
            VulkanBatchCompletionTicket ticket = std::move(batch.batch_completion);
            ticket.group->Arrive(ticket.participant_index, std::move(*grouped_completion));
        } else {
            InvokeCallbacksNoexcept(callbacks, "[VulkanQueue] runtime completion");
            if (outcome.gpu_success) {
                InvokeCallbacksNoexcept(success_callbacks, "[VulkanQueue] runtime success completion");
            }

            if (outcome.gpu_success || outcome.release_safe) {
                for (auto* resource : batch.deferred_releases) {
                    MoerDelete(resource);
                }
            } else {
                for (auto* resource : batch.deferred_releases) {
                    vk_device.EnqueueDeferredRelease(resource);
                }
            }
        }
        batch.deferred_releases.clear();
        callbacks.clear();
        success_callbacks.clear();
        // CompleteAll may permit device teardown as soon as the
        // retirement serial is published below.
        batch.submit.lifetime.reset();
    }

    {
        // Pair predicate updates with CompleteAll's check-and-wait lock.
        std::lock_guard lock(event_mutex);
        cpu_retired_timeline      = std::max(cpu_retired_timeline, event_timeline);
        retirement_settled_serial = std::max(retirement_settled_serial, _completion.retirement_serial);
    }
    queue_cv.notify_all();
}

void VkCommandQueue::CompleteAll(uint64 _timeline) {
    Array<std::shared_ptr<VulkanBatchCompletionGroup>> completion_groups{};
    uint64                                             target_retirement_serial = 0;
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        target_retirement_serial = retirement_enqueued_serial;
        for (const VulkanBatchCompletionSettlement& settlement : batch_completion_settlements) {
            if (settlement.retirement_serial > target_retirement_serial) {
                continue;
            }
            if (auto group = settlement.group.lock()) {
                completion_groups.emplace_back(std::move(group));
            }
        }
#if MOER_VULKAN_TEST_SUPPORT
        NotifyVulkanQueueLocalSyncWait(queue.GetType(), target_retirement_serial, completion_groups.size());
#endif
        queue_cv.wait(lock, [this, _timeline, target_retirement_serial]() {
            return cpu_retired_timeline >= _timeline && retirement_settled_serial >= target_retirement_serial;
        });
    }

    // A grouped packet can finish its queue-local retirement before the last
    // sibling owner releases the batch-wide Query/callback/deferred-release
    // tiers. External queue Sync must include that release, while Completion
    // owners remain strictly non-blocking.
    for (const auto& group : completion_groups) {
        group->WaitUntilSettled();
    }

    std::unique_lock<std::mutex> lock(event_mutex);
    std::erase_if(
        batch_completion_settlements, [target_retirement_serial](const VulkanBatchCompletionSettlement& settlement) {
            return settlement.retirement_serial <= target_retirement_serial;
        }
    );
}

} // namespace Moer::Render
