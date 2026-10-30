#pragma once

#include "misc/Types.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {

// Validates the C++ argument layout against active SPIR-V constants.
// Also checks any backend-owned bindless index address appended to that layout.
// Returns the user upload size (excluding the address), or zero when unused.
// Throws std::invalid_argument for shader declaration/layout mismatches or device limits.
RENDER_API uint32
ValidatePipelineConstants(const PipelineShaderInfo& pipeline_shader_info, uint32 max_constant_byte_size);

} // namespace Moer::Render
