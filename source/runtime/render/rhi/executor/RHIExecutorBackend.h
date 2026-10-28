#pragma once

#include "rhi/RHICommand.h"
#include "taskgraph/GraphTask.h"

#include <optional>
#include <utility>

namespace Moer::Render {

enum class ERHIExecSubmitFlags : uint8_t {
    None     = 0,
    FlushGPU = 1 << 0,
};

constexpr bool HasRHIExecSubmitFlag(ERHIExecSubmitFlags _flags, ERHIExecSubmitFlags _flag) noexcept {
    return (static_cast<uint8_t>(_flags) & static_cast<uint8_t>(_flag)) != 0;
}

enum class ERHIFlushDepth : uint8_t {
    RHITranslate = 0,
    SubmitGPU    = 1,
};

enum class ERHISyncDepth : uint8_t {
    RHI     = 0,
    Present = 1,
};

struct RHIPresentRequest {
    SwapchainRef      swapchain{};
    TextureRef        source_texture{};
    TextureView       source{};
    PresentReceiptRef receipt{};

    RHIPresentRequest()                                    = default;
    RHIPresentRequest(const RHIPresentRequest&)            = default;
    RHIPresentRequest& operator=(const RHIPresentRequest&) = default;

    RHIPresentRequest(RHIPresentRequest&& _other) noexcept : source(_other.source) {
        swapchain.Swap(_other.swapchain);
        source_texture.Swap(_other.source_texture);
        receipt.swap(_other.receipt);
        _other.source = {};
    }

    RHIPresentRequest& operator=(RHIPresentRequest&& _other) noexcept {
        if (this == &_other) {
            return *this;
        }
        swapchain.Swap(_other.swapchain);
        source_texture.Swap(_other.source_texture);
        receipt.swap(_other.receipt);
        std::swap(source, _other.source);
        return *this;
    }

    RHIPresentRequest(SwapchainRef _swapchain, TextureView _source, PresentReceiptRef _receipt = {}) :
        swapchain(std::move(_swapchain)),
        source_texture(_source.texture),
        source(_source),
        receipt(std::move(_receipt)) {}
};

// One frontend command submission and its root logical queue. The backend may
// split its command segments into several native submissions.
struct RHISubmissionRequest {
    EQueueType queue{EQueueType::Ignore};
    CmdSubmit  submit;

    RHISubmissionRequest(EQueueType _queue, CmdSubmit&& _submit) : queue(_queue), submit(std::move(_submit)) {}

    RHISubmissionRequest(RHISubmissionRequest&&) noexcept            = default;
    RHISubmissionRequest& operator=(RHISubmissionRequest&&) noexcept = default;
    RHISubmissionRequest(const RHISubmissionRequest&)                = delete;
    RHISubmissionRequest& operator=(const RHISubmissionRequest&)     = delete;
};

// Ordered requests published together, with an optional Present and the
// topology used to schedule their native submissions.
struct RHISubmissionBatch {
    uint64                           sequence{0};
    Array<RHISubmissionRequest>      submits{};
    std::optional<RHIPresentRequest> present{};
    RHISubmissionTopologyPlan        topology{};

    RHISubmissionBatch()                                         = default;
    RHISubmissionBatch(RHISubmissionBatch&&) noexcept            = default;
    RHISubmissionBatch& operator=(RHISubmissionBatch&&) noexcept = default;
    RHISubmissionBatch(const RHISubmissionBatch&)                = delete;
    RHISubmissionBatch& operator=(const RHISubmissionBatch&)     = delete;
};

class RHIBackendExecutor {
public:
    virtual ~RHIBackendExecutor() = default;

    // RHIExecutor serializes Enqueue/Flush publication order. Sync may run
    // concurrently after its preceding batches have been published, so each
    // backend must safely linearize Sync against later Enqueue/Flush calls.
    virtual void          Enqueue(RHISubmissionBatch&& _batch)                     = 0;
    virtual GraphEventRef Sync(ERHISyncDepth _depth = ERHISyncDepth::RHI)          = 0;
    virtual void          Flush(ERHIFlushDepth _depth = ERHIFlushDepth::SubmitGPU) = 0;
    // Publish non-blocking cancellation before RHIExecutor waits for
    // concurrent Sync callers. Backends without cancellable host waits may
    // keep the default no-op implementation.
    virtual void BeginShutdown() noexcept {}
    virtual void ShutDown() = 0;
};

} // namespace Moer::Render
