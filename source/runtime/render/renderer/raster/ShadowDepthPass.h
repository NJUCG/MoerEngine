#pragma once
#include "GeometryPass.h"
#include "RasterConfig.h"
#include "math/Function.h"
#include "shader/ShaderPipeline.h"

namespace Moer {
class DirectionalLightComponent;
class PointLightComponent;
} // namespace Moer

namespace Moer::Render::Raster {

struct RasterContext;
class ShadowDepthPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(ShadowDepthPassPipeline);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(GeometryPassBindlessParam, param);
    DEFINE_SHADER_ARGS(bdls, param);

    MUTATION_BOOL(SHADOW_DEPTH_PASS);
    MUTATION_SET(MutationSet, SHADOW_DEPTH_PASS);
};

class ShadowDepthPass {
public:
    ShadowDepthPass(RasterContext& context);

    void Process(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera);

    // 资源管理
    void PrepareCSMResources(RasterContext& context, const RasterConfig& ui_config);
    void PreparePointShadowResources(RasterContext& context, const RasterConfig& ui_config);

    // 辅助函数
    static DirectionalLightComponent* GetMainLightDirection(RasterContext& context);
    static PointLightComponent*       GetMainPointLight(RasterContext& context);

    // 渲染逻辑

    void RenderCSM(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera);
    void RenderPointShadows(RasterContext& context, const RasterConfig& config);

private:
    void RenderShadow(
        RasterContext&   context,
        const float4x4&  view_proj,
        const Rect2D&    rect,
        TextureView      depth_attachment,
        std::string_view pass_name
    );

private:
    StaticArray<std::string, CSM_MAX_CASCADES> shadow_depth_pass_names;
    uint                                       enabled_cascade_layers;

    Moer::UnorderedMap<VertexFactory, ShadowDepthPassPipeline> pipeline_map;
    VertexShader                                               vertex_shader;
};
} // namespace Moer::Render::Raster