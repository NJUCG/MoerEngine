#include "CpuScene.h"

#include "LogicalComponents.h"
#include "LogicalScene.h"
#include "log/LogSystem.h"
#include "shaderheaders/shared/scene/SharedSceneStruct.h"
#include <entt/entt.hpp>
#include <sstream>

namespace Moer {

// 将 LogicalScene 中的 Light 组件转换为 shader 可见的 GLight 数据。
static bool TryBuildGLight(const entt::registry& registry, entt::entity entity, GLight& out_light) {
    out_light = GLight{};

    if (registry.all_of<ecs::CLightDirectional>(entity)) {
        const auto& c_light = registry.get<ecs::CLightDirectional>(entity);
        out_light.color     = c_light.color;
        out_light.intensity = c_light.intensity;
        out_light.type      = static_cast<uint>(ELightType::Directional);
        out_light.direction = c_light.d_direction;
        return true;
    }
    if (registry.all_of<ecs::CLightPoint>(entity)) {
        const auto& c_light = registry.get<ecs::CLightPoint>(entity);
        out_light.color     = c_light.color;
        out_light.intensity = c_light.intensity;
        out_light.type      = static_cast<uint>(ELightType::Point);
        out_light.position  = c_light.d_position;
        return true;
    }
    if (registry.all_of<ecs::CLightAmbient>(entity)) {
        const auto& c_light = registry.get<ecs::CLightAmbient>(entity);
        out_light.color     = c_light.color;
        out_light.intensity = c_light.intensity;
        out_light.type      = static_cast<uint>(ELightType::Ambient);
        return true;
    }
    if (registry.all_of<ecs::CLightSpot>(entity)) {
        // TODO: CLightSpot 字段尚未定义；后续实现 spotlight 参数后，在这里补 GLight 转换。
        LOG_ERROR("CLightSpot is not implemented yet");
        return false;
    }
    if (registry.all_of<ecs::CLightEnvironment>(entity)) {
        // TODO: CLightEnvironment 的 GPU 表达尚未确定；后续拆分 IBL 资源管理时补转换。
        LOG_ERROR("CLightEnvironment is not implemented yet");
        return false;
    }

    LOG_ERROR("Entity is tagged as CLight but has no supported concrete light component.");
    return false;
}

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
    auto& r = m_logical_scene.r();

    if (HasLightDestroyRequest()) {
        // 删除链路当前走全量 rebuild，先保证正确性，后续再考虑增量回收
        RebuildLightsExcludingPendingDestroy();
    } else {
        CreateNeededLights();
        UpdateLights();
    }
    UpdateMaterials();
    UpdateMeshes();

    r.clear<ecs::CTagNeedCreateLight>();
    r.clear<ecs::CTagNeedUpdateLight>();
    r.clear<ecs::CTagNeedUpdateMaterial>();
    r.clear<ecs::CTagNeedUpdateTransform>();
}

uint CpuScene::GetPrimitiveId(entt::entity primitive_entt) const {
    auto it = m_map_primitive_entity_to_id.find(primitive_entt);
    if (it != m_map_primitive_entity_to_id.end()) {
        return it->second;
    }
    return UINT_MAX; // 返回无效值
}

uint CpuScene::GetFirstInstanceIndex(uint primitive_id) const {
    if (primitive_id >= m_primitive_id_to_first_instance_idx.size()) {
        return UINT_MAX;
    }
    // 检查该 Primitive 是否有 Instance
    if (m_primitive_id_to_transform_entt_arrays[primitive_id].empty()) {
        return UINT_MAX;
    }
    // O(1) 查询：直接使用前缀和数组
    return m_primitive_id_to_first_instance_idx[primitive_id];
}

uint CpuScene::GetPrimitiveCount() const {
    return static_cast<uint>(m_primitive_buf.size());
}

