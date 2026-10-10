#pragma once

#include "rhi/RHIResource.h"

namespace Moer::Render {

// Validates the C++ argument layout against active SPIR-V constants.
// Returns the full upload size, or zero when no shader stage uses constants.
// Throws std::invalid_argument for shader declaration/layout mismatches or device limits.
RENDER_API uint
ValidatePipelineConstants(const PipelineShaderInfo& pipeline_shader_info, uint max_constant_byte_size);

} // namespace Moer::Render
