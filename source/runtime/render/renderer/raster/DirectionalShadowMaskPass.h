// 构建延迟光照使用的屏幕空间方向光阴影因子。
#pragma once

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include "RasterResource.h"

namespace Moer::Render::Raster {

class DirectionalShadowMaskPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(DirectionalShadowMaskPassPipeline);
    DEFINE_SHADER_BUFFER(lighting_data);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(DirectionalShadowMaskPassBindlessParam, param);
    DEFINE_SHADER_ARGS(lighting_data, bdls, param);
};

class DirectionalShadowMaskPass {
public:
    struct RecordParameters {
        uint             normal_handle{0};
        uint             depth_handle{0};
        TextureRef       normal_owner{};
        DepthBufferRef   depth_owner{};
        StaticArray<DepthBufferRef, CSM_MAX_CASCADES> cascade_shadow_owners{};
        StaticArray<TextureRef, RasterContext::PointShadowData::MAX_POINT_SHADOWS>
            point_shadow_owners{};
        BufferRef        lighting_data{};
        BindlessArrayRef bindless{};
        TextureRef       output{};
        Rect2D           render_area{};
    };

    DirectionalShadowMaskPass(RasterContext& context);

    [[nodiscard]] RecordParameters Prepare(const RasterContext& context) const;
    void Record(CommandList& cmd_list, const RecordParameters& parameters);
    void Process(RasterContext& context);

private:
    DirectionalShadowMaskPassPipeline pipeline;
};
} // namespace Moer::Render::Raster