uint CpuScene::GetInstanceCountForPrimitive(uint primitive_id) const {
    if (primitive_id >= m_primitive_id_to_transform_entt_arrays.size()) {
        return 0;
    }
    return static_cast<uint>(m_primitive_id_to_transform_entt_arrays[primitive_id].size());
}

const GInstance& CpuScene::GetInstanceForPrimitive(uint primitive_id, uint instance_idx) const {
    assert(primitive_id < m_primitive_id_to_transform_entt_arrays.size());
    auto& arr = m_primitive_id_to_transform_entt_arrays[primitive_id];
    assert(instance_idx < arr.size());
    return arr[instance_idx];
}

uint CpuScene::GetLightCount() const {
    return static_cast<uint>(m_light_buf.size());
}

void CpuScene::InitializeLights() {
    auto& r = m_logical_scene.r();

    m_light_buf.clear();
    m_light_buf.reserve(r.view<const ecs::CLight>().size());

    m_map_light_entity_to_id.clear();

    const auto& view = r.view<const ecs::CLight>();
    view.each([&](const auto entity, const ecs::CLight&) {
        GLight light{};
        if (!TryBuildGLight(r, entity, light)) {
            return;
        }

        uint light_id = static_cast<uint>(m_light_buf.size());
        m_light_buf.emplace_back(light);
        m_map_light_entity_to_id[entity] = light_id;
    });
}

// 创建带 CTagNeedCreateLight 的新 Light cache slot。
void CpuScene::CreateNeededLights() {
    auto& r = m_logical_scene.r();

    auto view = r.view<const ecs::CTagNeedCreateLight, const ecs::CLight>();
    view.each([&](const auto entity, const ecs::CLight&) {
        if (m_map_light_entity_to_id.contains(entity)) {
            return;
        }

        GLight light{};
        if (!TryBuildGLight(r, entity, light)) {
            return;
        }

        uint light_id = static_cast<uint>(m_light_buf.size());
        m_light_buf.emplace_back(light);
        m_map_light_entity_to_id[entity] = light_id;
    });
}

void CpuScene::UpdateLights() {
    auto& r = m_logical_scene.r();

    // 查询所有需要同步到渲染场景的 Light 实体
    auto view = r.view<const ecs::CTagNeedUpdateLight, const ecs::CLight>();
    view.each([&](const auto entity, const ecs::CLight&) {
        if (r.all_of<ecs::CTagNeedDestroyLight>(entity)) {
            return;
        }

        auto light_id_it = m_map_light_entity_to_id.find(entity);
        if (light_id_it == m_map_light_entity_to_id.end()) {
            LOG_ERROR(
                "The light to update was not initialized. Runtime creation should use CTagNeedCreateLight."
            );
            return;
        }

        GLight light{};
        if (!TryBuildGLight(r, entity, light)) {
            return;
        }

        uint light_id         = light_id_it->second;
        m_light_buf[light_id] = light;
    });
}

// 检查本帧是否存在 light 删除请求
bool CpuScene::HasLightDestroyRequest() const {
    const auto& r    = m_logical_scene.r();
    const auto  view = r.view<const ecs::CTagNeedDestroyLight, const ecs::CLight>();
    return view.begin() != view.end();
}

// 当前通过全量重建 light cache 处理删除，优先保证结构正确性
void CpuScene::RebuildLightsExcludingPendingDestroy() {
    auto& r = m_logical_scene.r();

    m_light_buf.clear();
    m_light_buf.reserve(r.view<const ecs::CLight>().size());

    m_map_light_entity_to_id.clear();

    const auto& view = r.view<const ecs::CLight>();
    view.each([&](const auto entity, const ecs::CLight&) {
        if (r.all_of<ecs::CTagNeedDestroyLight>(entity)) {
            return;
        }

        GLight light{};
        if (!TryBuildGLight(r, entity, light)) {
            return;
        }

        uint light_id = static_cast<uint>(m_light_buf.size());
        m_light_buf.emplace_back(light);
        m_map_light_entity_to_id[entity] = light_id;
    });
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
        }
        return -2; // 存在，但还没上传到Gpu
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
        m_map_material_entity_to_id[entity] = material_id; // build index cache
    });
}

