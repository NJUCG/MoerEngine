// Culls visible scene instances and writes the deferred geometry buffers.
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
#include "RasterTool.h"

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
    GeometryPass(RasterContext& context) : culling_pass(context) {
        RHIDepthStencilStateInfo depth_stencil_info =
            RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>();

        GfxPsoCreateInfo pipeline_info(
            RHIRasterizeInfo::Preset(),
            {},
            {
                RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM),           // base_color
                RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32), // normal
                RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM)            // metal_rough_ao
            },
            depth_stencil_info,
            context.textures.depth_linear_sampler.tex->GetFormat()
        );

        GeometryPassPipeline::MutationSet mutation_set{};
        mutation_set.SetMutation<GeometryPassPipeline::SHADOW_DEPTH_PASS>(false);

        Shader& vertex_shader = ShaderManager::Get().CompileShader(
            ST_VERTEX, "pipelines/raster/deferred/geometry/GeometryPassVertex.hlsl", mutation_set
        );
        Shader& fragment_shader = ShaderManager::Get().CompileShader(
            ST_FRAGMENT, "pipelines/raster/deferred/geometry/GeometryPassPixel.hlsl", mutation_set
        );

        pipeline = ShaderManager::Get()
                       .Raster()
                       .Vertex(vertex_shader)
                       .Pixel(fragment_shader)
                       .Build<GeometryPassPipeline>(std::move(pipeline_info));
    }

    void Process(RasterContext& context, RasterConfig& raster_config, const Camera& camera) {
        const auto& gpu_scene_res = context.GetGpuSceneRes();

        const bool use_occlusion_culling = raster_config.enable_occlusion_culling &&
                                           context.hiz_data.previous_valid &&
                                           context.textures.hiz_previous.tex != nullptr &&
                                           context.hiz_data.mip_count > 0;

        // Culling always runs to select LODs; frustum and Hi-Z rejection remain configurable.
        CullingPass::CullStatistics culling_stats;
        CullingPass::CullingOptions culling_options{
            raster_config.enable_frustum_culling,
            use_occlusion_culling,
            raster_config.cluster_lod_error_threshold,
            raster_config.force_lod_level
        };

        culling_pass.Process(
            context,
            camera,
            gpu_scene_res,
            context.gpu_culling_buffers.geometry,
            &culling_stats,
            RasterTool::GetGeometryCullingProfileScopeName(),
            culling_options
        );

        raster_config.culling_stats.total_instances_before = culling_stats.total_instances_before;
        raster_config.culling_stats.total_instances_after  = culling_stats.total_instances_after;
        raster_config.culling_stats.visible_draws          = culling_stats.visible_draws;
        raster_config.culling_stats.total_draws            = culling_stats.total_draws;
        raster_config.culling_stats.frustum_culled_instances =
            culling_stats.frustum_culled_instances;
        raster_config.culling_stats.occlusion_culled_instances =
            culling_stats.occlusion_culled_instances;
        raster_config.culling_stats.lod_culled_instances = culling_stats.lod_culled_instances;

        GeometryPassBindlessParam param;
        param.world2clip = Transpose(camera.GetViewProjectionMatrix());

        param.instance_buf_hdl              = gpu_scene_res.instance_buf.hdl;
        param.visible_instance_id_buf_hdl   = context.gpu_culling_buffers.geometry.visible_instance_id_buf.hdl;
        param.use_visible_instance_id_remap = 1;
        param.primitive_buf_hdl             = gpu_scene_res.primitive_buf.hdl;
        param.position_buf_hdl              = gpu_scene_res.position_buf.hdl;
        param.packed_normal_buf_hdl         = gpu_scene_res.packed_normal_buf.hdl;
        param.packed_tangent_buf_hdl        = gpu_scene_res.packed_tangent_buf.hdl;
        param.texcoord0_buf_hdl             = gpu_scene_res.texcoord0_buf.hdl;
        param.material_buf_hdl              = gpu_scene_res.material_buf.hdl;

        param.enable_alpha_test = raster_config.geometry_enable_alpha_test ? 1 : 0;
        param.alpha_test_blend_pixel_cutoff =
            raster_config.geometry_alpha_test_blend_pixel_cutoff;
        param.debug_visualization_mode =
            static_cast<uint>(raster_config.geometry_debug_visualization);

        const auto render_area = context.textures.base_color.GetRect2D();
        assert(
            render_area == context.textures.normal.GetRect2D() &&
            render_area == context.textures.metal_rough_ao.GetRect2D()
        );

        context.cmd_list.PushScopeWithTimeScope(RasterTool::GetGeometryDrawProfileScopeName());
        auto draw_command = context.cmd_list.Gfx(pipeline, context.bdls, param);

        const auto& visibility = context.gpu_culling_buffers.geometry;
        draw_command.DrawIndirect(
            "Geometry Pass",
            render_area,
            {},
            IndexBuffer{gpu_scene_res.index_buf.buf->GetView(), EIndexElementType::IET_UINT32},
            visibility.draw_cmd_buf->GetView(),
            visibility.GetDrawCountView(),
            visibility.draw_cmd_buf->GetStride(),
            visibility.max_draw_count,
            DepthAttachment(context.textures.depth_linear_sampler.tex->GetView().GetTexture()),
            ColorAttachment(context.textures.base_color.tex),
            ColorAttachment(context.textures.normal.tex),
            ColorAttachment(context.textures.metal_rough_ao.tex)
        );
        context.cmd_list.PopScopeWithTimeScope();
    }

private:
    GeometryPassPipeline pipeline;
    CullingPass          culling_pass;
};

} // namespace Moer::Render::Raster
