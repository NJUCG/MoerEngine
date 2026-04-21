#ifndef MOER_ENGINE_VULKAN_SUBMISSION_EXECUTOR_H
#define MOER_ENGINE_VULKAN_SUBMISSION_EXECUTOR_H

#include "rhi/RHIExecutorBackend.h"
#include "rhi/RHIIO.h"
#include <atomic>
#include <memory>

namespace Moer::Render {

class RENDER_API VulkanSubmissionExecutor final : public RHIBackendExecutor {
public:
    VulkanSubmissionExecutor();
    ~VulkanSubmissionExecutor() override;

    void Enqueue(RHIBackendSubmissionBatch&& batch) override;
    GraphEventRef Sync(ERHISyncDepth depth = ERHISyncDepth::RHI) override;
    void Flush(ERHIFlushDepth depth = ERHIFlushDepth::SubmitGPU) override;
    void ShutDown() override;

private:
    struct State;

    std::unique_ptr<State> state_{};
    std::atomic_uint64_t   executor_op_seq_base_{0};
};

} // namespace Moer::Render

#endif
