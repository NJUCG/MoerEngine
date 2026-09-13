#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/tessellated_surface/ShaderParameters.h"

namespace Moer::Render::Raster {

class TessellatedSurfacePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TessellatedSurfacePipeline);
    DEFINE_SHADER_BUFFER(surface_data);
    DEFINE_SHADER_ARGS(surface_data);
};

class TessellatedSurfacePass {
public:
    struct RecordParameters {
        bool                   enabled{false};
        TessellatedSurfaceData data{};
        BufferRef              surface_data{};
        TextureWithHandle      base_color{};
        TextureWithHandle      normal{};
        TextureWithHandle      metal_rough_ao{};
        TextureView            depth{};
        Rect2D                 render_area{};
        uint32_t               instance_count{0};
    };

    explicit TessellatedSurfacePass(RasterContext& context);

    [[nodiscard]] RecordParameters Prepare(
        const RasterContext& context,
        const RasterConfig&  config,
        const Camera&        camera
    ) const;
    void Record(CommandList& cmd_list, const RecordParameters& parameters);
    void Process(RasterContext& context, const RasterConfig& config, const Camera& camera);

    bool IsSupported() const {
        return supported;
    }

private:
    TessellatedSurfacePipeline pipeline;
    BufferRef                  surface_data_buffer;
    uint32_t                   device_max_tessellation_factor = 0;
    bool                       supported                      = false;
};

} // namespace Moer::Render::Raster
