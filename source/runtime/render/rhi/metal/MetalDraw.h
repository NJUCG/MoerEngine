#pragma once

#include "rhi/metal/MetalPipeline.h"

#include <span>

namespace Moer::Render {
MetalPipelineState* ValidateSimpleDraw(const SetDrawStateCmd& draw);
void EncodeSimpleDraw(id<MTLCommandBuffer> command_buffer, const SetDrawStateCmd& draw,
    std::span<const uint> indirect_counts,
    NSMutableArray<id<MTLBuffer>>* staging_buffers,
    NSMutableArray<id<MTLTexture>>* texture_views,
    NSMutableArray<id<MTLSamplerState>>* sampler_states);
MetalPipelineState* ValidateComputeDispatch(const DispatchCmd& dispatch,
    const TCachedArgArray& cached_args);
void EncodeComputeDispatch(id<MTLCommandBuffer> command_buffer,
    NSMutableArray<id<MTLBuffer>>* staging_buffers,
    NSMutableArray<id<MTLTexture>>* texture_views,
    const DispatchCmd& dispatch, const TCachedArgArray& cached_args);
} // namespace Moer::Render
