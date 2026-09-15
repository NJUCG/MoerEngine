#pragma once
#include "GeometryPass.h"
#include "RasterConfig.h"
#include "math/Function.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "rendergraph/RenderGraph.h"
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

class PointShadowMultiviewPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(PointShadowMultiviewPipeline);
    DEFINE_SHADER_BUFFER(point_shadow_view_matrices);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(GeometryPassBindlessParam, param);
    DEFINE_SHADER_ARGS(point_shadow_view_matrices, bdls, param);

    MUTATION_BOOL(SHADOW_DEPTH_PASS);
    MUTATION_BOOL(POINT_SHADOW_MULTIVIEW);
    MUTATION_SET(MutationSet, SHADOW_DEPTH_PASS, POINT_SHADOW_MULTIVIEW);
};

class ShadowDepthPass {
public:
    struct GraphResources {
        RenderGraph::TokenHandle shadow_maps{};
    };

    struct DrawBatch {
        CullingPass::RecordParameters culling{};
        GeometryPassBindlessParam      shader{};
        Rect2D                         rect{};
        TextureView                    depth_view{};
        std::string                    pass_name{};
        std::string                    marker_name{};
        std::string                    draw_profile_scope_name{};
        PointShadowViewMatrices        multiview_matrices{};
        bool                           multiview = false;
    };

    struct RecordParameters {
        BindlessArrayRef bindless{};
        BufferView       index_buffer{};
        BufferRef        point_shadow_view_matrices{};
        bool             update_bindless_array = false;
        Array<DrawBatch> draws{};
    };

    ShadowDepthPass(RasterContext& context);

    [[nodiscard]] RecordParameters
    Prepare(RasterContext& context, const RasterConfig& ui_config, const Camera& camera);
    void Record(CommandList& cmd_list, const RecordParameters& parameters);
    RenderGraph::PreparedPassHandle AddToGraph(
        RenderGraph& graph,
        RasterContext& context,
        const RasterConfig& ui_config,
        const Camera& camera,
        GraphResources resources,
        std::span<const RenderGraph::SetupPassHandle> setup_dependencies = {}
    );

    // Legacy synchronous adapter. New graph passes call Prepare/Record directly.
    void Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera);

    // 资源管理
    [[nodiscard]] bool PrepareCSMResources(RasterContext& context, const RasterConfig& ui_config);
    [[nodiscard]] bool PreparePointShadowResources(RasterContext& context, const RasterConfig& ui_config);

    // 辅助函数
    static std::optional<ecs::CLightDirectional> GetMainLightDirection(RasterContext& context);
    static std::optional<ecs::CLightPoint>       GetMainPointLight(RasterContext& context);

private:
    bool RefreshShadowCasterBounds(RasterContext& context);

    void PrepareCSM(
        RasterContext& context, const RasterConfig& ui_config, const Camera& camera,
        RecordParameters& parameters
    );
    void PreparePointShadows(
        RasterContext& context, const RasterConfig& config, const Camera& camera,
        RecordParameters& parameters
    );
    [[nodiscard]] GeometryPassBindlessParam BuildDrawParameters(
        const RasterContext& context, const RasterConfig& config, const float4x4& world2clip,
        bool already_transposed
    ) const;
    void RecordShadowDraw(CommandList& cmd_list, const RecordParameters& parameters, const DrawBatch& batch);
    void RecordPointShadowMultiview(
        CommandList& cmd_list, const RecordParameters& parameters, const DrawBatch& batch
    );

private:
    static constexpr uint32_t k_point_shadow_view_count = 6u;
    static constexpr uint32_t k_point_shadow_view_mask  = (1u << k_point_shadow_view_count) - 1u;

    uint                    enabled_cascade_layers;
    ShadowDepthPassPipeline m_pso;
    PointShadowMultiviewPipeline m_point_shadow_multiview_pso;
    BufferRef                    m_point_shadow_view_matrices;
    bool                         m_point_shadow_multiview_supported = false;
    CullingPass             m_culling_pass;
    Array<Box3D>            m_shadow_caster_bounds;
    uint64_t                m_shadow_caster_bounds_generation = 0u;
    bool                    m_shadow_caster_bounds_valid      = false;
    bool                    m_log_cascade_bounds_next_render  = false;
    LoopedTimer             m_shadow_caster_bounds_log_timer{5.0, false};
};
} // namespace Moer::Render::Raster
