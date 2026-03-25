#include "rhi/RHICommand.h"
#include "rhi/RHI.h"
#include "vulkan/VulkanSubmissionExecutor.h"

#include <iterator>

namespace Moer::Render {
namespace {

RHIExecSubmitOptions ToLegacySubmitOptions(ERHIExecSubmitFlags flags) {
    return RHIExecSubmitOptions{
        .flush_gpu = EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FlushGPU),
        .frame_end = EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FrameEnd),
    };
}

} // namespace

RHIExecutor& RHIExecutor::Get() {
    static RHIExecutor executor;
    return executor;
}

void RHIExecutor::Submit(
    Array<CommandList>&& command_lists,
    ERHIExecSubmitFlags  flags,
    RHIPresentRequest*   present
) {
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

    Array<RHIExecOp> ops;
    ops.reserve(command_lists.size() + (present != nullptr ? 1u : 0u));
    std::optional<EQueueType> last_non_empty_queue{};

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
        last_non_empty_queue = queue_type;

        RHISubmitCmdList submit_list{};
        submit_list.queue = queue_type;
        submit_list.submits.emplace_back(command_list.Submit());
        ops.emplace_back(std::move(submit_list));
    }

    if (present != nullptr && present->source.texture != nullptr) {
        if (last_non_empty_queue.has_value() && last_non_empty_queue.value() != EQueueType::Graphics) {
            LOG_ERROR("RHIExecutor::Submit requires the last command list before present to be Graphics");
            assert(false && "Present requires the last command list to run on the Graphics queue");
            return;
        }
        ops.emplace_back(RHIPresentOp{present->swapchain, present->source, EQueueType::Graphics});
    }

    Submit(std::move(ops), ToLegacySubmitOptions(flags));
}

void RHIExecutor::Submit(
    Array<RHISubmitCmdList>&& submit_lists,
    RHIExecSubmitOptions      options
) {
    Array<RHIExecOp> ops;
    ops.reserve(submit_lists.size());
    for (auto& submit_list : submit_lists) {
        ops.emplace_back(std::move(submit_list));
    }
    Submit(std::move(ops), options);
}

void RHIExecutor::Submit(Array<RHIExecOp>&& ops, RHIExecSubmitOptions options) {
    std::lock_guard<std::mutex> lock(submit_mutex);
    if (!ops.empty()) {
        pending_ops.insert(
            pending_ops.end(),
            std::make_move_iterator(ops.begin()),
            std::make_move_iterator(ops.end())
        );
    }
    pending_frame_end = pending_frame_end || options.frame_end;

    if (!options.flush_gpu) {
        return;
    }

    Array<RHIExecOp> flushed_ops = std::move(pending_ops);
    pending_ops.clear();
    options.frame_end = pending_frame_end;
    pending_frame_end = false;

    if (flushed_ops.empty() && !options.frame_end) {
        return;
    }

    VulkanSubmissionExecutor::Execute(std::move(flushed_ops), options);
}

} // namespace Moer::Render
