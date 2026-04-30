#pragma once
#include "GeometryPass.h"
#include "RasterConfig.h"
#include "math/Function.h"
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
    // 获取主光源实体 ID（用于获取 CNode）
    static std::optional<entt::entity> GetMainLightDirectionEntity(RasterContext& context);
    static std::optional<entt::entity> GetMainPointLightEntity(RasterContext& context);

    // 渲染逻辑

    void RenderCSM(RasterContext& context, const RasterConfig& ui_config, const Camera& camera);
    void RenderPointShadows(RasterContext& context, const RasterConfig& config, const Camera& camera);

private:
    void RenderShadow(
        RasterContext&      context,
        const RasterConfig& config,
        const float4x4&     view_proj,
        const Rect2D&       rect,
        TextureView         depth_view,
        bool                use_gpu_culling,
        std::string_view    pass_name,
        std::optional<uint> csm_profile_layer = std::nullopt
    );

private:
    uint                    enabled_cascade_layers;
    ShadowDepthPassPipeline m_pso;
    CullingPass             m_culling_pass;
};
} // namespace Moer::Render::Raster