#pragma once

#include "rhi/RHI.h"
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

    // static UnorderedMap<VertexFactory, Array<MeshDrawData>>
    // GetDrawMeshDatasMap(RasterContext& context, bool is_shadow_depth_pass) {
    //     // 将每种不同顶点类型的Mesh分发到不同的MeshDrawDatas中。在Draw时，调用不同的PSO处理对应的MeshDrawDatas！
    //     // 所以，下面这段scene.ForEach的代码，也可以理解成将 “按Entity分组的Mesh” 转换为 “按顶点类型分组的Mesh的DrawDatas”
    //     UnorderedMap<VertexFactory, Array<MeshDrawData>> mesh_draw_datas_map;

    //     std::span<const UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>>
    //         vertex_buffer_maps = context.scene.GetVertexBufferViews();
    //     std::span<const UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>> index_buffer_maps =
    //         context.scene.GetIndexBufferViews();

    //     uint geom_idx = 0;
    //     context.scene.ForEach([&](Entity _entity) {
    //         auto& mesh = RenderableManager::Get().GetMeshInfo(_entity);

    //         const UnorderedMap<VertexAttributesBitmask, Array<Render::VertexBuffer>>& vertex_buffer_map =
    //             vertex_buffer_maps[mesh->global_mesh_idx];
    //         const UnorderedMap<VertexAttributesBitmask, Render::IndexBuffer>& index_buffer_map =
    //             index_buffer_maps[mesh->global_mesh_idx];

    //         // LOG_INFO("  Entity <-> Mesh info addr {}", (void*)mesh.get());

    //         for (const auto& [bitmask, vertex_buffer] :
    //              vertex_buffer_map) { // for each vertex buffer (index buffer is the same)

    //             if (bitmask == 1) {
    //                 continue;
    //                 // TODO: 神人，为什么一些场景会有bitmask==1的情况？只有position？
    //             }
    //             if (bitmask == 3) {
    //                 continue;
    //                 // TODO: bitmask == 3，即只有position和normal，这些Mesh应该是用于动画等功能的。不应该被正常渲染
    //             }

    //             const auto& index_buffer = index_buffer_map.at(bitmask);

    //             VertexFactory factory(bitmask, is_shadow_depth_pass);

    //             Array<MeshDrawData>& mesh_draw_array = mesh_draw_datas_map[factory];
    //             mesh_draw_array.emplace_back(vertex_buffer, index_buffer);
    //             // 注意，请不要改变Array内部的顺序
    //             // => 逻辑关联处：RHICommand.h -> MeshDrawData
    //         }

    //         for (uint i = 0; i < mesh->geometries.size(); i++) {
    //             uint                idx  = geom_idx++;
    //             const MeshGeometry& geom = *mesh->geometries[i];

    //             uint first_index    = geom.local_idx_offset;
    //             uint index_count    = geom.local_idx_count;
    //             uint first_vertex   = geom.local_vtx_offset;
    //             uint first_instance = idx;

    //             auto bitmask = geom.mesh_buffers->vertex_factory_buffers.GetAttributesBitmask();

    //             if (bitmask == 1) {
    //                 continue;
    //                 // TODO: 神人，为什么一些场景会有bitmask==1的情况？只有position？
    //             }
    //             if (bitmask == 3) {
    //                 continue;
    //                 // TODO: bitmask == 3，即只有position和normal，这些Mesh应该是用于动画等功能的。不应该被正常渲染
    //             }

    //             VertexFactory factory(bitmask, is_shadow_depth_pass);

    //             Array<MeshDrawData>& mesh_draw_array = mesh_draw_datas_map[factory];
    //             mesh_draw_array.back().EmplaceDrawIndexed(
    //                 first_index, index_count, first_vertex, first_instance
    //             );
    //         }
    //     });

    //     return mesh_draw_datas_map;
    // }

    // TODO: 和Raytracing中对应部分合并
    static void InitRaytracingScene(RasterContext& context, Array<RaytracingGeometryRef>& rt_geometries) {
        auto& scene    = context.scene;
        auto& device   = context.device;
        auto& rt_scene = context.rt_scene;
        auto& cmd_list = context.cmd_list;

        Array<AccelerationStructureBuildParam> build_params;

        rt_geometries.reserve(scene.GetEntityCount());
        build_params.reserve(scene.GetEntityCount());

        scene.ForEach([&](Entity _entity) {
            auto&                  mesh = RenderableManager::Get().GetMeshInfo(_entity);
            RaytracingGeometryInfo rt_geo_info{};
            rt_geo_info.build_flags   = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
            rt_geo_info.vertex_format = PF_R32G32B32_SFLOAT;
            rt_geo_info.index_type    = IET_UINT32;

            for (uint i = 0; i < mesh->geometries.size(); i++) {
                uint vtx_offset = mesh->geometries[i]->local_vtx_offset;
                uint vtx_count  = mesh->geometries[i]->local_vtx_count;
                uint idx_offset = mesh->geometries[i]->local_idx_offset;
                uint idx_count  = mesh->geometries[i]->local_idx_count;
                auto vtx_buffer = mesh->geometries[i]->mesh_buffers->vertex_buffer;
                auto idx_buffer = mesh->geometries[i]->mesh_buffers->index_buffer;

                rt_geo_info.segments.emplace_back(
                    0,                                         // vertex_offset
                    0,                                         // index_offset
                    vtx_offset,                                // first_vertex
                    vtx_count,                                 // vertex_count
                    sizeof(float3),                            // vertex_stride
                    idx_offset / 3,                            // first_primitive
                    idx_count / 3,                             // primitive_count
                    vtx_buffer,                                // vertex_buffer
                    idx_buffer,                                // index_buffer
                    RTGT_TRIANGLES,                            // type
                    ERayTracingGeometryFlags::GEOMETRY_OPAQUE, // flags
                    false,                                     // b_force_opaque
                    false,                                     // b_cull_back_face
                    false                                      // b_flip_face
                );
            }

            RaytracingGeometryRef blas = device.CreateRaytracingGeometry(rt_geo_info);
            rt_geometries.push_back(blas);

            auto& instance     = rt_scene->AddInstance();
            instance.geom      = blas;
            instance.transform = TransformManager::Get().Get(_entity).GetMatrix3x4();

            instance.flag.need_create = true;
            instance.custom_index     = instance.instance_id;
            instance.visible_mask     = RTVM_ALL;
            rt_scene->MarkModified(instance.instance_id);
            build_params.push_back({blas, ERaytracingBuildMode::BUILD});
        });

        cmd_list.BuildAccelerationStructures(std::move(build_params));
        cmd_list.UpdateRaytracingScene(rt_scene);
    }

    static void UpdateRaytracingScene(RasterContext& context) {

        auto& scene    = context.scene;
        auto& rt_scene = context.rt_scene;
        auto& cmd_list = context.cmd_list;

        for (size_t i = 0; i < scene.GetEntityCount(); i++) {
            auto& instance = rt_scene->GetInstance(i);
            rt_scene->MarkModified(instance.instance_id);
        }
        cmd_list.UpdateRaytracingScene(rt_scene);
    }
};

} // namespace Moer::Render::Raster