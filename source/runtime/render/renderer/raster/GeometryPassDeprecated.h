#pragma once

#include "math/Function.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/Material.h"
#include "scene/camera/Camera.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

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
    GeometryPass(RasterContext& _context) :
        vertex_shader("pipelines/raster/deferred/geometry/GeometryPassCommonVertex.hlsl"),
        shadow_vertex_shader(
            "pipelines/raster/deferred/geometry/GeometryPassCommonVertex.hlsl",
            GeometryPassPipeline::MutationSet::GetMutationSetFromValues(true)
        ) {}

    void Process(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera) {

        GeometryPassBindlessParam param;
        param.world2clip                    = Transpose(camera->GetViewProjectionMatrix());
        param.instance_data                 = context.gpu_instance_info_handle;
        param.geometry_data                 = context.gpu_geometry_info_handle;
        param.geometry_instance_data        = context.gpu_geometry_instance_handle;
        param.material_buffer               = context.gpu_material_info_handle;
        param.enable_alpha_test             = ui_config.geometry_enable_alpha_test ? 1 : 0;
        param.alpha_test_blend_pixel_cutoff = ui_config.geometry_alpha_test_blend_pixel_cutoff;

        // MeshDrawDatas
        auto mesh_draw_datas_map = RasterTool::GetDrawMeshDatasMap(context, false);

        // PipelineMap
        for (auto& [factory, _] : mesh_draw_datas_map) {

            if (!pipeline_map.contains(factory)) {
                VertexStream     stream = factory.GetVertexStream();
                GfxPsoCreateInfo pso_info(
                    RHIRasterizeInfo::Preset(),
                    std::move(stream),
                    {
                        RHIColorAttachmentInfo::Preset(PF_R32_UINT),                 // vbuffer
                        RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32), // normal
                        RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32), // tangent
                        RHIColorAttachmentInfo::Preset(PF_R32G32_SFLOAT),            // uv
                        RHIColorAttachmentInfo::Preset(PF_R32G32B32A32_SFLOAT)       // position
                    },
                    RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(), // depth buf
                    context.textures.depth_linear_sampler.tex->GetFormat()
                );

                Shader& vtx = vertex_shader.GetShader(const_cast<VertexFactory*>(&factory));

                GeometryPassPipeline::MutationSet mutation_set{};
                mutation_set.SetMutation<GeometryPassPipeline::SHADOW_DEPTH_PASS>(false);
                Shader& frag = ShaderManager::Get().CompileShader(
                    ST_FRAGMENT,
                    "pipelines/raster/deferred/geometry/GeometryPassCommonPixel.hlsl",
                    mutation_set
                );

                pipeline_map.emplace(
                    factory,
                    ShaderManager::Get().Raster().Vertex(vtx).Pixel(frag).Build<GeometryPassPipeline>(
                        std::move(pso_info)
                    )
                );
            }
        }

        // DrawBatch
        auto draw_batch = DrawBatch{};
        auto arg_idx    = context.cmd_list.RegisterArgs(GeometryPassPipeline::SetArgs(context.bdls, param));
        for (auto& [factory, draw_array] : mesh_draw_datas_map) {
            draw_batch.Emplace(pipeline_map[factory].handle, arg_idx)
                .RegisterDrawDatas(std::move(draw_array));
        }

        // Draw
        auto rect2d = context.textures.position.GetRect2D();
        assert(
            rect2d == context.textures.vbuffer.GetRect2D() && rect2d == context.textures.normal.GetRect2D() &&
            rect2d == context.textures.tangent.GetRect2D() && rect2d == context.textures.uv.GetRect2D()
        );
        context.cmd_list
            .Gfx(
                "Geometry Pass (MultiPass)",
                rect2d,
                DepthAttachment(context.textures.depth_linear_sampler.tex->GetView().GetTexture()),
                ColorAttachment(context.textures.vbuffer.tex),
                ColorAttachment(context.textures.normal.tex),
                ColorAttachment(context.textures.tangent.tex),
                ColorAttachment(context.textures.uv.tex),
                ColorAttachment(context.textures.position.tex)
            )
            .AcceptDrawBatch(std::move(draw_batch))
            .Dispatch();
        // 注：此处ColorAttachment的顺序需要和GfxPsoCreateInfo中的顺序一致
    }

private:
    Moer::UnorderedMap<VertexFactory, GeometryPassPipeline> pipeline_map;
    VertexShader                                            vertex_shader;
    VertexShader                                            shadow_vertex_shader;
};

} // namespace Moer::Render::Raster