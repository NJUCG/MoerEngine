#pragma once

#include "rhi/RHICommon.h"

#include <cstdint>

namespace Moer::Render::RHISubmissionPipelinePolicy {

inline constexpr uint32_t DefaultBatchWindow = 2;
inline constexpr uint32_t MinBatchWindow     = 1;
inline constexpr uint32_t MaxBatchWindow     = 8;

[[nodiscard]] constexpr uint32_t ClampBatchWindow(
    uint32_t _configured
) noexcept {
    return _configured < MinBatchWindow ? MinBatchWindow :
           _configured > MaxBatchWindow ? MaxBatchWindow :
                                          _configured;
}

[[nodiscard]] constexpr bool GraphicsComputeShareNativeLane(
    const RHIQueueTopology& _topology
) noexcept {
    return _topology.graphics.available &&
           _topology.compute.available &&
           _topology.graphics.native_queue_id ==
               _topology.compute.native_queue_id;
}

[[nodiscard]] constexpr uint32_t ResolveEffectiveBatchWindow(
    uint32_t                _configured,
    const RHIQueueTopology& _topology
) noexcept {
    return GraphicsComputeShareNativeLane(_topology) ?
               MinBatchWindow :
               ClampBatchWindow(_configured);
}

} // namespace Moer::Render::RHISubmissionPipelinePolicy
