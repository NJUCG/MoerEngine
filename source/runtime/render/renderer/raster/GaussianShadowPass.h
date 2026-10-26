#pragma once
#include "RasterResource.h"
#include "rendergraph/RenderGraph.h"
#include "scene/gaussian/GaussianGpuScene.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/gaussian/GaussianShadowParameters.h"
namespace Moer::Render::Raster {
inline RenderGraph::TextureDesc DescribeShadowTexture(const TextureRef& texture) {
    auto       aspects = RenderGraph::TextureAspect::None;
    const auto flags   = texture->GetAspectFlags();
    if (uint32(flags & ETextureAspectFlags::COLOR))
        aspects = aspects | RenderGraph::TextureAspect::Color;
    if (uint32(flags & ETextureAspectFlags::DEPTH_SLICE))
        aspects = aspects | RenderGraph::TextureAspect::Depth;
    if (uint32(flags & ETextureAspectFlags::STENCIL_SLICE))
        aspects = aspects | RenderGraph::TextureAspect::Stencil;
    return {.mip_count = texture->GetNumMips(), .layer_count = texture->GetNumArray(), .aspects = aspects};
}
class GaussianShadowPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(GaussianShadowPipeline);
    DEFINE_SHADER_CONSTANT(GaussianShadowDraw, param);
    DEFINE_SHADER_BUFFER(gaussians);
    DEFINE_SHADER_BUFFER(instances);
    DEFINE_SHADER_ARGS(param, gaussians, instances);
};
class GaussianShadowInspectPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianShadowInspectPipeline);
    DEFINE_SHADER_BUFFER(data);
    DEFINE_SHADER_TEX(output);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(data, output, bdls);
};
class RENDER_API GaussianShadowPass {
public:
    struct RecordParameters {
        GaussianShadowData         data{};
        BufferRef                  data_buffer{}, gaussians{}, instances{};
        BindlessArrayRef           bindless{};
        StaticArray<TextureRef, 4> maps{};
        Array<GGaussianInstance>   draws{};
        Array<uint>                instance_indices{};
    };
    explicit GaussianShadowPass(RasterContext& context);
    GaussianShadowPass(ShaderManager& manager, BindlessArrayRef bindless, EPixelFormat depth_format);
    ~GaussianShadowPass();
    RecordParameters Prepare(
        RasterContext&          context,
        const RasterConfig&     config,
        const Camera&           camera,
        const GaussianGpuScene& scene,
        GaussianViewSettings    settings,
        bool                    editor_view
    );
    void       Record(CommandList& commands, const RecordParameters& parameters);
    TextureRef RecordDepthDebug(CommandList& commands, const RecordParameters& parameters);
    void       EnsureResources(RasterContext& context, const RasterConfig& config, bool cast);
    const StaticArray<TextureRef, 4>& GetMaps() const {
        return m_maps;
    }
    TextureRef RecordDepthDebug(CommandList& commands) {
        return m_last ? RecordDepthDebug(commands, *m_last) : TextureRef{};
    }
    void ResetFrame() {
        m_last.reset();
    }
    double           GetGpuTime();
    const BufferRef& GetDataBuffer() const {
        return m_data;
    }

private:
    std::optional<RecordParameters> m_last;
    GaussianShadowPipeline          m_pipeline;
    GaussianShadowInspectPipeline   m_inspect;
    TextureRef                      m_debug_depth;
    BindlessArrayRef                m_bindless;
    BufferRef                       m_data;
    uint                            m_data_handle = 0;
    StaticArray<TextureRef, 4>      m_maps{};
    StaticArray<uint, 4>            m_handles{};
    StaticArray<TextureRef, 4>      m_mesh_maps{};
    StaticArray<uint, 4>            m_mesh_handles{};
    QueryFuture                     m_timing{};
    double                          m_gpu_ms = 0;
    bool                            m_cube   = false;
};
} // namespace Moer::Render::Raster
