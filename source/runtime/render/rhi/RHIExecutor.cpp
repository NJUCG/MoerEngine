#include "rhi/RHICommand.h"
#include "rhi/RHI.h"
#include "Core.h"
#include "taskgraph/GraphTask.h"
#include "vulkan/VulkanSubmissionExecutor.h"

namespace Moer::Render {

RHIExecutor& RHIExecutor::Get() {
    static RHIExecutor executor;
    return executor;
}

void RHIExecutor::Submit(
    Array<CommandList>&& command_lists,
    ERHIExecSubmitFlags  flags,
    RHIPresentRequest*   present
) {
    if (present != nullptr && !present->swapchain) {
        LOG_ERROR("RHIExecutor::Submit got a null swapchain in RHIPresentRequest");
        assert(false && "Present request swapchain must be valid");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(submit_mutex);

        for (size_t cmd_index = 0; cmd_index < command_lists.size(); ++cmd_index) {
            auto&            command_list = command_lists[cmd_index];
            const EQueueType queue_type   = command_list.GetQueueType();
            if (queue_type != EQueueType::Graphics && queue_type != EQueueType::Compute) {
                LOG_ERROR(
                    "RHIExecutor::Submit only accepts Graphics or Compute command lists, got={}",
                    static_cast<uint32>(queue_type)
                );
                assert(false && "RHIExecutor only accepts Graphics or Compute command lists");
                return;
            }
            if (command_list.IsEmpty()) {
                continue;
            }
            pending_command_lists.emplace_back(std::move(command_list));
        }

        if (present != nullptr && present->source.texture != nullptr) {
            pending_present = *present;
        }
    }

    if (EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FlushGPU)) {
        Flush(ERHIFlushDepth::SubmitGPU);
    }
}

void RHIExecutor::Flush(ERHIFlushDepth depth) {
    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        EnqueuePendingLocked();
    }

    switch (RenderDevice::Get().GetRHIType()) {
        case ERHIType::Vulkan:
            VulkanSubmissionExecutor::Flush(depth);
            break;
        case ERHIType::D3D12:
            break;
        default:
            assert(false && "Unsupported RHI type in RHIExecutor::Flush");
            break;
    }
}

void RHIExecutor::Sync(ERHISyncDepth depth) {
    Flush(ERHIFlushDepth::SubmitGPU);

    GraphEventRef event = nullptr;
    switch (RenderDevice::Get().GetRHIType()) {
        case ERHIType::Vulkan:
            event = VulkanSubmissionExecutor::Sync(depth);
            break;
        case ERHIType::D3D12:
            break;
        default:
            assert(false && "Unsupported RHI type in RHIExecutor::Sync");
            break;
    }
    if (event) {
        event->Wait();
    }
}

void RHIExecutor::ShutDown() {
    auto& executor = Get();
    {
        std::lock_guard<std::mutex> lock(executor.submit_mutex);
        executor.EnqueuePendingLocked();
    }

    switch (RenderDevice::Get().GetRHIType()) {
        case ERHIType::Vulkan:
            VulkanSubmissionExecutor::Shutdown();
            break;
        case ERHIType::D3D12:
            break;
        default:
            assert(false && "Unsupported RHI type in RHIExecutor::ShutDown");
            break;
    }
}

void RHIExecutor::EnqueuePendingLocked() {
    VulkanSubmissionBatch batch{};
    batch.submits.reserve(pending_command_lists.size());
    bool require_present_sync_fallback = false;

    std::optional<EQueueType> last_non_empty_queue{};
    for (auto& command_list : pending_command_lists) {
        if (command_list.IsEmpty()) {
            continue;
        }
        const EQueueType queue = command_list.GetQueueType();
        last_non_empty_queue   = queue;
        batch.submits.emplace_back(queue, command_list.Submit());
    }
    pending_command_lists.clear();

    if (pending_present.has_value()) {
        if (!last_non_empty_queue.has_value()) {
            LOG_WARNING(
                "RHIExecutor::Submit got a present-only flush; using ERHISyncDepth::Present as a fallback"
            );
            require_present_sync_fallback = true;
        }
        if (last_non_empty_queue.has_value() && last_non_empty_queue.value() != EQueueType::Graphics) {
            LOG_ERROR("RHIExecutor::Submit requires the last command list before present to be Graphics");
            assert(false && "Present requires the last command list to run on the Graphics queue");
            pending_present.reset();
            return;
        }
        batch.present = pending_present.value();
        pending_present.reset();
    }

    const bool has_work = !batch.submits.empty() || batch.present.has_value();
    if (!has_work) {
        return;
    }

    if (require_present_sync_fallback) {
        GraphEventRef event = VulkanSubmissionExecutor::Sync(ERHISyncDepth::Present);
        if (event) {
            event->Wait();
        }
    }

    VulkanSubmissionExecutor::Enqueue(std::move(batch));
}

} // namespace Moer::Render
