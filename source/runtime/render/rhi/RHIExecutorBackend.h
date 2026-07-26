#pragma once

#include "rhi/RHICommand.h"
#include "taskgraph/GraphTask.h"

#include <optional>

namespace Moer::Render {

enum class ERHIExecSubmitFlags : uint8_t {
    None     = 0,
    FlushGPU = 1 << 0,
};

constexpr bool HasRHIExecSubmitFlag(
    ERHIExecSubmitFlags _flags,
    ERHIExecSubmitFlags _flag
) noexcept {
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

    RHIPresentRequest() = default;

    RHIPresentRequest(
        SwapchainRef      _swapchain,
        TextureView       _source,
        PresentReceiptRef _receipt = {}
    ) :
        swapchain(std::move(_swapchain)),
        source_texture(_source.texture),
        source(_source),
        receipt(std::move(_receipt)) {}
};

struct RHIBackendSubmissionBatchEntry {
    EQueueType queue{EQueueType::Ignore};
    CmdSubmit  submit;

    RHIBackendSubmissionBatchEntry(EQueueType _queue, CmdSubmit&& _submit) :
        queue(_queue), submit(std::move(_submit)) {}

    RHIBackendSubmissionBatchEntry(RHIBackendSubmissionBatchEntry&&) noexcept = default;
    RHIBackendSubmissionBatchEntry& operator=(RHIBackendSubmissionBatchEntry&&) noexcept = default;
    RHIBackendSubmissionBatchEntry(const RHIBackendSubmissionBatchEntry&) = delete;
    RHIBackendSubmissionBatchEntry& operator=(const RHIBackendSubmissionBatchEntry&) = delete;
};

struct RHIBackendSubmissionBatch {
    uint64                                   sequence{0};
    Array<RHIBackendSubmissionBatchEntry>    submits{};
    std::optional<RHIPresentRequest>          present{};
    RHISubmissionTopologyPlan                 topology{};

    RHIBackendSubmissionBatch() = default;
    RHIBackendSubmissionBatch(
        RHIBackendSubmissionBatch&&
    ) noexcept = default;
    RHIBackendSubmissionBatch& operator=(
        RHIBackendSubmissionBatch&&
    ) noexcept = default;
    RHIBackendSubmissionBatch(
        const RHIBackendSubmissionBatch&
    ) = delete;
    RHIBackendSubmissionBatch& operator=(
        const RHIBackendSubmissionBatch&
    ) = delete;
};

class RHIBackendExecutor {
public:
    virtual ~RHIBackendExecutor() = default;

    // RHIExecutor serializes Enqueue/Flush publication order. Sync may run
    // concurrently after its preceding batches have been published, so each
    // backend must safely linearize Sync against later Enqueue/Flush calls.
    virtual void          Enqueue(RHIBackendSubmissionBatch&& _batch) = 0;
    virtual GraphEventRef Sync(ERHISyncDepth _depth = ERHISyncDepth::RHI) = 0;
    virtual void          Flush(ERHIFlushDepth _depth = ERHIFlushDepth::SubmitGPU) = 0;
    // Publish non-blocking cancellation before RHIExecutor waits for
    // concurrent Sync callers. Backends without cancellable host waits may
    // keep the default no-op implementation.
    virtual void          BeginShutdown() noexcept {}
    virtual void          ShutDown() = 0;
};

} // namespace Moer::Render
