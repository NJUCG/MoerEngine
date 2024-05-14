#pragma once
#include "rhi/RHICommon.h"

#include <tuple>
namespace Moer {

    class ResourceTransition {
    public:
        RENDER_API static std::tuple<ERHIAccessFlags, ERHIAccessFlags, ERHIPipelineStageFlags, ERHIPipelineStageFlags>
        GetImageTransition(ETextureLayout oldLayout, ETextureLayout new_layout);
        RENDER_API static std::tuple<ERHIAccessFlags, ERHIPipelineStageFlags>
        GetTextureTransition(ETextureStateFlags _src_state, EPassType _pass_type, bool _is_src = true);
        RENDER_API static std::tuple<ERHIAccessFlags, ERHIPipelineStageFlags>
        GetBufferTransitation(EBufferLayout layout, EPassType pass_type);
    };

};// namespace Moer
