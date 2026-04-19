#ifndef MOER_ENGINE_VULKAN_SUBMISSION_EXECUTOR_H
#define MOER_ENGINE_VULKAN_SUBMISSION_EXECUTOR_H

#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"
#include <optional>
#include <utility>

namespace Moer::Render {

class VkCommandQueue;
class VkCopyQueue;

struct VulkanSubmissionBatchSubmit {
    EQueueType queue{EQueueType::Ignore};
    CmdSubmit  submit;

    VulkanSubmissionBatchSubmit(EQueueType in_queue, CmdSubmit&& in_submit) :
        queue(in_queue),
        submit(std::move(in_submit)) {}

    VulkanSubmissionBatchSubmit(VulkanSubmissionBatchSubmit&&) noexcept            = default;
    VulkanSubmissionBatchSubmit& operator=(VulkanSubmissionBatchSubmit&&) noexcept = default;
    VulkanSubmissionBatchSubmit(const VulkanSubmissionBatchSubmit&)                = delete;
    VulkanSubmissionBatchSubmit& operator=(const VulkanSubmissionBatchSubmit&)     = delete;
};

struct VulkanSubmissionBatch {
    Array<VulkanSubmissionBatchSubmit> submits{};
    std::optional<RHIPresentRequest>   present{};
};

class RENDER_API VulkanSubmissionExecutor {
public:
    static void Enqueue(VulkanSubmissionBatch&& batch);
    static GraphEventRef Sync(ERHISyncDepth depth = ERHISyncDepth::RHI);
    static void Flush(ERHIFlushDepth depth = ERHIFlushDepth::SubmitGPU);
    static void Shutdown();
};

} // namespace Moer::Render

#endif
