#ifndef MOER_RENDER_VULKAN_PARALLEL_RECORD_WORK_ESTIMATOR_H
#define MOER_RENDER_VULKAN_PARALLEL_RECORD_WORK_ESTIMATOR_H

#include "RenderAPI.h"
#include "rhi/RHICommand.h"

namespace Moer::Render::VulkanParallelRecordDetail {

// Estimates recorder-side CPU work. GPU payload size (for example copy bytes,
// dispatch group counts, or indirect draw counts) intentionally does not
// contribute because it does not emit additional native recording calls.
RENDER_API uint32 EstimateWorkUnits(
    const Command& _command, const TCachedArgArray& _cached_arguments
);

} // namespace Moer::Render::VulkanParallelRecordDetail

#endif // MOER_RENDER_VULKAN_PARALLEL_RECORD_WORK_ESTIMATOR_H