void CpuScene::UpdateMaterials() {
    auto& r = m_logical_scene.r();

    auto view = r.view<const ecs::CTagNeedUpdateMaterial, const ecs::CMaterial>();
    view.each([&](const auto entity, const ecs::CMaterial& c_material) {
        auto mat_id_it = m_map_material_entity_to_id.find(entity);
        if (mat_id_it == m_map_material_entity_to_id.end()) {
            LOG_ERROR(
                "The material to update was not initialized. Adding new materials is not supported yet."
            );
            return;
        }

        const uint material_id = mat_id_it->second;

        GMaterial g_material        = m_material_buf[material_id];
        g_material.albedo_factor    = c_material.albedo_factor;
        g_material.emissive_factor  = c_material.emissive_factor;
        g_material.metallic_factor  = c_material.metallic_factor;
        g_material.roughness_factor = c_material.roughness_factor;
        g_material.alpha_mode       = static_cast<uint8>(c_material.alpha_mode);
        g_material.alpha_cutoff     = c_material.alpha_cutoff;

        m_material_buf[material_id] = g_material;
    });
}

void CpuScene::InitializeMeshes() {
    auto& r = m_logical_scene.r();

    /**
     * 这个函数比较复杂，分为以下几步：
     * 1. GPrimitive(CPrimitive)收集
     *    收集所有CPrimitive
     *    因为CPrimitive是渲染的最小单元，为了GPU Driven与GPU Cache命中率，所以会按照CPrimitive顺序渲染
     *    这里就先收集所有CPrimitive，并构建索引
     * 2. GInstance(CNode)收集
     *    遍历所有Node，找到每个Node对应的所有CPrimitive，并收集对应的CNode (GInstance)
     * 3. DrawIndexedCmdData填充
     * 
     * 从这个地方，就可以看出来ECS的强大之处了
     * - 想怎么遍历，就怎么遍历
     */

    m_primitive_buf.clear();
    m_draw_cmd_buf.clear();
    m_instance_buf.clear();

    m_map_primitive_entity_to_id.clear();
    m_map_transform_entity_to_instance_slots.clear();
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

            g_primitive.material_idx = m_map_material_entity_to_id.at(c_primitive.material_entt);

            if (c_primitive.position.is_valid) {
                g_primitive.attribute_mask |= GPrimitiveEAttributeMask::Position;
                g_primitive.position_start_idx = static_cast<uint>(c_primitive.position.start_idx);
            }
            if (c_primitive.packed_normal.is_valid) {
                g_primitive.attribute_mask |= GPrimitiveEAttributeMask::PackedNormal;
                g_primitive.packed_normal_start_idx = static_cast<uint>(c_primitive.packed_normal.start_idx);
            }
            if (c_primitive.packed_tangent.is_valid) {
                g_primitive.attribute_mask |= GPrimitiveEAttributeMask::PackedTangent;
                g_primitive.packed_tangent_start_idx =
                    static_cast<uint>(c_primitive.packed_tangent.start_idx);
            }
            if (c_primitive.texcoord0.is_valid) {
                g_primitive.attribute_mask |= GPrimitiveEAttributeMask::Texcoord0;
                g_primitive.texcoord0_start_idx = static_cast<uint>(c_primitive.texcoord0.start_idx);
            }
            if (c_primitive.index.is_valid) {
                g_primitive.index_start_idx = static_cast<uint>(c_primitive.index.start_idx);
            } else {
                g_primitive.index_start_idx = 0; // 默认值
            }

            // AABB for frustum culling
            g_primitive.aabb_min = c_primitive.aabb.min;
            g_primitive.aabb_max = c_primitive.aabb.max;

            // 验证 AABB 有效性
            if (!c_primitive.aabb.IsValid()) {
                LOG_WARNING(
                    "Primitive {} has invalid AABB: min=({},{},{}), max=({},{},{})",
                    m_primitive_buf.size(),
                    g_primitive.aabb_min.x,
                    g_primitive.aabb_min.y,
                    g_primitive.aabb_min.z,
                    g_primitive.aabb_max.x,
                    g_primitive.aabb_max.y,
                    g_primitive.aabb_max.z
                );
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
        // 注：这里CTagRootNode是空结构体，entt的each只会传递entity参数，不会传递Component引用
        //    因此，如果添加了...ecs::CTagRootNode&，就会编译错误
        r.view<const ecs::CTagRootNode>().each([&](const auto entity) {
            node_queue.push(entity);
        });

        while (!node_queue.empty()) {
            const entt::entity entity = node_queue.front();
            node_queue.pop();

            const ecs::CNode& c_node = r.get<ecs::CNode>(entity);

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
                    const uint instance_idx_in_primitive =
                        static_cast<uint>(m_primitive_id_to_transform_entt_arrays[primitive_id].size());

                    m_primitive_id_to_transform_entt_arrays[primitive_id].emplace_back(
                        GInstance{
                            .world_transform = Transpose(c_node.d_world_transform), // HLSL列主序，需要转置
                            .primitive_id    = primitive_id // 存储 Primitive ID 用于反向映射
                        }
                    );
                    m_map_transform_entity_to_instance_slots[entity].emplace_back(
                        InstanceSlot{
                            .primitive_id              = primitive_id,
                            .instance_idx_in_primitive = instance_idx_in_primitive,
                            .flat_instance_idx         = UINT_MAX,
                        }
                    );
                    instance_cnt++;
                }
            }
        }
    }

    {
        m_instance_buf.reserve(instance_cnt);

        // 构建前缀和数组：m_primitive_id_to_first_instance_idx[i] = 前 i 个 Primitive 的 Instance 总数
        m_primitive_id_to_first_instance_idx.clear();
        m_primitive_id_to_first_instance_idx.resize(m_primitive_id_to_transform_entt_arrays.size());
        uint prefix_sum = 0;
        for (uint i = 0; i < m_primitive_id_to_transform_entt_arrays.size(); ++i) {
            m_primitive_id_to_first_instance_idx[i] = prefix_sum;
            for (const auto& instance : m_primitive_id_to_transform_entt_arrays[i]) {
                m_instance_buf.emplace_back(instance);
            }
            prefix_sum += static_cast<uint>(m_primitive_id_to_transform_entt_arrays[i].size());
        }

        for (auto& [_, instance_slots] : m_map_transform_entity_to_instance_slots) {
            for (InstanceSlot& instance_slot : instance_slots) {
                instance_slot.flat_instance_idx =
                    m_primitive_id_to_first_instance_idx[instance_slot.primitive_id] +
                    instance_slot.instance_idx_in_primitive;
            }
        }
    }

    // {
    //     // log
    //     std::stringstream ss;
    //     ss << "\nArray of GInstances Arrays: \n";
    //     for (uint i = 0; i < m_primitive_id_to_transform_entt_arrays.size(); ++i) {
    //         ss << "\tPrimitive " << i << ": " << m_primitive_id_to_transform_entt_arrays[i].size()
    //            << " instances\n";
    //         for (const auto& instance : m_primitive_id_to_transform_entt_arrays[i]) {
    //             ss << "\t\tInstance " << instance.world_transform.ToString(false, 3) << "\n";
    //         }
    //     }
    //     LOG_INFO("{}", ss.str());
    // }

    {
        // 3. DrawIndexedCmdData填充
        //
        // 需要为每个 Primitive 填充 DrawIndexedCmdData
        // - index_cnt: 从 CPrimitive.index_count 获取
        // - instance_cnt: 从 m_primitive_id_to_transform_entt_arrays[i].size() 获取
        // - first_index: 从 CPrimitive.index.offset 转换为索引偏移 (除以 sizeof(uint32))
        // - vertex_offset: 通常为 0，因为顶点数据通过 GPrimitive 中的 offset 字段访问
        // - first_instance: 前面所有 Primitive 的 Instance 总数

        m_draw_cmd_buf.clear();
        m_draw_cmd_buf.reserve(m_primitive_buf.size());

        // 再次遍历 CPrimitive，顺序与第一次遍历一致
        r.view<const ecs::CPrimitive>().each([&](const auto entity, const ecs::CPrimitive& c_primitive) {
            const uint primitive_id = m_map_primitive_entity_to_id.at(entity);
            const uint instance_cnt =
                static_cast<uint>(m_primitive_id_to_transform_entt_arrays[primitive_id].size());

            Render::DrawIndexedCmdData draw_cmd_data{};

            // index_cnt: 索引数量
            draw_cmd_data.index_cnt = c_primitive.index_count;

            // instance_cnt: 该 Primitive 有多少个 Instance
            draw_cmd_data.instance_cnt = instance_cnt;

            // first_index: 第一个索引在 Index Buffer 中的偏移
            // CPrimitive.index.offset 是字节偏移，需要转换为索引偏移
            if (c_primitive.index.is_valid) {
                draw_cmd_data.first_index = c_primitive.index.start_idx;
            } else {
                draw_cmd_data.first_index = 0;
                assert(false && "CPrimitive.index is invalid");
            }

            // vertex_offset: 顶点偏移，通常为 0
            // 因为顶点数据通过 GPrimitive 中的 position_offset 等字段访问
            draw_cmd_data.vertex_offset = 0;

            // first_instance: 直接使用前缀和数组（O(1)查询）
            draw_cmd_data.first_instance = m_primitive_id_to_first_instance_idx[primitive_id];

            m_draw_cmd_buf.emplace_back(draw_cmd_data);
        });
    }

    {
        // assert

        assert(
            m_primitive_buf.size() == r.view<const ecs::CPrimitive>().size() && "Primitive count mismatch"
        );
        assert(
            m_primitive_id_to_transform_entt_arrays.size() == r.view<const ecs::CPrimitive>().size() &&
            "Primitive ID to Transform Entity Arrays count mismatch"
        );

        assert(m_instance_buf.size() == instance_cnt);

        assert(m_draw_cmd_buf.size() == m_primitive_buf.size());
    }
}

void CpuScene::UpdateMeshes() {
    auto& r = m_logical_scene.r();

    auto view = r.view<const ecs::CTagNeedUpdateTransform, const ecs::CNode>();
    view.each([&](const auto entity, const ecs::CNode& c_node) {
        auto instance_slots_it = m_map_transform_entity_to_instance_slots.find(entity);
        if (instance_slots_it == m_map_transform_entity_to_instance_slots.end()) {
            return;
        }

        for (const InstanceSlot& instance_slot : instance_slots_it->second) {
            assert(instance_slot.primitive_id < m_primitive_id_to_transform_entt_arrays.size());
            assert(
                instance_slot.instance_idx_in_primitive <
                m_primitive_id_to_transform_entt_arrays[instance_slot.primitive_id].size()
            );
            assert(instance_slot.flat_instance_idx < m_instance_buf.size());

            GInstance instance{
                .world_transform = Transpose(c_node.d_world_transform),
                .primitive_id    = instance_slot.primitive_id,
            };

            m_primitive_id_to_transform_entt_arrays[instance_slot.primitive_id]
                                                   [instance_slot.instance_idx_in_primitive] = instance;
            m_instance_buf[instance_slot.flat_instance_idx]                                  = instance;
        }
    });
}

} // namespace Moer