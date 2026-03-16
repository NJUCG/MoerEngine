#ifndef MOER_ENGINE_VULKAN_SUBMISSION_EXECUTOR_H
#define MOER_ENGINE_VULKAN_SUBMISSION_EXECUTOR_H

#include "rhi/RHICommand.h"

namespace Moer::Render {

class VulkanSubmissionExecutor {
public:
    static void Execute(Array<RHIExecOp>&& ops, const RHIExecSubmitOptions& options = {});
    static void Flush();
    static void Shutdown();
};

} // namespace Moer::Render

#endif
