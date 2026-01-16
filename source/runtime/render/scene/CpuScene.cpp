#include "CpuScene.h"

#include "LogicalData.h"
#include "LogicalEnum.h"
#include "LogicalScene.h"
#include "SharedSceneStruct.h"
#include <entt/entt.hpp>

namespace Moer {

CpuScene::CpuScene(ecs::LogicalScene& m_logical_scene) : m_logical_scene(m_logical_scene) {
    /**
     * 注意初始化顺序：
     * - Materials必须在Meshes之前初始化，因为Mesh需要Material ID
     */
    InitializeLights();
    InitializeMaterials();
    InitializeMeshes();
}

void CpuScene::Update() {
    UpdateLights();
    UpdateMaterials();
    UpdateMeshes();
}

void CpuScene::InitializeLights() {
    auto& r = m_logical_scene.r();

    m_light_buf.clear();
    m_light_buf.reserve(r.view<const ecs::CLight>().size());

    m_map_light_entity_to_id.clear();

    auto emplace_light = [&](const auto entity, const GLight& light) {
        uint light_id = static_cast<uint>(m_light_buf.size());
        m_light_buf.emplace_back(light);
        m_map_light_entity_to_id[entity] = light_id; // build index cache
    };

    {
        const auto& view = r.view<const ecs::CLightDirectional>();
        view.each([&](const auto entity, const ecs::CLightDirectional& c_light_dir) {
            emplace_light(
                entity,
                GLight{
                    .type      = static_cast<uint8>(ECLightType::Directional),
                    .color     = c_light_dir.color,
                    .intensity = c_light_dir.intensity,
                    .direction = r.get<ecs::CTransform>(entity).rotation.Rotate(float3(0.f, 0.f, -1.f)),
                }
            );
        });
    }
    {
        const auto& view = r.view<const ecs::CLightPoint>();
        view.each([&](const auto entity, const ecs::CLightPoint& c_light_point) {
            emplace_light(
                entity,
                GLight{
                    .type      = static_cast<uint8>(ECLightType::Point),
                    .color     = c_light_point.color,
                    .intensity = c_light_point.intensity,
                    .position  = r.get<ecs::CTransform>(entity).translation,
                }
            );
        });
    }
    {
        const auto& view = r.view<const ecs::CLightAmbient>();
        view.each([&](const auto entity, const ecs::CLightAmbient& c_light_ambient) {
            emplace_light(
                entity,
                GLight{
                    .type      = static_cast<uint8>(ECLightType::Ambient),
                    .color     = c_light_ambient.color,
                    .intensity = c_light_ambient.intensity,
                }
            );
        });
    }
    {
        const auto& view = r.view<const ecs::CLightSpot>();
        // TODO
    }
    {
        const auto& view = r.view<const ecs::CLightEnvironment>();
        // TODO
    }
}

void CpuScene::UpdateLights() {
    // TODO
}

void CpuScene::InitializeMaterials() {
    auto& r = m_logical_scene.r();

    // materials

    m_material_buf.clear();
    m_material_buf.reserve(r.view<const ecs::CMaterial>().size());

    m_map_material_entity_to_id.clear();

    auto to_hdl = [&](const entt::entity entity) -> int64 {
        if (entity == entt::null) {
            return -1; // 不存在，应该使用factor
        } else {
            return -2; // 存在，但还没上传到Gpu
        }
    };

    r.view<const ecs::CMaterial>().each([&](const auto entity, const ecs::CMaterial& c_material) {
        GMaterial g_material{};

        g_material.albedo_factor    = c_material.albedo_factor;
        g_material.emissive_factor  = c_material.emissive_factor;
        g_material.metallic_factor  = c_material.metallic_factor;
        g_material.roughness_factor = c_material.roughness_factor;

        g_material.alpha_mode   = static_cast<uint8>(c_material.alpha_mode);
        g_material.alpha_cutoff = c_material.alpha_cutoff;

        g_material.normal_map_hdl             = to_hdl(c_material.normal_map_entt);
        g_material.ao_map_hdl                 = to_hdl(c_material.ao_map_entt);
        g_material.albedo_map_hdl             = to_hdl(c_material.albedo_map_entt);
        g_material.emissive_map_hdl           = to_hdl(c_material.emissive_map_entt);
        g_material.metallic_roughness_map_hdl = to_hdl(c_material.metallic_roughness_map_entt);

        // TODO: 在GpuScene中填充各种map的handle

        uint material_id = static_cast<uint>(m_material_buf.size());
        m_material_buf.emplace_back(g_material);
        m_map_entity_to_material_id[entity] = material_id; // build index cache
    });
}

void CpuScene::UpdateMaterials() {
    // TODO
}

void CpuScene::InitializeMeshes() {
    auto& r = m_logical_scene.r();

    /**
     * 这个函数比较复杂，分为以下几步：
     * 1. GPrimitive(CPrimitive)收集
     *    收集所有CPrimitive
     *    因为CPrimitive是渲染的最小单元，为了GPU Driven与GPU Cache命中率，所以会按照CPrimitive顺序渲染
     *    这里就先收集所有CPrimitive，并构建索引
     * 2. GInstance(CTransform)收集
     *    遍历所有Node，找到每个Node对应的所有CPrimitive，并收集对应的CTransform (GInstance)
     * 3. DrawIndexedCmdData填充
     * 
     * 从这个地方，就可以看出来ECS的强大之处了
     * - 想怎么遍历，就怎么遍历
     */

    m_primitive_buf.clear();
    m_draw_cmd_buf.clear();
    m_instance_buf.clear();

    m_map_primitive_entity_to_id.clear();
    m_primitive_id_to_transform_entt_arrays.clear();

    m_primitive_buf.reserve(r.view<const ecs::CPrimitive>().size());
    m_draw_cmd_buf.reserve(r.view<const ecs::CPrimitive>().size());

    // 注意！下面的大小是下界，最后 m_instance_buf大小 应该比CNode数量更多
    // - 可以思考一下，什么情况下，m_instance_buf.size() == CNode数量？
    // - 如果思考不出来的话，可以仔细翻一下下面的代码。答案就在下面的代码中
    m_instance_buf.reserve(r.view<const ecs::CNode>().size());

    {
        r.view<const ecs::CPrimitive>().each([&](const auto entity, const ecs::CPrimitive& c_primitive) {
            // build GPrimitive from CPrimitive

            GPrimitive g_primitive{};

            g_primitive.material_id = m_map_entity_to_material_id[c_primitive.material_id];

            if (c_primitive.position.is_valid) {
                g_primitive.attribute_mask |= GPrimitiveEAttributeMask::Position;
                g_primitive.position_offset = static_cast<uint>(c_primitive.position.offset);
            }
            if (c_primitive.packed_normal.is_valid) {
                g_primitive.attribute_mask |= GPrimitiveEAttributeMask::PackedNormal;
                g_primitive.packed_normal_offset = static_cast<uint>(c_primitive.packed_normal.offset);
            }
            if (c_primitive.packed_tangent.is_valid) {
                g_primitive.attribute_mask |= GPrimitiveEAttributeMask::PackedTangent;
                g_primitive.packed_tangent_offset = static_cast<uint>(c_primitive.packed_tangent.offset);
            }
            if (c_primitive.texcoord0.is_valid) {
                g_primitive.attribute_mask |= GPrimitiveEAttributeMask::Texcoord0;
                g_primitive.texcoord0_offset = static_cast<uint>(c_primitive.texcoord0.offset);
            }

            uint primitive_id = static_cast<uint>(m_primitive_buf.size());
            m_primitive_buf.emplace_back(g_primitive);
            m_map_primitive_entity_to_id[entity] = primitive_id; // build index cache

            m_primitive_id_to_transform_entt_arrays.emplace_back(); // prepare instance id array

            // m_draw_cmd_buf 在Instance收集完毕后再填充
        });
    }

    uint instance_cnt = 0;
    {
        Queue<entt::entity> node_queue;

        // Init root nodes
        r.view<const ecs::CTagRootNode>().each([&](const auto entity, const ecs::CTagRootNode&) {
            node_queue.push(entity);
        });

        while (!node_queue.empty()) {
            const entt::entity entity = node_queue.front();
            node_queue.pop();

            const ecs::CNode&      c_node      = r.get<ecs::CNode>(entity);
            const ecs::CTransform& c_transform = r.get<ecs::CTransform>(entity);

            // next
            if (c_node.first_child_entt != entt::null) {
                node_queue.push(c_node.first_child_entt);
            }
            if (c_node.next_sibling_entt != entt::null) {
                node_queue.push(c_node.next_sibling_entt);
            }

            if (r.all_of<ecs::CRenderable>(entity)) {
                const ecs::CRenderable& c_renderable = r.get<ecs::CRenderable>(entity);
                const ecs::CMesh&       c_mesh       = r.get<ecs::CMesh>(c_renderable.mesh_entt);

                // 遍历每一个CNode对应的CMesh的所有CPrimitive
                // - CNode       : CRenderable = 1 : 1
                // - CRenderable : CMesh       = 1 : 1
                // - CMesh       : CPrimitive  = 1 : N
                // 这里冗余的CRenderable是为了在内存中去重
                for (const entt::entity primitive_entt : c_mesh.primitive_entts) {
                    const uint primitive_id = m_map_primitive_entity_to_id[primitive_entt];

                    m_primitive_id_to_transform_entt_arrays[primitive_id].emplace_back(
                        GInstance{.world_transform = c_transform.d_world_transform}
                    );
                    instance_cnt++;
                }
            }
        }
    }

    {
        m_instance_buf.reserve(instance_cnt);

        for (auto& array : m_primitive_id_to_transform_entt_arrays) {
            for (const auto& instance : array) {
                m_instance_buf.emplace_back(instance);
            }
        }
    }
}

void CpuScene::UpdateMeshes() {
    // TODO
}

} // namespace Moer