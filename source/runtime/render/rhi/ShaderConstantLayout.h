#pragma once

#include "rhi/RHIResource.h"

namespace Moer::Render {

// Returns the upload size, or zero when no stage uses constants. SPIR-V reflection only.
RENDER_API uint ValidatePipelineConstants(const PipelineShaderInfo& shader_info, uint max_byte_size);

} // namespace Moer::Render
