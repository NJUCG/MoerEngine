// 对场景实例执行可见性剔除，并写入延迟渲染几何缓冲。
#pragma once

#include "math/Function.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rendergraph/RenderGraph.h"
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
    struct GraphResources {
        RenderGraph::TokenHandle   scene{};
        RenderGraph::TextureHandle hiz_previous{};
        RenderGraph::TextureHandle base_color{};
        RenderGraph::TextureHandle normal{};
        RenderGraph::TextureHandle metal_rough_ao{};
        RenderGraph::TextureHandle depth{};
    };

    struct RecordParameters {
        CullingPass::RecordParameters culling{};
        GeometryPassBindlessParam      shader{};
        BindlessArrayRef               bindless{};
        BufferView                     index_buffer{};
        BufferView                     draw_commands{};
        BufferView                     draw_count{};
        uint                           draw_command_stride = 0;
        uint                           max_draw_count      = 0;
        Rect2D                         render_area{};
        DepthBufferRef                 depth{};
        TextureRef                     base_color{};
        TextureRef                     normal{};
        TextureRef                     metal_rough_ao{};
    };

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

    [[nodiscard]] RecordParameters
    Prepare(RasterContext& context, RasterConfig& raster_config, const Camera& camera) {
        const auto& gpu_scene_res = context.GetGpuSceneRes();

        const bool use_occlusion_culling = raster_config.enable_occlusion_culling &&
                                           context.hiz_data.previous_valid &&
                                           context.textures.hiz_previous.tex != nullptr &&
                                           context.hiz_data.mip_count > 0;

        // 剔除阶段始终运行以选择 LOD；视锥和 Hi-Z 剔除仍可配置。
        CullingPass::CullStatistics culling_stats;
        CullingPass::CullingOptions culling_options{
            raster_config.enable_frustum_culling,
            use_occlusion_culling,
            raster_config.cluster_lod_error_threshold,
            raster_config.force_lod_level
        };

        auto culling = culling_pass.Prepare(
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

        GeometryPassBindlessParam param{};
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

        const auto& visibility = context.gpu_culling_buffers.geometry;
        return RecordParameters{
            .culling = std::move(culling),
            .shader = param,
            .bindless = context.bdls,
            .index_buffer = gpu_scene_res.index_buf.buf->GetView(),
            .draw_commands = visibility.draw_cmd_buf->GetView(),
            .draw_count = visibility.GetDrawCountView(),
            .draw_command_stride = visibility.draw_cmd_buf->GetStride(),
            .max_draw_count = visibility.max_draw_count,
            .render_area = render_area,
            .depth = context.textures.depth_linear_sampler.tex,
            .base_color = context.textures.base_color.tex,
            .normal = context.textures.normal.tex,
            .metal_rough_ao = context.textures.metal_rough_ao.tex
        };
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        culling_pass.Record(cmd_list, parameters.culling);

        cmd_list.PushScopeWithTimeScope(RasterTool::GetGeometryDrawProfileScopeName());
        auto draw_command = cmd_list.Gfx(pipeline, parameters.bindless, parameters.shader);
        draw_command.DrawIndirect(
            "Geometry Pass",
            parameters.render_area,
            {},
            IndexBuffer{parameters.index_buffer, EIndexElementType::IET_UINT32},
            parameters.draw_commands,
            parameters.draw_count,
            parameters.draw_command_stride,
            parameters.max_draw_count,
            DepthAttachment(parameters.depth->GetView().GetTexture()),
            ColorAttachment(parameters.base_color),
            ColorAttachment(parameters.normal),
            ColorAttachment(parameters.metal_rough_ao)
        );
        cmd_list.PopScopeWithTimeScope();
    }

    [[nodiscard]] RenderGraph::PreparedPassHandle AddToGraph(
        RenderGraph& graph,
        RasterContext& context,
        RasterConfig& raster_config,
        const Camera& camera,
        GraphResources resources,
        std::span<const RenderGraph::SetupPassHandle> setup_dependencies = {}
    );

    void Process(RasterContext& context, RasterConfig& raster_config, const Camera& camera) {
        const auto parameters = Prepare(context, raster_config, camera);
        Record(context.cmd_list, parameters);
    }

private:
    GeometryPassPipeline pipeline;
    CullingPass          culling_pass;
};

} // namespace Moer::Render::Raster
