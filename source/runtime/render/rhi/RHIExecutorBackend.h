#pragma once

#include "rhi/RHICommand.h"

namespace Moer::Render {

struct RHIBackendSubmissionBatchEntry {
    EQueueType queue{EQueueType::Ignore};
    CmdSubmit  submit;

    RHIBackendSubmissionBatchEntry(EQueueType in_queue, CmdSubmit&& in_submit) :
        queue(in_queue),
        submit(std::move(in_submit)) {}

    RHIBackendSubmissionBatchEntry(RHIBackendSubmissionBatchEntry&&) noexcept = default;
    RHIBackendSubmissionBatchEntry& operator=(RHIBackendSubmissionBatchEntry&&) noexcept = default;
    RHIBackendSubmissionBatchEntry(const RHIBackendSubmissionBatchEntry&) = delete;
    RHIBackendSubmissionBatchEntry& operator=(const RHIBackendSubmissionBatchEntry&) = delete;
};

struct RHIBackendSubmissionBatch {
    Array<RHIBackendSubmissionBatchEntry> submits{};
    std::optional<RHIPresentRequest>      present{};
};

class RHIBackendExecutor {
public:
    virtual ~RHIBackendExecutor() = default;

    virtual void Enqueue(RHIBackendSubmissionBatch&& batch) = 0;
    virtual GraphEventRef Sync(ERHISyncDepth depth = ERHISyncDepth::RHI) = 0;
    virtual void Flush(ERHIFlushDepth depth = ERHIFlushDepth::SubmitGPU) = 0;
    virtual void ShutDown() = 0;
};

} // namespace Moer::Render