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
    RHIPresentRequest(const RHIPresentRequest&) = default;
    RHIPresentRequest& operator=(
        const RHIPresentRequest&
    ) = default;

    RHIPresentRequest(RHIPresentRequest&& _other) noexcept :
        source(_other.source) {
        swapchain.Swap(_other.swapchain);
        source_texture.Swap(_other.source_texture);
        receipt.swap(_other.receipt);
        _other.source = {};
    }

    RHIPresentRequest& operator=(
        RHIPresentRequest&& _other
    ) noexcept {
        if (this == &_other) {
            return *this;
        }
        swapchain.Swap(_other.swapchain);
        source_texture.Swap(_other.source_texture);
        receipt.swap(_other.receipt);
        std::swap(source, _other.source);
        return *this;
    }

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

// Strong, generation-aware lifecycle target for one PresentationSurface.
// The Render owner publishes this control request after all older Present
// packets. The backend freezes the matching completion frontier only when the
// sole Submission owner consumes it, so later frames and unrelated surfaces
// cannot extend the drain.
struct RHIPresentationDrainTarget {
    SwapchainRef                   swapchain{};
    PresentationCompletionStateRef completion_state{};
    uint64                         state_instance_id{0};
    uint64                         presentation_epoch{0};
    uint64                         drawable_generation{0};

    RHIPresentationDrainTarget() = default;

    RHIPresentationDrainTarget(
        SwapchainRef _swapchain,
        uint64       _presentation_epoch,
        uint64       _drawable_generation
    ) :
        swapchain(std::move(_swapchain)),
        completion_state(
            swapchain ? swapchain->GetPresentationCompletionState() :
                        PresentationCompletionStateRef{}
        ),
        state_instance_id(
            completion_state ? completion_state->GetInstanceId() : 0
        ),
        presentation_epoch(_presentation_epoch),
        drawable_generation(_drawable_generation) {}

    [[nodiscard]] bool IsValid() const noexcept {
        return swapchain && completion_state &&
               state_instance_id != 0 &&
               completion_state->GetInstanceId() == state_instance_id;
    }
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
    virtual GraphEventRef DrainPresentation(
        RHIPresentationDrainTarget _target
    ) = 0;
    virtual void          Flush(ERHIFlushDepth _depth = ERHIFlushDepth::SubmitGPU) = 0;
    // Publish non-blocking cancellation before RHIExecutor waits for
    // concurrent Sync callers. Backends without cancellable host waits may
    // keep the default no-op implementation.
    virtual void          BeginShutdown() noexcept {}
    virtual void          ShutDown() = 0;
};

} // namespace Moer::Render
