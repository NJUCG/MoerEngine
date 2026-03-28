#pragma once

#include "math/Function.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

#include "CullingPass.h"
#include "RasterConfig.h"
#include "RasterResource.h"

namespace Moer::Render::Raster {

class GeometryPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(GeometryPassPipeline);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(GeometryPassBindlessParam, param);
    DEFINE_SHADER_ARGS(bdls, param);

    MUTATION_BOOL(SHADOW_DEPTH_PASS);
    MUTATION_SET(MutationSet, SHADOW_DEPTH_PASS);
};

class GeometryPass {
public:
    GeometryPass(RasterContext& context) : m_culling_pass(context), m_culling_last_enabled(false) {

        // 1. PSO

        RHIDepthStencilStateInfo ds_info =
            RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>();
        //TODO：开启深度模板测试提高性能
        // ds_info.b_enable_front_face_stencil = true;
        // ds_info.front_face_pass_stencil_op  = EStencilOp::SO_REPLACE;
        // ds_info.b_enable_back_face_stencil  = true;
        // ds_info.back_face_pass_stencil_op   = EStencilOp::SO_REPLACE;

        GfxPsoCreateInfo pso_info(
            RHIRasterizeInfo::Preset(),
            {},
            {
                RHIColorAttachmentInfo::Preset(PF_R32_UINT),                 // vbuffer
                RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32), // normal
                RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32), // tangent
                RHIColorAttachmentInfo::Preset(PF_R32G32_SFLOAT),            // uv
                RHIColorAttachmentInfo::Preset(PF_R32G32B32A32_SFLOAT)       // position
            },
            ds_info, // depth buf
            context.textures.depth_linear_sampler.tex->GetFormat()
        );

        GeometryPassPipeline::MutationSet mutation_set{};
        mutation_set.SetMutation<GeometryPassPipeline::SHADOW_DEPTH_PASS>(false);

        Shader& vtx = ShaderManager::Get().CompileShader(
            ST_VERTEX, "pipelines/raster/deferred/geometry/GeometryPassVertex.hlsl", mutation_set
        );
        Shader& frag = ShaderManager::Get().CompileShader(
            ST_FRAGMENT, "pipelines/raster/deferred/geometry/GeometryPassPixel.hlsl", mutation_set
        );

        m_pso = ShaderManager::Get().Raster().Vertex(vtx).Pixel(frag).Build<GeometryPassPipeline>(
            std::move(pso_info)
        );
    }

    void Process(RasterContext& context, RasterConfig& ui_config, const Camera& camera) {
        const auto& gpu_scene_res = context.scene.gpu_scene_res();

        // 1. Frustum Culling (if enabled)
        if (ui_config.enable_frustum_culling) {
            // 每帧恢复原始 draw commands（因为 culling 会修改 instance_cnt）
            context.scene.RestoreDrawCommands(context.cmd_list);

            CullingPass::CullStatistics stats;
            m_culling_pass.Process(context, camera, gpu_scene_res, &stats);

            // 更新 UI 统计信息
            ui_config.culling_stats.total_instances_before = stats.total_instances_before;
            ui_config.culling_stats.total_instances_after  = stats.total_instances_after;
            ui_config.culling_stats.visible_draws          = stats.visible_draws;
            ui_config.culling_stats.total_draws            = stats.total_draws;
            m_culling_last_enabled                         = true;
        } else {
            // 重置统计
            ui_config.culling_stats = RasterConfig::CullingStats{};
            // 恢复原始 draw commands（如果之前启用了 culling）
            if (m_culling_last_enabled) {
                context.scene.RestoreDrawCommands(context.cmd_list);
                m_culling_last_enabled = false;
            }
        }

        // 2. Params
        GeometryPassBindlessParam param;
        param.world2clip = Transpose(camera.GetViewProjectionMatrix());

        param.instance_buf_hdl       = gpu_scene_res.instance_buf.hdl;
        param.primitive_buf_hdl      = gpu_scene_res.primitive_buf.hdl;
        param.position_buf_hdl       = gpu_scene_res.position_buf.hdl;
        param.packed_normal_buf_hdl  = gpu_scene_res.packed_normal_buf.hdl;
        param.packed_tangent_buf_hdl = gpu_scene_res.packed_tangent_buf.hdl;
        param.texcoord0_buf_hdl      = gpu_scene_res.texcoord0_buf.hdl;
        param.material_buf_hdl       = gpu_scene_res.material_buf.hdl;

        param.enable_alpha_test             = ui_config.geometry_enable_alpha_test ? 1 : 0;
        param.alpha_test_blend_pixel_cutoff = ui_config.geometry_alpha_test_blend_pixel_cutoff;

        // 3. Draw
        auto rect2d = context.textures.position.GetRect2D();
        assert(
            rect2d == context.textures.vbuffer.GetRect2D() && rect2d == context.textures.normal.GetRect2D() &&
            rect2d == context.textures.tangent.GetRect2D() && rect2d == context.textures.uv.GetRect2D()
        );

        context.cmd_list.Gfx(m_pso, context.bdls, param)
            .DrawIndirect(
                "Geometry Pass",
                rect2d,
                {}, // Vertex Buffers 通过 Bindless 访问
                IndexBuffer{gpu_scene_res.index_buf.buf->GetView(), EIndexElementType::IET_UINT32},
                gpu_scene_res.draw_cmd_buf.buf->GetView(),       // DrawIndexedCmdData 数组
                gpu_scene_res.draw_cmd_buf.buf->GetNumElement(), // CPU count
                gpu_scene_res.draw_cmd_buf.buf->GetStride(),
                DepthAttachment(context.textures.depth_linear_sampler.tex->GetView().GetTexture()),
                ColorAttachment(context.textures.vbuffer.tex),
                ColorAttachment(context.textures.normal.tex),
                ColorAttachment(context.textures.tangent.tex),
                ColorAttachment(context.textures.uv.tex),
                ColorAttachment(context.textures.position.tex)
            );
    }

private:
    GeometryPassPipeline m_pso;
    CullingPass          m_culling_pass;
    bool                 m_culling_last_enabled;
};

} // namespace Moer::Render::Raster