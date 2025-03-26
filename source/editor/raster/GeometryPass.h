#pragma once

#include "math/Function.h"
#include "scene/Camera.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

#include "RasterResource.h"
#include "RasterTool.h"
#include "ui/EditorUI.h"
#include "ui/raster_ui/RasterConfig.h"

namespace Moer::Render::Raster {

class GeometryPass {
public:
    GeometryPass(RasterContext& context) { CreateViewData(context); }

    void CreateViewData(RasterContext& context) {
        view_param_buffer =
            context.device.CreateBuffer<byte>(sizeof(ViewParam), EBufferUsageFlags::CONSTANT_BUFFER);
    }

    void Process(RasterContext& context, const RasterConfig& ui_config, CameraRef& camera) {

        // 将每种不同顶点类型的Mesh分发到不同的MeshDrawDatas中。在Draw时，调用不同的PSO处理对应的MeshDrawDatas！
        // 所以，下面这段scene.ForEach的代码，也可以理解成将 “按Entity分组的Mesh” 转换为 “按顶点类型分组的Mesh的DrawDatas”
        UnorderedMap<VertexAttributesBitmask, Array<MeshDrawData>> mesh_draw_datas_map;

        std::span<const UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>>
            vertex_buffer_maps = context.scene.GetVertexBufferViews();
        std::span<const UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>> index_buffer_maps =
            context.scene.GetIndexBufferViews();

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

                auto& mesh_draw_datas = mesh_draw_datas_map[bitmask];

                // 注意，请不要改变Array内部的顺序
                // => 逻辑关联处：RHICommand.h -> MeshDrawData
                auto& mesh_draw_data = mesh_draw_datas.emplace_back(vertex_buffer, index_buffer);

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

                auto& mesh_draw_datas = mesh_draw_datas_map[bitmask];

                mesh_draw_datas.back().EmplaceDrawIndexed(
                    first_index, index_count, first_vertex, first_instance
                );
            }
        });

        // 注意生命周期！
        ViewParam* view_param = MoerNew(ViewParam);

        view_param->clip2view  = Transpose(camera->GetProjectionMatrixInv());
        view_param->view2clip  = Transpose(camera->GetProjectionMatrix());
        view_param->view2world = Transpose(camera->GetViewMatrixInv());
        view_param->world2view = Transpose(camera->GetViewMatrix());
        view_param->world2clip = Transpose(camera->GetViewProjectionMatrix());
        view_param->clip2world = Transpose(camera->GetViewProjectionMatrixInv());

        context.cmd_list.CopyFrom(
            std::span<byte>((byte*)view_param, sizeof(ViewParam)), view_param_buffer->GetView()
        );
        context.cmd_list.AddCallback([view_param]() { MoerDelete(view_param); });

        GeometryPassBindlessParam param;
        param.color                  = float4(0., 0., 0., 0.);
        param.texture                = 0;
        param.buffer                 = 0;
        param.instance_data          = context.gpu_instance_info_handle;
        param.geometry_data          = context.gpu_geometry_info_handle;
        param.geometry_instance_data = context.gpu_geometry_instance_handle;
        param.camera_view_proj       = Transpose(camera->GetViewProjectionMatrix());

        context.cmd_list.GfxGeometryPass<GeometryPassPipeline>(view_param_buffer, context.bdls, param)
            .Draw(
                "Geometry Pass (MultiPasses)",
                Rect2D(0, 0, context.resolution.x, context.resolution.y),
                std::move(mesh_draw_datas_map),
                DepthAttachment(context.textures.depth_linear_sampler.tex->GetView().GetTexture()),
                ColorAttachment(context.textures.vbuffer.tex),
                ColorAttachment(context.textures.normal.tex),
                ColorAttachment(context.textures.tangent.tex),
                ColorAttachment(context.textures.uv.tex),
                ColorAttachment(context.textures.position.tex)
            );
        // 注：此处ColorAttachment的顺序需要和 GeometryPassPsoManager.cpp 中的 RHIColorAttachmentInfo::Preset 顺序一致
    }

public:
    BufferRef view_param_buffer; // no bindless
};

} // namespace Moer::Render::Raster