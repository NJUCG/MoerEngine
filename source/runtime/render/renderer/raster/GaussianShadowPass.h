#pragma once
#include "RasterResource.h"
#include "scene/gaussian/GaussianGpuScene.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/gaussian/GaussianShadowParameters.h"
namespace Moer::Render::Raster {
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
    void             Record(CommandList& commands, const RecordParameters& parameters);
    TextureRef       RecordDepthDebug(CommandList& commands, const RecordParameters& parameters);
    const BufferRef& GetDataBuffer() const {
        return m_data;
    }

private:
    GaussianShadowPipeline        m_pipeline;
    GaussianShadowInspectPipeline m_inspect;
    TextureRef                    m_debug_depth;
    BindlessArrayRef              m_bindless;
    BufferRef                     m_data;
    uint                          m_data_handle = 0;
    StaticArray<TextureRef, 4>    m_maps{};
    StaticArray<uint, 4>          m_handles{};
    bool                          m_cube = false;
};
} // namespace Moer::Render::Raster
