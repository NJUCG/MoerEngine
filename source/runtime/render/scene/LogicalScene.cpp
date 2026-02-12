#include "LogicalScene.h"

#include "LogicalComponents.h"

#include "misc/Hash.h"

#include <entt/entt.hpp>

namespace Moer::ecs {

static entt::registry registry;

entt::registry& LogicalScene::r() {
    return registry;
}

const entt::registry& LogicalScene::r() const {
    return registry;
}

LogicalScene::LogicalScene() {
    registry = entt::registry{};

    // Registry Settings

    registry.group<ecs::CNode, ecs::CTransform>();
}

LogicalScene::~LogicalScene() {
    registry.clear();
}

void LogicalScene::SBuildPrimitiveHash() {

    r().view<ecs::CPrimitive>().each([](auto& c_primitive) {
        uint64 hash = 14695981039346656037ULL;

        // 将 BufferView 视为一个整体进行处理
        auto hash_bv = [&](const CPrimitive::BufferView& bv) {
            HashCombine(hash, bv.start_idx);
            HashCombine(hash, bv.stride);
            HashCombine(hash, (uint32)bv.is_valid);
        };

        hash_bv(c_primitive.position);
        hash_bv(c_primitive.packed_normal);
        hash_bv(c_primitive.packed_tangent);
        hash_bv(c_primitive.texcoord0);
        hash_bv(c_primitive.index);

        HashCombine(hash, static_cast<uint64>(c_primitive.vertex_count));
        HashCombine(hash, static_cast<uint64>(c_primitive.index_count));
        HashCombine(hash, static_cast<uint64>(c_primitive.material_entt));

        c_primitive.d_primitive_hash = hash;
    });
}

void LogicalScene::SBuildMeshHash() {
    // 1. 获取所有 CMesh 的 View
    auto view = r().view<ecs::CMesh>();

    // 2. 遍历每个 Mesh
    view.each([&](auto& c_mesh) {
        // 使用初始偏移值
        uint64 hash = 14695981039346656037ULL;

        // 3. 遍历该 Mesh 引用的所有 Primitive
        for (entt::entity p_entity : c_mesh.primitive_entts) {
            // 安全检查：确保实体有效且拥有 CPrimitive 组件
            // 在实时渲染器中，如果确定逻辑层数据完整，可以去掉 if 提高性能
            if (r().valid(p_entity) && r().all_of<ecs::CPrimitive>(p_entity)) {
                const auto& c_primitive = r().get<ecs::CPrimitive>(p_entity);

                // 混合该 Primitive 的哈希值
                // 注意：这里依赖 SBuildPrimitiveHash 必须先执行完毕
                HashCombine(hash, c_primitive.d_primitive_hash);
            }
        }

        // 4. 存储最终生成的 Mesh 哈希
        c_mesh.d_mesh_hash = hash;
    });
}

void LogicalScene::SUpdateAllNodeTransforms() {

    // 按深度排序节点，确保父节点在前，子节点在后
    // TODO: cache is sorted
    registry.group<ecs::CNode, ecs::CTransform>().sort<ecs::CNode>([](const auto& lhs, const auto& rhs) {
        return lhs.depth < rhs.depth;
    });

    // 更新变换矩阵
    registry.group<ecs::CNode, ecs::CTransform>().each([](auto& c_node, auto& c_transform) {
        if (c_transform.is_dirty == false)
            return;

        bool     is_parent_dirty  = false;
        float4x4 parent_transform = float4x4::Identity();
        if (c_node.parent_entt != entt::null) {
            const auto& parent_c_transform = registry.get<ecs::CTransform>(c_node.parent_entt);
            is_parent_dirty                = parent_c_transform.is_dirty;
            parent_transform               = parent_c_transform.d_world_transform;
        }

        // is_dirty会向下传递
        c_transform.is_dirty = c_transform.is_dirty || is_parent_dirty;
        if (c_transform.is_dirty) {
            c_transform.d_world_transform = float4x4::Identity();

            float4x4 local_transform =
                Transform(c_transform.translation, c_transform.scale, c_transform.rotation).GetMatrix4x4();

            c_transform.d_world_transform = parent_transform * local_transform;
        }
    });

    // 清空所有节点的is_dirty标记
    registry.group<ecs::CNode, ecs::CTransform>().each([](auto& c_node, auto& c_transform) {
        c_transform.is_dirty = false;
    });
}

void LogicalScene::UEmplaceNodeToParent(
    const entt::entity parent_entt,
    CNode&             parent_node,
    const entt::entity child_id,
    CNode&             child_node
) {

    parent_node.child_count += 1;

    child_node.parent_entt = parent_entt;

    child_node.depth = parent_node.depth + 1;

    // 没有子节点
    if (parent_node.last_child_entt == entt::null) {

        parent_node.first_child_entt = child_id;
        parent_node.last_child_entt  = child_id;

    } else {

        const auto prev_sibling_entt = parent_node.last_child_entt;

        parent_node.last_child_entt = child_id;

        // 更新前一个最后子节点的 next_sibling_entt
        auto& prev_sibling_node             = r().get<ecs::CNode>(prev_sibling_entt);
        prev_sibling_node.next_sibling_entt = child_id;

        // 更新当前y节点的 prev_sibling_entt
        child_node.prev_sibling_entt = prev_sibling_entt;
    }
}

void LogicalScene::UCreateDefaultCamera(entt::entity parent_node_id, bool shuold_create_main_camera) {
    // 创建默认摄像机实体
    entt::entity camera_entity = r().create();

    auto& c_node      = r().emplace<ecs::CNode>(camera_entity);
    auto& c_transform = r().emplace<ecs::CTransform>(camera_entity);
    auto& c_camera    = r().emplace<ecs::CCamera>(camera_entity);
    if (shuold_create_main_camera) {
        r().emplace<ecs::CTagMainCamera>(camera_entity);
    }

    // CNode
    if (parent_node_id == entt::null) {
        // 挂在根节点下
        parent_node_id = r().view<ecs::CTagRootNode>().front();
    }

    auto& parent_node = r().get<ecs::CNode>(parent_node_id);

    UEmplaceNodeToParent(parent_node_id, parent_node, camera_entity, c_node);

    // CCamera
    c_camera.camera = Camera::CreateDefaultCamera();

    // CTransform
    // TODO: Camera目前没有和CTransform接入
}

void LogicalScene::UCreateDefaultLights(entt::entity parent_node_id, bool should_create_main_light) {

    if (parent_node_id == entt::null) {
        // 挂在根节点下
        parent_node_id = r().view<ecs::CTagRootNode>().front();
    }
    auto& parent_node = r().get<ecs::CNode>(parent_node_id);

    auto create_light_entity = [&](ELightType light_type) -> entt::entity {
        entt::entity light_entity = r().create();

        auto& c_node      = r().emplace<ecs::CNode>(light_entity);
        auto& c_transform = r().emplace<ecs::CTransform>(light_entity);
        auto& c_light     = r().emplace<ecs::CLight>(light_entity);
        c_light.type      = light_type;

        UEmplaceNodeToParent(parent_node_id, parent_node, light_entity, c_node);

        return light_entity;
    };

    // first directional main light
    {
        entt::entity main_light_entity = create_light_entity(ELightType::Directional);
        if (should_create_main_light) {
            r().emplace<ecs::CTagMainLight>(main_light_entity);
        }

        auto& c_directional_light     = r().emplace<ecs::CLightDirectional>(main_light_entity);
        c_directional_light.color     = float3(0.9f, 0.65f, 0.4f);
        c_directional_light.intensity = 100.0f;

        auto& c_transform    = r().get<ecs::CTransform>(main_light_entity);
        c_transform.rotation = Quaternion(
            float3(0.f, 0.f, -1.f),   // from
            float3(-1.f, -2.5f, -1.f) // to
        );
        c_transform.is_dirty = true;
    }

    // TODO..
}

} // namespace Moer::ecs