#pragma once

#include "rhi/RHIResource.h"

namespace Moer::Render {

// An empty result means the stage composition is valid. These checks preserve
// the currently supported combinations; native state/device checks remain in the backend.
RENDER_API std::string_view ValidateGraphicsShaderStages(std::span<const SingleShaderInfo> shaders);
RENDER_API std::string_view ValidateComputeShaderStages(std::span<const SingleShaderInfo> shaders);

// Graphics/compute callers must validate stage uniqueness before querying.
RENDER_API const SingleShaderInfo*
FindShaderStage(std::span<const SingleShaderInfo> shaders, EShaderType shader_type);

// Retains the existing reflection merge order, independently of input list order.
RENDER_API Array<const SingleShaderInfo*>
           GetGraphicsShadersInStageOrder(std::span<const SingleShaderInfo> shaders);

} // namespace Moer::Render
