#pragma once

#include "GeometryPass.h"
#include "GaussianShadowPass.h"
#include "RasterConfig.h"
#include "math/Function.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "misc/Types.h"
#include "rendergraph/RenderGraph.h"
#include "shader/ShaderPipeline.h"

#include <memory>
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
    DEFINE_SHADER_CONSTANT(GeometryPassBindlessParam, param);
    DEFINE_SHADER_ARGS(bdls, param);

    MUTATION_BOOL(SHADOW_DEPTH_PASS);
    MUTATION_SET(MutationSet, SHADOW_DEPTH_PASS);
};

class PointShadowMultiviewPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(PointShadowMultiviewPipeline);
    DEFINE_SHADER_BUFFER(point_shadow_view_matrices);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT(GeometryPassBindlessParam, param);
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
        std::optional<GaussianShadowPass::RecordParameters> gaussian{};
    };

    struct GraphPassHandle {
        RenderGraph::PassHandle                 pass{};
        std::shared_ptr<const RecordParameters> parameters{};
    };

    ShadowDepthPass(RasterContext& context);
    void SetGaussians(
        RasterContext&          context,
        const SceneUpdateBatch& updates,
        const GaussianGpuScene* scene,
        GaussianViewSettings    settings,
        bool                    editor_view,
        const RasterConfig&     config
    );
    Array<TextureRef> GetGaussianShadowTextures(RasterContext& context) const;
    TextureRef AddGaussianDepthDebugToGraph(RenderGraph& graph, const RecordParameters& parameters) {
        return parameters.gaussian && m_gaussian_pass ?
                   m_gaussian_pass->AddDepthDebugToGraph(graph, *parameters.gaussian) :
                   TextureRef{};
    }
    double GetGaussianShadowGpuTime() {
        return m_gaussian_scene && m_gaussian_pass && !m_gaussian_bounds.empty() ?
                   m_gaussian_pass->GetGpuTime() :
                   0.;
    }
    BufferRef GetGaussianShadowData() const {
        return m_gaussian_scene && m_gaussian_pass ? m_gaussian_pass->GetDataBuffer() : BufferRef{};
    }

    [[nodiscard]] RecordParameters
    Prepare(RasterContext& context, const SceneUpdateBatch& updates, const RasterConfig& ui_config, const Camera& camera);
    GraphPassHandle AddToGraph(
        RenderGraph&        graph,
        RasterContext&      context,
        const SceneUpdateBatch& updates,
        const RasterConfig& ui_config,
        const Camera&       camera,
        GraphResources      resources
    );

    // 资源管理
    [[nodiscard]] bool PrepareCSMResources(RasterContext& context, const RasterConfig& ui_config);
    [[nodiscard]] bool PreparePointShadowResources(RasterContext& context, const RasterConfig& ui_config);

private:
    bool RefreshShadowCasterBounds(const SceneUpdateBatch& updates);
    static std::optional<ecs::CLightDirectional> GetMainLightDirection(const SceneUpdateBatch& updates);
    static std::optional<ecs::CLightPoint> GetMainPointLight(const SceneUpdateBatch& updates);

    void PrepareCSM(
        RasterContext& context, const SceneUpdateBatch& updates, const RasterConfig& ui_config, const Camera& camera,
        RecordParameters& parameters
    );
    void PreparePointShadows(
        RasterContext& context, const SceneUpdateBatch& updates, const RasterConfig& config, const Camera& camera,
        RecordParameters& parameters
    );
    [[nodiscard]] GeometryPassBindlessParam BuildDrawParameters(
        const RasterContext& context, const RasterConfig& config, const float4x4& world2clip,
        bool already_transposed
    ) const;
    void RecordShadowDraw(CommandList& cmd_list, const RecordParameters& parameters, const DrawBatch& batch);
    void RecordPointShadowMultiviewDraw(
        CommandList&            cmd_list,
        const RecordParameters& parameters,
        const DrawBatch&        batch
    );

private:
    static constexpr uint32 k_point_shadow_view_count = 6u;
    static constexpr uint32 k_point_shadow_view_mask  = (1u << k_point_shadow_view_count) - 1u;

    // Gaussian shadow state is prepared before graph compilation.
    std::unique_ptr<GaussianShadowPass> m_gaussian_pass;
    const GaussianGpuScene*             m_gaussian_scene = nullptr;
    GaussianViewSettings                m_gaussian_settings{};
    bool                                m_gaussian_editor_view = false;
    bool                                m_gaussian_point_mode  = false;
    uint64                              m_gaussian_revision    = 0;
    Array<Box3D>                        m_gaussian_bounds;

    // Mesh pipelines and persistent shadow-caster bounds.
    uint32                       enabled_cascade_layers;
    ShadowDepthPassPipeline      m_pso;
    PointShadowMultiviewPipeline m_point_shadow_multiview_pso;
    BufferRef                    m_point_shadow_view_matrices;
    bool                         m_point_shadow_multiview_supported = false;
    CullingPass                  m_culling_pass;
    Array<Box3D>                 m_shadow_caster_bounds;
    uint64                       m_shadow_caster_bounds_generation = 0u;
    bool                         m_shadow_caster_bounds_valid      = false;
    bool                         m_log_cascade_bounds_next_render  = false;
    LoopedTimer                  m_shadow_caster_bounds_log_timer{5.0, false};
};
} // namespace Moer::Render::Raster
