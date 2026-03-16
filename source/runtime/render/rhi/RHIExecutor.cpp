#include "rhi/RHICommand.h"
#include "rhi/RHI.h"
#include "vulkan/VulkanSubmissionExecutor.h"

#include <iterator>

namespace Moer::Render {
namespace {

void SubmitFallback(Array<RHIExecOp>&& ops) {
    bool seen_present = false;
    for (auto& op : ops) {
        std::visit(
            [&seen_present](auto& op_value) {
                using TOp = std::decay_t<decltype(op_value)>;
                if constexpr (std::is_same_v<TOp, RHISubmitCmdList>) {
                    if (seen_present) {
                        LOG_ERROR("RHIExecutor ordering validation failed: RHISubmitCmdList appears after Present");
                        return;
                    }
                    if (op_value.queue == EQueueType::Copy) {
                        auto& copy_queue = RenderDevice::Get().GetCopyQueue();
                        for (auto& submit : op_value.submits) {
                            copy_queue.Execute(std::move(submit));
                        }
                        return;
                    }

                    auto& queue = RenderDevice::Get().GetCommandQueue(op_value.queue);
                    for (auto& submit : op_value.submits) {
                        queue.Execute(std::move(submit));
                    }
                } else {
                    seen_present = true;
                    if (!op_value.swapchain || !op_value.target.texture) {
                        return;
                    }
                    if (op_value.queue == EQueueType::Copy || op_value.queue == EQueueType::Ignore) {
                        return;
                    }
                    auto& queue = RenderDevice::Get().GetCommandQueue(op_value.queue);
                    queue.Present(op_value.swapchain, op_value.target);
                }
            },
            op
        );
    }
}

} // namespace

RHIExecutor& RHIExecutor::Get() {
    static RHIExecutor executor;
    return executor;
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

    if (RenderDevice::Get().GetRHIType() == ERHIType::Vulkan) {
        VulkanSubmissionExecutor::Execute(std::move(flushed_ops), options);
        return;
    }
    SubmitFallback(std::move(flushed_ops));
}

} // namespace Moer::Render
