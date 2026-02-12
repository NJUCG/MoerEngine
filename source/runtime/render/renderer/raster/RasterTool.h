#pragma once

#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"

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

    // TODO: InitRaytracingScene 和 UpdateRaytracingScene 已迁移到 GpuScene
    // 请使用 scene.GetGpuScene().InitRaytracingScene(cmd_list) 和 scene.GetGpuScene().UpdateRaytracingScene(cmd_list)
    // 以下代码已废弃，保留用于参考
    /*
    static void InitRaytracingScene(RasterContext& context, Array<RaytracingGeometryRef>& rt_geometries) {
        auto& device   = context.device;
        auto& rt_scene = context.rt_scene;
        auto& cmd_list = context.cmd_list;

        // 获取 LogicalScene 和 GpuScene 资源
        auto&       r       = context.scene.r();
        const auto& gpu_res = context.scene.gpu_scene_res();

        // 获取共享的 vertex 和 index buffers
        BufferRef position_buf_ref = gpu_res.position_buf.buf;
        BufferRef index_buf_ref    = gpu_res.index_buf.buf;

        Array<AccelerationStructureBuildParam> build_params;

        // 遍历所有有 CRenderable 的 entity
        auto renderable_view = r.view<const ecs::CRenderable, const ecs::CTransform>();
        // 使用 size_hint() 估算大小（EnTT view 没有 size() 方法）
        rt_geometries.reserve(renderable_view.size_hint());
        build_params.reserve(renderable_view.size_hint());

        renderable_view.each(
            [&](const auto entity, const ecs::CRenderable& c_renderable, const ecs::CTransform& c_transform) {
                // 获取 CMesh
                if (!r.valid(c_renderable.mesh_entt) || !r.all_of<ecs::CMesh>(c_renderable.mesh_entt)) {
                    LOG_WARNING("Invalid mesh entity: {}", static_cast<uint>(c_renderable.mesh_entt));
                    return; // Skip invalid mesh
                }

                const ecs::CMesh& c_mesh = r.get<ecs::CMesh>(c_renderable.mesh_entt);

                // 为每个 CRenderable 创建一个 BLAS（包含对应CRenderable的所有 primitive，并且会重复创建）
                // TODO: 去重
                RaytracingGeometryInfo rt_geo_info{};
                rt_geo_info.build_flags   = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
                rt_geo_info.vertex_format = PF_R32G32B32_SFLOAT;
                rt_geo_info.index_type    = IET_UINT32;

                // 遍历该 Mesh 的所有 Primitive
                for (const entt::entity primitive_entt : c_mesh.primitive_entts) {
                    if (!r.valid(primitive_entt) || !r.all_of<ecs::CPrimitive>(primitive_entt)) {
                        LOG_WARNING("Invalid primitive entity: {}", static_cast<uint>(primitive_entt));
                        continue; // Skip invalid primitive
                    }

                    const ecs::CPrimitive& c_primitive = r.get<ecs::CPrimitive>(primitive_entt);

                    // 检查必要的 buffer view 是否有效
                    if (!c_primitive.position.is_valid || !c_primitive.index.is_valid) {
                        continue; // Skip primitive without valid position/index
                    }

                    // 从 CPrimitive 获取顶点和索引信息
                    uint vtx_offset = c_primitive.position.start_idx; // element offset
                    uint vtx_count  = c_primitive.vertex_count;
                    uint idx_offset = c_primitive.index.start_idx; // element offset (in indices)
                    uint idx_count  = c_primitive.index_count;

                    // 使用 GpuScene 的共享 buffers（RaytracingSegment 需要 BufferRef，不是 BufferView）
                    rt_geo_info.segments.emplace_back(
                        0,                // vertex_offset
                        0,                // index_offset
                        vtx_offset,       // first_vertex
                        vtx_count,        // vertex_count
                        sizeof(float3),   // vertex_stride
                        idx_offset / 3,   // first_primitive (indices are uint32, 3 per triangle)
                        idx_count / 3,    // primitive_count
                        position_buf_ref, // vertex_buffer (BufferRef)
                        index_buf_ref,    // index_buffer (BufferRef)
                        RTGT_TRIANGLES,   // type
                        ERayTracingGeometryFlags::GEOMETRY_OPAQUE, // flags
                        false,                                     // b_force_opaque
                        false,                                     // b_cull_back_face
                        false                                      // b_flip_face
                    );
                }

                // 如果没有有效的 segments，跳过这个 entity
                if (rt_geo_info.segments.empty()) {
                    return;
                }

                // 创建 BLAS
                RaytracingGeometryRef blas = device.CreateRaytracingGeometry(rt_geo_info);
                rt_geometries.push_back(blas);

                // 添加 instance
                auto& instance = rt_scene->AddInstance();
                instance.geom  = blas;
                // 将 float4x4 转换为 Matrix3x4f
                instance.transform = Matrix3x4f(
                    c_transform.d_world_transform.r0,
                    c_transform.d_world_transform.r1,
                    c_transform.d_world_transform.r2
                );

                instance.flag.need_create = true;
                instance.custom_index     = instance.instance_id;
                instance.visible_mask     = RTVM_ALL;
                rt_scene->MarkModified(instance.instance_id);
                
                // 建立 entity -> instance_idx 映射
                rt_scene->SetEntityToInstanceMapping(entity, instance.array_idx);
                
                build_params.push_back({blas, ERaytracingBuildMode::BUILD});
            }
        );

        cmd_list.BuildAccelerationStructures(std::move(build_params));
        cmd_list.UpdateRaytracingScene(rt_scene);
    }
    */

    /*
    static void UpdateRaytracingScene(RasterContext& context) {
        auto& rt_scene = context.rt_scene;
        auto& cmd_list = context.cmd_list;
        auto& r        = context.scene.r();

        // 遍历所有有 CRenderable 的 entity，更新对应的 instance transform
        auto renderable_view = r.view<const ecs::CRenderable, const ecs::CTransform>();

        renderable_view.each([&](const auto entity, const ecs::CRenderable& c_renderable, const ecs::CTransform& c_transform) {
            // 通过映射获取 instance_idx
            uint instance_idx = rt_scene->GetInstanceIdxFromEntity(entity);
            if (instance_idx == UINT_MAX) {
                return; // Skip entity without valid instance mapping
            }

            // 更新 transform
            auto& instance = rt_scene->GetInstance(instance_idx);
            instance.transform = Matrix3x4f(
                c_transform.d_world_transform.r0,
                c_transform.d_world_transform.r1,
                c_transform.d_world_transform.r2
            );
            rt_scene->MarkModified(instance.instance_id);
        });

        cmd_list.UpdateRaytracingScene(rt_scene);
    }
    */
};

} // namespace Moer::Render::Raster