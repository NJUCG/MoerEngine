#pragma once
#include "GeometryPass.h"
#include "RasterConfig.h"
#include "math/Function.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "shader/ShaderPipeline.h"

#include <optional>

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

    void Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera);

    // 资源管理
    void PrepareCSMResources(RasterContext& context, const RasterConfig& ui_config);
    void PreparePointShadowResources(RasterContext& context, const RasterConfig& ui_config);

    // 辅助函数
    static std::optional<ecs::CLightDirectional> GetMainLightDirection(RasterContext& context);
    static std::optional<ecs::CLightPoint>       GetMainPointLight(RasterContext& context);

    // 渲染逻辑

    void RenderCSM(RasterContext& context, const RasterConfig& ui_config, const Camera& camera);
    void RenderPointShadows(RasterContext& context, const RasterConfig& config, const Camera& camera);

private:
    bool RefreshShadowCasterBounds(RasterContext& context);

    void RenderShadow(
        RasterContext&      context,
        const RasterConfig& config,
        const float4x4&     view_proj,
        const Rect2D&       rect,
        TextureView         depth_view,
        std::string_view    pass_name,
        std::optional<uint> csm_profile_layer = std::nullopt
    );

private:
    uint                    enabled_cascade_layers;
    ShadowDepthPassPipeline m_pso;
    CullingPass             m_culling_pass;
    Array<Box3D>            m_shadow_caster_bounds;
    uint64_t                m_shadow_caster_bounds_generation = 0u;
    bool                    m_shadow_caster_bounds_valid      = false;
    bool                    m_log_cascade_bounds_next_render  = false;
    LoopedTimer             m_shadow_caster_bounds_log_timer{5.0, false};
};
} // namespace Moer::Render::Raster
