// Builds the hierarchical reverse-Z depth pyramid used by GPU occlusion culling.
#pragma once

#include "RasterResource.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/culling/ShaderParameters.h"

namespace Moer::Render::Raster {

class HiZBuildPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(HiZBuildPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(HiZBuildParam, param);
    DEFINE_SHADER_TEX(src_texture);
    DEFINE_SHADER_TEX(dst_texture);

    DEFINE_SHADER_ARGS(param, src_texture, dst_texture);
};

class HiZBuildPass {
public:
    HiZBuildPass(RasterContext& context) {
        pipeline = context.manager.Compute<HiZBuildPipeline>(
            "pipelines/raster/culling/HiZBuild.comp.hlsl"
        );
    }

    void Process(RasterContext& context) {
        if (context.textures.hiz_current.tex == nullptr) {
            return;
        }

        const uint mip_count = context.textures.hiz_current.tex->GetNumMips();
        if (mip_count == 0) {
            return;
        }

        // Mip 0 copies depth; each later dispatch reduces one 2x2 level into the next mip.
        auto dispatch_build = [&](TextureView source_view,
                                  TextureView destination_view,
                                  const uint2 source_size,
                                  const uint2 destination_size,
                                  bool        is_mip0) {
            HiZBuildParam param{};
            param.src_size = source_size;
            param.dst_size = destination_size;
            param.is_mip0  = is_mip0 ? 1u : 0u;

            context.cmd_list.Compute(pipeline, param, source_view, destination_view)
                .Dispatch(
                    uint3((destination_size.x + 7) / 8, (destination_size.y + 7) / 8, 1),
                    "Build Hi-Z"
                );
        };

        context.cmd_list.PushScopeWithTimeScope("Build Hi-Z");

        {
            const TextureView source_view      = context.textures.depth_nearest_sampler.tex->GetView();
            const TextureView destination_view = context.textures.hiz_current.tex->GetView(0, 1);
            const uint2       mip0_size        = context.textures.hiz_current.GetSize(0);

            dispatch_build(source_view, destination_view, mip0_size, mip0_size, true);
        }

        for (uint mip = 1; mip < mip_count; ++mip) {
            const TextureView source_view =
                context.textures.hiz_current.tex->GetView(static_cast<uint8>(mip - 1), 1);
            const TextureView destination_view =
                context.textures.hiz_current.tex->GetView(static_cast<uint8>(mip), 1);
            const uint2 source_size      = context.textures.hiz_current.GetSize(mip - 1);
            const uint2 destination_size = context.textures.hiz_current.GetSize(mip);

            dispatch_build(source_view, destination_view, source_size, destination_size, false);
        }

        context.cmd_list.PopScopeWithTimeScope();
    }

private:
    HiZBuildPipeline pipeline;
};

} // namespace Moer::Render::Raster
