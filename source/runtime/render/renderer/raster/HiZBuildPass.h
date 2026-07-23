// 构建 GPU 遮挡剔除使用的层次化反向 Z 深度金字塔。
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
    struct MipDispatch {
        TextureView source{};
        TextureView destination{};
        uint2       source_size{};
        uint2       destination_size{};
        bool        is_mip0{false};
    };

    /**
     * Immutable, strongly-owned recording input.  RenderGraph workers must not
     * read RasterContext::textures while CommitHiZHistory may swap the current
     * and previous images on the render thread.
     */
    struct RecordParameters {
        DepthBufferRef     source_owner{};
        TextureRef         destination_owner{};
        Array<MipDispatch> dispatches{};

        [[nodiscard]] bool IsEmpty() const noexcept {
            return dispatches.empty();
        }
    };

    HiZBuildPass(RasterContext& context) {
        pipeline = context.manager.Compute<HiZBuildPipeline>(
            "pipelines/raster/culling/HiZBuild.comp.hlsl"
        );
    }

    [[nodiscard]] RecordParameters Prepare(const RasterContext& context) const {
        RecordParameters parameters{};
        if (context.textures.hiz_current.tex == nullptr) {
            return parameters;
        }

        const uint mip_count = context.textures.hiz_current.tex->GetNumMips();
        if (mip_count == 0) {
            return parameters;
        }

        parameters.source_owner      = context.textures.depth_nearest_sampler.tex;
        parameters.destination_owner = context.textures.hiz_current.tex;
        parameters.dispatches.reserve(mip_count);

        const uint2 mip0_size = context.textures.hiz_current.GetSize(0);
        parameters.dispatches.emplace_back(MipDispatch{
            .source           = parameters.source_owner->GetView(),
            .destination      = parameters.destination_owner->GetView(0, 1),
            .source_size      = mip0_size,
            .destination_size = mip0_size,
            .is_mip0          = true,
        });

        for (uint mip = 1; mip < mip_count; ++mip) {
            parameters.dispatches.emplace_back(MipDispatch{
                .source = parameters.destination_owner->GetView(static_cast<uint8>(mip - 1), 1),
                .destination = parameters.destination_owner->GetView(static_cast<uint8>(mip), 1),
                .source_size = context.textures.hiz_current.GetSize(mip - 1),
                .destination_size = context.textures.hiz_current.GetSize(mip),
                .is_mip0 = false,
            });
        }
        return parameters;
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        if (parameters.IsEmpty()) {
            return;
        }

        // mip 0 复制深度，后续每次 dispatch 将当前层级的每个 2x2 区域归约到下一个 mip。
        auto dispatch_build = [&](TextureView source_view,
                                  TextureView destination_view,
                                  const uint2 source_size,
                                  const uint2 destination_size,
                                  bool        is_mip0) {
            HiZBuildParam param{};
            param.src_size = source_size;
            param.dst_size = destination_size;
            param.is_mip0  = is_mip0 ? 1u : 0u;

            cmd_list.Compute(pipeline, param, source_view, destination_view)
                .Dispatch(
                    uint3((destination_size.x + 7) / 8, (destination_size.y + 7) / 8, 1),
                    "Build Hi-Z"
                );
        };

        cmd_list.PushScopeWithTimeScope("Build Hi-Z");
        for (const MipDispatch& dispatch : parameters.dispatches) {
            dispatch_build(
                dispatch.source,
                dispatch.destination,
                dispatch.source_size,
                dispatch.destination_size,
                dispatch.is_mip0
            );
        }
        cmd_list.PopScopeWithTimeScope();
    }

    void Process(RasterContext& context) {
        Record(context.cmd_list, Prepare(context));
    }

private:
    HiZBuildPipeline pipeline;
};

} // namespace Moer::Render::Raster
