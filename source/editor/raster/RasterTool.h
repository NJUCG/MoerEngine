#pragma once

#include "rhi/RHI.h"
#include "scene/RenderableManager.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

#include "RasterResource.h"

namespace Moer::Render::Raster {

class RasterTool {
public:
    static Array<SingleDrawParam> GetFullScreenDrawDatas() {
        Array<SingleDrawParam> full_screen_draw_datas;
        full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
        return full_screen_draw_datas;
    }

    static UnorderedMap<VertexFactory, Array<MeshDrawData>>
    GetDrawMeshDatasMap(RasterContext& context, bool is_shadow_depth_pass) {
        // 将每种不同顶点类型的Mesh分发到不同的MeshDrawDatas中。在Draw时，调用不同的PSO处理对应的MeshDrawDatas！
        // 所以，下面这段scene.ForEach的代码，也可以理解成将 “按Entity分组的Mesh” 转换为 “按顶点类型分组的Mesh的DrawDatas”
        UnorderedMap<VertexFactory, Array<MeshDrawData>> mesh_draw_datas_map;

        std::span<const UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>>
            vertex_buffer_maps = context.scene.GetVertexBufferViews();
        std::span<const UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>> index_buffer_maps =
            context.scene.GetIndexBufferViews();

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

                VertexFactory factory(bitmask, is_shadow_depth_pass);

                Array<MeshDrawData>& mesh_draw_array = mesh_draw_datas_map[factory];
                mesh_draw_array.emplace_back(vertex_buffer, index_buffer);
                // 注意，请不要改变Array内部的顺序
                // => 逻辑关联处：RHICommand.h -> MeshDrawData
            }

            for (uint i = 0; i < mesh->geometries.size(); i++) {
                uint                idx  = geom_idx++;
                const MeshGeometry& geom = *mesh->geometries[i];

                uint first_index    = geom.local_idx_offset;
                uint index_count    = geom.local_idx_count;
                uint first_vertex   = geom.local_vtx_offset;
                uint first_instance = idx;

                auto bitmask = geom.mesh_buffers->vertex_factory_buffers.GetAttributesBitmask();

                if (bitmask == 3) {
                    continue;
                    // TODO: bitmask == 3，即只有position和normal，这些Mesh应该是用于动画等功能的。不应该被正常渲染
                }

                VertexFactory factory(bitmask, is_shadow_depth_pass);

                Array<MeshDrawData>& mesh_draw_array = mesh_draw_datas_map[factory];
                mesh_draw_array.back().EmplaceDrawIndexed(
                    first_index, index_count, first_vertex, first_instance
                );
            }
        });

        return mesh_draw_datas_map;
    }
};

} // namespace Moer::Render::Raster