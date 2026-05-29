#pragma once

#include "RasterConfig.h"
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
        m_pso = context.manager.Compute<HiZBuildPipeline>("pipelines/raster/culling/HiZBuild.comp.hlsl");
    }

    void Process(RasterContext& context, RasterConfig&) {
        if (context.textures.hiz_current.tex == nullptr) {
            return;
        }

        const uint mip_count = context.textures.hiz_current.tex->GetNumMips();
        if (mip_count == 0) {
            return;
        }

        // 第一版先保证结果正确：mip0 从 depth 拷贝，后续每层单独做一次 2x2 reduction
        auto dispatch_build = [&](TextureView src_view,
                                  TextureView dst_view,
                                  const uint2 src_size,
                                  const uint2 dst_size,
                                  bool        is_mip0) {
            HiZBuildParam param{};
            param.src_size = src_size;
            param.dst_size = dst_size;
            param.is_mip0  = is_mip0 ? 1u : 0u;

            context.cmd_list.Compute(m_pso, param, src_view, dst_view)
                .Dispatch(uint3((dst_size.x + 7) / 8, (dst_size.y + 7) / 8, 1), "Build Hi-Z");
        };

        context.cmd_list.PushScopeWithTimeScope("Build Hi-Z");

        {
            TextureView src_view  = context.textures.depth_nearest_sampler.tex->GetView();
            TextureView dst_view  = context.textures.hiz_current.tex->GetView(0, 1);
            uint2       mip0_size = context.textures.hiz_current.GetSize(0);

            dispatch_build(src_view, dst_view, mip0_size, mip0_size, true);
        }

        for (uint mip = 1; mip < mip_count; ++mip) {
            TextureView src_view = context.textures.hiz_current.tex->GetView(static_cast<uint8>(mip - 1), 1);
            TextureView dst_view = context.textures.hiz_current.tex->GetView(static_cast<uint8>(mip), 1);
            uint2       src_size = context.textures.hiz_current.GetSize(mip - 1);
            uint2       dst_size = context.textures.hiz_current.GetSize(mip);

            dispatch_build(src_view, dst_view, src_size, dst_size, false);
        }

        context.cmd_list.PopScopeWithTimeScope();
    }

private:
    HiZBuildPipeline m_pso;
};

} // namespace Moer::Render::Raster
