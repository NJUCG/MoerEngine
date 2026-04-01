#ifndef MOER_ENGINE_VULKAN_SUBMISSION_EXECUTOR_H
#define MOER_ENGINE_VULKAN_SUBMISSION_EXECUTOR_H

#include "rhi/RHICommand.h"
#include "rhi/RHIIO.h"

namespace Moer::Render {

class VkCommandQueue;
class VkCopyQueue;
class VulkanAllocator;

class RENDER_API VulkanSubmissionExecutor {
public:
    static void Execute(Array<RHIExecOp>&& ops, const RHIExecSubmitOptions& options = {});
    static void Flush();
    static void Shutdown();
    static void EnqueueQueueCompletion(
        uint64                        op_seq,
        Array<WaitEvent>&&            wait_events,
        VkCommandQueue*               queue,
        uint64                        timeline_value,
        UniquePtr<VulkanAllocator>&&  allocator,
        Array<std::function<void()>>&& callbacks,
        Array<SignalEvent>&&          signal_events
    );
    static void EnqueueCopyQueueCompletion(
        uint64                        op_seq,
        Array<WaitEvent>&&            wait_events,
        VkCopyQueue*                  queue,
        uint64                        timeline_value,
        UniquePtr<VulkanAllocator>&&  allocator,
        Array<std::function<void()>>&& callbacks,
        Array<IOSignalEvt>&&          signal_events
    );
};

} // namespace Moer::Render

#endif
