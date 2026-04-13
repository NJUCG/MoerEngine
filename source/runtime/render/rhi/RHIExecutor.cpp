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
    assert(Moer::IsCurrentlyGameThread() && "RHIExecutor::Submit must only be called from the main thread");

    if (EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FrameEnd) &&
        !EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FlushGPU)) {
        LOG_ERROR("RHIExecutor::Submit requires FrameEnd to be submitted together with FlushGPU");
        assert(false && "FrameEnd requires FlushGPU");
        return;
    }

    if (present != nullptr && !present->swapchain) {
        LOG_ERROR("RHIExecutor::Submit got a null swapchain in RHIPresentRequest");
        assert(false && "Present request swapchain must be valid");
        return;
    }

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
    pending_frame_end = pending_frame_end || EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FrameEnd);

    if (!EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FlushGPU)) {
        return;
    }
    FlushPendingLocked(pending_frame_end);
}

void RHIExecutor::Sync(ERHISyncDepth depth) {
    assert(Moer::IsCurrentlyGameThread() && "RHIExecutor::Sync must only be called from the main thread");

    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        FlushPendingLocked(pending_frame_end);
    }
    GraphEventRef event = VulkanSubmissionExecutor::Sync(depth);
    if (event) {
        event->Wait();
    }
}

void RHIExecutor::ShutDown() {
    auto& executor = Get();
    {
        std::lock_guard<std::mutex> lock(executor.submit_mutex);
        executor.FlushPendingLocked(executor.pending_frame_end);
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

void RHIExecutor::FlushPendingLocked(bool frame_end) {
    VulkanSubmissionBatch batch{};
    batch.submits.reserve(pending_command_lists.size());

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
            LOG_ERROR("RHIExecutor::Submit present request requires at least one Graphics command list");
            assert(false && "Present requires a preceding Graphics command list");
            pending_present.reset();
            pending_frame_end = false;
            return;
        }
        if (last_non_empty_queue.has_value() && last_non_empty_queue.value() != EQueueType::Graphics) {
            LOG_ERROR("RHIExecutor::Submit requires the last command list before present to be Graphics");
            assert(false && "Present requires the last command list to run on the Graphics queue");
            pending_present.reset();
            pending_frame_end = false;
            return;
        }
        batch.present = pending_present.value();
        pending_present.reset();
    }

    const bool has_work = !batch.submits.empty() || batch.present.has_value() || frame_end;
    pending_frame_end   = false;
    if (!has_work) {
        return;
    }

    VulkanSubmissionExecutor::Execute(
        std::move(batch),
        VulkanSubmissionExecuteOptions{.frame_end = frame_end}
    );
}

} // namespace Moer::Render
