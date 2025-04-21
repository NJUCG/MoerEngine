#pragma once

#include "math/Function.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/Camera.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

#include "RasterResource.h"
#include "RasterTool.h"
#include "ui/EditorUI.h"
#include "ui/raster_ui/RasterConfig.h"

namespace Moer::Render::Raster {
// "raster/geometry_pass/GeometryPassCommonPixel.hlsl"
class GeometryPass {
public:
    GeometryPass(RasterContext& _context) :
        vertex_shader("raster/geometry_pass/GeometryPassCommonVertex.hlsl") {}

    static DrawBatch GetDrawBatch(
        Moer::UnorderedMap<VertexFactory, GeometryPassPipeline> pipeline_map,
        VertexShader                                            vertex_shader,
        RasterContext&                                          context,
        const ArrayArgReference&                                arg_idx
    ) {
        DrawBatch draw_batch{};

        // 将每种不同顶点类型的Mesh分发到不同的MeshDrawDatas中。在Draw时，调用不同的PSO处理对应的MeshDrawDatas！
        // 所以，下面这段scene.ForEach的代码，也可以理解成将 “按Entity分组的Mesh” 转换为 “按顶点类型分组的Mesh的DrawDatas”
        UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>> mesh_draw_datas_map;

        std::span<const UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>>
            vertex_buffer_maps = context.scene.GetVertexBufferViews();
        std::span<const UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>> index_buffer_maps =
            context.scene.GetIndexBufferViews();
        UnorderedMap<VertexFactory, Array<MeshDrawData>> draw_mesh_datas_map;
        // LOG_INFO("");
        // LOG_INFO("New Render:");
        uint geom_idx = 0;
        context.scene.ForEach([&](Entity _entity) {
            auto& mesh = RenderableManager::Get().GetMeshInfo(_entity);

            const UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>& vertex_buffer_map =
                vertex_buffer_maps[mesh->global_mesh_idx];
            const UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>& index_buffer_map =
                index_buffer_maps[mesh->global_mesh_idx];

            // LOG_INFO("  Entity <-> Mesh info addr {}", (void*)mesh.get());

            for (const auto& [bitmask, vertex_buffer] :
                 vertex_buffer_map) { // for each vertex buffer (index buffer is the same)

                if (bitmask == 3) {
                    continue;
                    // TODO: bitmask == 3，即只有position和normal，这些Mesh应该是用于动画等功能的。不应该被正常渲染
                }

                const auto& index_buffer = index_buffer_map.at(bitmask);

                auto&         mesh_draw_datas = mesh_draw_datas_map[bitmask];
                VertexFactory factory{bitmask};
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
                        PF_D32_SFLOAT_S8_UINT
                    );
                    Shader& vtx  = vertex_shader.GetShader(&factory);
                    Shader& frag = ShaderManager::Get().CompileShader(
                        ST_FRAGMENT, "raster/geometry_pass/GeometryPassCommonPixel.hlsl"
                    );

                    pipeline_map.emplace(
                        factory,
                        ShaderManager::Get().Raster().Vertex(vtx).Pixel(frag).Build<GeometryPassPipeline>(
                            std::move(pso_info)
                        )
                    );
                }

                Array<MeshDrawData>& mesh_draw_array = draw_mesh_datas_map[factory];
                mesh_draw_array.emplace_back(vertex_buffer, index_buffer);
                // 注意，请不要改变Array内部的顺序
                // => 逻辑关联处：RHICommand.h -> MeshDrawData
                // auto& mesh_draw_data = mesh_draw_datas.emplace_back(vertex_buffer, index_buffer);

                // LOG_INFO("    Bitmask {}; index buf: {}, {}", bitmask, index_buffer.buffer.GetNumElements(), index_buffer.buffer.GetByteOffset());
            }

            for (uint i = 0; i < mesh->geometries.size(); i++) {
                uint                idx  = geom_idx++;
                const MeshGeometry& geom = *mesh->geometries[i];

                uint first_index    = geom.local_idx_offset;
                uint index_count    = geom.local_idx_count;
                uint first_vertex   = geom.local_vtx_offset;
                uint first_instance = idx;

                // LOG_INFO("    Geom {}; first_index {}, index_count {}, first_vertex {}, first_instance {}; geom offset {} {}; geom count {} {}",
                //          i,
                //          first_index,
                //          index_count,
                //          first_vertex,
                //          first_instance,
                //          geom.local_idx_offset,
                //          geom.local_vtx_offset,
                //          geom.local_idx_count,
                //          geom.local_vtx_count);

                auto bitmask = geom.mesh_buffers->vertex_factory_buffers.GetAttributesBitmask();

                if (bitmask == 3) {
                    continue;
                    // TODO: bitmask == 3，即只有position和normal，这些Mesh应该是用于动画等功能的。不应该被正常渲染
                }
                VertexFactory        factory{bitmask};
                Array<MeshDrawData>& mesh_draw_array = draw_mesh_datas_map[factory];
                mesh_draw_array.back().EmplaceDrawIndexed(
                    first_index, index_count, first_vertex, first_instance
                );

                // auto& mesh_draw_datas = mesh_draw_datas_map[bitmask];

                // mesh_draw_datas.back().EmplaceDrawIndexed(
                //     first_index, index_count, first_vertex, first_instance
                // );
            }
        });

        for (auto& [factory, draw_array] : draw_mesh_datas_map) {
            auto& pipeline = pipeline_map[factory];

            draw_batch.Emplace(pipeline_map[factory].handle, arg_idx)
                .RegisterDrawDatas(std::move(draw_array));
        }

        return draw_batch;
    }

    void Process(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera) {

        GeometryPassBindlessParam param;
        param.world2clip             = Transpose(camera->GetViewProjectionMatrix());
        param.instance_data          = context.gpu_instance_info_handle;
        param.geometry_data          = context.gpu_geometry_info_handle;
        param.geometry_instance_data = context.gpu_geometry_instance_handle;

        auto arg_idx = context.cmd_list.RegisterArgs(GeometryPassPipeline::SetArgs(context.bdls, param));

        DrawBatch draw_batch = GeometryPass::GetDrawBatch(pipeline_map, vertex_shader, context, arg_idx);

        context.cmd_list
            .Gfx(
                "Geometry Pass (MultiPass)",
                Rect2D(0, 0, context.resolution.x, context.resolution.y),
                DepthAttachment(context.textures.depth_linear_sampler.tex->GetView().GetTexture()),
                ColorAttachment(context.textures.vbuffer.tex),
                ColorAttachment(context.textures.normal.tex),
                ColorAttachment(context.textures.tangent.tex),
                ColorAttachment(context.textures.uv.tex),
                ColorAttachment(context.textures.position.tex)
            )
            .AcceptDrawBatch(std::move(draw_batch))
            .Dispatch();
        // 注：此处ColorAttachment的顺序需要和 GeometryPassPsoManager.cpp 中的 RHIColorAttachmentInfo::Preset 顺序一致
    }

private:
    Moer::UnorderedMap<VertexFactory, GeometryPassPipeline> pipeline_map;
    VertexShader                                            vertex_shader;
};

} // namespace Moer::Render::Raster