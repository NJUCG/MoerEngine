#include "CpuScene.h"

#include "GpuSceneUpdate.h"
#include "LogicalComponents.h"
#include "LogicalScene.h"
#include "log/LogSystem.h"
#include "scene/NodeNameUtils.h"
#include "shaderheaders/shared/scene/SharedSceneStruct.h"
#include <entt/entt.hpp>

namespace Moer {

static bool IsNodeEffectivelyVisibleInGame(const entt::registry& registry, entt::entity entity) {
    if (entity == entt::null || !registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
        return false;
    }

    entt::entity current = entity;
    while (current != entt::null) {
        if (!registry.valid(current) || !registry.all_of<ecs::CNode>(current)) {
            return false;
        }

        if (const auto* visibility = registry.try_get<ecs::CVisibility>(current);
            visibility && !visibility->visible_in_game) {
            return false;
        }

        current = registry.get<ecs::CNode>(current).parent_entt;
    }

    return true;
}

// 将 LogicalScene 中的 Light 组件转换为 shader 可见的 GLight 数据。
static bool TryBuildGLight(const entt::registry& registry, entt::entity entity, GLight& out_light) {
    out_light = GLight{};

    if (registry.all_of<ecs::CNode>(entity) && !IsNodeEffectivelyVisibleInGame(registry, entity)) {
        out_light.type = static_cast<uint>(ELightType::None);
        return true;
    }

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

static int64 ToPendingTextureHandle(const entt::entity entity) {
    if (entity == entt::null) {
        return -1;
    }
    return -2;
}

static GMaterial BuildGMaterialFromComponent(const ecs::CMaterial& c_material) {
    GMaterial g_material{};

    g_material.albedo_factor    = c_material.albedo_factor;
    g_material.emissive_factor  = c_material.emissive_factor;
    g_material.metallic_factor  = c_material.metallic_factor;
    g_material.roughness_factor = c_material.roughness_factor;

    g_material.alpha_mode   = static_cast<uint8>(c_material.alpha_mode);
    g_material.alpha_cutoff = c_material.alpha_cutoff;

    g_material.normal_map_hdl             = ToPendingTextureHandle(c_material.normal_map_entt);
    g_material.ao_map_hdl                 = ToPendingTextureHandle(c_material.ao_map_entt);
    g_material.albedo_map_hdl             = ToPendingTextureHandle(c_material.albedo_map_entt);
    g_material.emissive_map_hdl           = ToPendingTextureHandle(c_material.emissive_map_entt);
    g_material.metallic_roughness_map_hdl = ToPendingTextureHandle(c_material.metallic_roughness_map_entt);

    return g_material;
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
    CreateNeededMaterials();
    UpdateMaterials();
    if (HasMeshRebuildRequest()) {
        // Renderable create/destroy 当前走全量 rebuild，先保证 instance / draw command 结构正确
        RebuildMeshes();
    } else {
        UpdateMeshes();
    }

    r.clear<ecs::CTagNeedCreateLight>();
    r.clear<ecs::CTagNeedCreateMaterial>();
    r.clear<ecs::CTagNeedUpdateLight>();
    r.clear<ecs::CTagNeedUpdateMaterial>();
    r.clear<ecs::CTagNeedUpdateTransform>();
    r.clear<ecs::CTagNeedRebuildMesh>();
    r.clear<ecs::CTagNeedRebuildRtBlas>();
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

    r.view<const ecs::CMaterial>().each([&](const auto entity, const ecs::CMaterial& c_material) {
        GMaterial g_material = BuildGMaterialFromComponent(c_material);

        uint material_id = static_cast<uint>(m_material_buf.size());
        m_material_buf.emplace_back(g_material);
        m_map_material_entity_to_id[entity] = material_id; // build index cache
    });
}

// 创建带 CTagNeedCreateMaterial 的新 Material cache slot。
void CpuScene::CreateNeededMaterials() {
    auto& r = m_logical_scene.r();

    auto view = r.view<const ecs::CTagNeedCreateMaterial, const ecs::CMaterial>();
    view.each([&](const auto entity, const ecs::CMaterial& c_material) {
        if (m_map_material_entity_to_id.contains(entity)) {
            return;
        }

        uint material_id = static_cast<uint>(m_material_buf.size());
        m_material_buf.emplace_back(BuildGMaterialFromComponent(c_material));
        m_map_material_entity_to_id[entity] = material_id;
    });
}

void CpuScene::UpdateMaterials() {
    auto& r = m_logical_scene.r();

    auto view = r.view<const ecs::CTagNeedUpdateMaterial, const ecs::CMaterial>();
    view.each([&](const auto entity, const ecs::CMaterial& c_material) {
        auto mat_id_it = m_map_material_entity_to_id.find(entity);
        if (mat_id_it == m_map_material_entity_to_id.end()) {
            LOG_ERROR(
                "The material to update was not initialized. Runtime creation should use CTagNeedCreateMaterial."
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
    m_cluster_group_buf.clear();

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

            // Cluster LOD 字段（暂存本地 group_id，之后在 CMesh 遍历时偏移为全局索引）
            g_primitive.cluster_group_id   = c_primitive.cluster_group_id;
            g_primitive.cluster_refined_id = c_primitive.cluster_refined_id;

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

    // Cluster LOD: 收集所有 CMesh 的 cluster group 到全局 m_cluster_group_buf，
    // 并偏移 GPrimitive 中的 group_id / refined_id 为全局索引
    {
        r.view<const ecs::CMesh>().each([&](const auto mesh_entity, const ecs::CMesh& c_mesh) {
            if (c_mesh.cluster_groups.empty()) return;

            const int global_group_offset = static_cast<int>(m_cluster_group_buf.size());

            for (const auto& g : c_mesh.cluster_groups) {
                int parent_id = g.parent_group_id;
                // 偏移为全局索引（多 mesh 场景下）
                if (parent_id >= 0) parent_id += global_group_offset;

                m_cluster_group_buf.push_back(GClusterGroup{
                    .simplified_center = g.simplified_center,
                    .simplified_radius = g.simplified_radius,
                    .simplified_error  = g.simplified_error,
                    .depth             = g.depth,
                    .parent_group_id   = parent_id,
                });
            }

            // 偏移该 mesh 下所有 primitive 的 group ID
            for (const auto prim_entt : c_mesh.primitive_entts) {
                auto it = m_map_primitive_entity_to_id.find(prim_entt);
                if (it == m_map_primitive_entity_to_id.end()) continue;

                GPrimitive& gp = m_primitive_buf[it->second];
                if (gp.cluster_group_id >= 0)   gp.cluster_group_id   += global_group_offset;
                if (gp.cluster_refined_id >= 0) gp.cluster_refined_id += global_group_offset;
            }
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

            if (r.all_of<ecs::CRenderable>(entity) && IsNodeEffectivelyVisibleInGame(r, entity)) {
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

// 检查本帧是否存在 renderable 结构变化请求
bool CpuScene::HasMeshRebuildRequest() const {
    const auto& r    = m_logical_scene.r();
    const auto  view = r.view<const ecs::CTagNeedRebuildMesh>();
    return view.begin() != view.end();
}

// 当前通过全量重建 mesh instance cache 处理 renderable create/destroy，优先保证结构正确性
void CpuScene::RebuildMeshes() {
    InitializeMeshes();
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

Render::GpuSceneUpdate CpuScene::BuildGpuSceneUpdate(
    bool full_rebuild,
    bool update_lights,
    bool update_materials,
    bool update_meshes,
    bool rebuild_rt_blas,
    bool update_rt_instances
) const {
    Render::GpuSceneUpdate update{};
    update.full_rebuild     = full_rebuild;
    update.update_lights    = full_rebuild || update_lights;
    update.update_materials = full_rebuild || update_materials;
    update.update_meshes    = full_rebuild || update_meshes;

    if (full_rebuild || rebuild_rt_blas) {
        update.raytracing_update = Render::EGpuSceneRaytracingUpdate::RebuildBlas;
    } else if (update_meshes) {
        update.raytracing_update = Render::EGpuSceneRaytracingUpdate::RebuildTlas;
    } else if (update_rt_instances) {
        update.raytracing_update = Render::EGpuSceneRaytracingUpdate::UpdateInstances;
    }

    const auto& registry = m_logical_scene.r();
    auto to_key = [](entt::entity entity) -> Render::GpuSceneResourceKey {
        return entity == entt::null ?
                   Render::k_invalid_gpu_scene_resource_key :
                   static_cast<Render::GpuSceneResourceKey>(entt::to_integral(entity));
    };

    if (full_rebuild) {
        update.textures.reserve(registry.view<const ecs::CTexture>().size());
        registry.view<const ecs::CTexture>().each(
            [&](entt::entity entity, const ecs::CTexture& texture) {
                const auto* resource_name = registry.try_get<ecs::CResourceName>(entity);
                Render::GpuSceneTextureData data{};
                data.key  = to_key(entity);
                data.name = resource_name == nullptr || ecs::IsBlankName(resource_name->name) ?
                                ecs::MakeDebugName("Texture", entity) :
                                resource_name->name;
                data.data              = texture.data;
                data.format            = texture.format;
                data.width             = texture.width;
                data.height            = texture.height;
                data.mip_level_count   = texture.mip_level_count;
                data.array_layer_count = texture.array_layer_count;
                update.textures.push_back(std::move(data));
            }
        );
    }

    if (update.update_lights) {
        update.lights = m_light_buf;
    }

    if (update.update_materials) {
        update.materials = m_material_buf;
        update.material_texture_refs.resize(update.materials.size());
        registry.view<const ecs::CMaterial>().each(
            [&](entt::entity entity, const ecs::CMaterial& material) {
                const auto material_it = m_map_material_entity_to_id.find(entity);
                if (material_it == m_map_material_entity_to_id.end()) {
                    return;
                }
                auto& refs = update.material_texture_refs[material_it->second];
                refs.normal             = to_key(material.normal_map_entt);
                refs.ao                 = to_key(material.ao_map_entt);
                refs.albedo             = to_key(material.albedo_map_entt);
                refs.emissive           = to_key(material.emissive_map_entt);
                refs.metallic_roughness = to_key(material.metallic_roughness_map_entt);
            }
        );
    }

    if (update.update_meshes) {
        update.draw_commands = m_draw_cmd_buf;
        update.primitives    = m_primitive_buf;
        update.instances     = m_instance_buf;
        update.cluster_groups = m_cluster_group_buf;

        const auto& mega      = mega_buf();
        update.positions      = mega.position;
        update.packed_normals = mega.packed_normal;
        update.packed_tangents = mega.packed_tangent;
        update.texcoords0     = mega.texcoord0;
        update.indices        = mega.index;
    } else if (update_rt_instances) {
        update.instances = m_instance_buf;
    }

    if (update.raytracing_update == Render::EGpuSceneRaytracingUpdate::RebuildBlas) {
        update.rt_meshes.reserve(registry.view<const ecs::CMesh>().size());
        registry.view<const ecs::CMesh>().each(
            [&](entt::entity mesh_entity, const ecs::CMesh& mesh) {
                Render::GpuSceneRtMeshData mesh_data{};
                mesh_data.key = to_key(mesh_entity);
                const uint leaf_count = mesh.num_leaf_clusters > 0 ?
                                            Min(
                                                mesh.num_leaf_clusters,
                                                static_cast<uint>(mesh.primitive_entts.size())
                                            ) :
                                            static_cast<uint>(mesh.primitive_entts.size());
                mesh_data.geometries.reserve(leaf_count);
                for (uint index = 0; index < leaf_count; ++index) {
                    const entt::entity primitive_entity = mesh.primitive_entts[index];
                    const auto* primitive = registry.try_get<const ecs::CPrimitive>(primitive_entity);
                    if (primitive == nullptr || !primitive->position.is_valid ||
                        !primitive->index.is_valid) {
                        continue;
                    }

                    mesh_data.geometries.push_back(
                        {
                            primitive->position.start_idx,
                            primitive->vertex_count,
                            primitive->index.start_idx,
                            primitive->index_count,
                            GetPrimitiveId(primitive_entity),
                        }
                    );
                }
                update.rt_meshes.push_back(std::move(mesh_data));
            }
        );
    }

    if (update.raytracing_update != Render::EGpuSceneRaytracingUpdate::None) {
        update.rt_instances.reserve(
            registry.view<const ecs::CRenderable, const ecs::CNode>().size_hint()
        );
        registry.view<const ecs::CRenderable, const ecs::CNode>().each(
            [&](entt::entity entity, const ecs::CRenderable& renderable, const ecs::CNode& node) {
                if (renderable.mesh_entt == entt::null ||
                    !IsNodeEffectivelyVisibleInGame(registry, entity) ||
                    !registry.valid(renderable.mesh_entt) ||
                    !registry.all_of<ecs::CMesh>(renderable.mesh_entt)) {
                    return;
                }

                const auto& mesh = registry.get<const ecs::CMesh>(renderable.mesh_entt);
                const uint leaf_count = mesh.num_leaf_clusters > 0 ?
                                            Min(
                                                mesh.num_leaf_clusters,
                                                static_cast<uint>(mesh.primitive_entts.size())
                                            ) :
                                            static_cast<uint>(mesh.primitive_entts.size());
                uint valid_primitive_count = 0;
                uint first_primitive_id     = UINT_MAX;
                for (uint index = 0; index < leaf_count; ++index) {
                    const entt::entity primitive_entity = mesh.primitive_entts[index];
                    const auto* primitive = registry.try_get<const ecs::CPrimitive>(primitive_entity);
                    if (primitive == nullptr || !primitive->position.is_valid ||
                        !primitive->index.is_valid) {
                        continue;
                    }
                    const uint primitive_id = GetPrimitiveId(primitive_entity);
                    if (first_primitive_id == UINT_MAX) {
                        first_primitive_id = primitive_id;
                    }
                    ++valid_primitive_count;
                }

                update.rt_instances.push_back(
                    {
                        to_key(renderable.mesh_entt),
                        node.d_world_transform,
                        valid_primitive_count,
                        first_primitive_id,
                    }
                );
            }
        );
    }

    return update;
}

} // namespace Moer
