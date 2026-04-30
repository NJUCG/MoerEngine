#include "LogicalScene.h"

#include "LogicalComponents.h"
#include "log/LogSystem.h"
#include "math/Function.h"
#include "misc/Hash.h"
#include "shaderheaders/shared/utils/Packing.h"

#include <entt/entt.hpp>
#include <cmath>

namespace Moer::ecs {

static void LogSceneApiError(std::string_view message) {
    LOG_ERROR("[Scene API Error] {}", message);
}

static void SetEntityName(entt::registry& registry, entt::entity entity, std::string_view name) {
    if (name.empty()) {
        return;
    }

    registry.emplace_or_replace<ecs::CName>(entity).name = name;
}

static float3 SafeNormalizePrimitiveVector(const float3& value, const float3& fallback) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 1e-5f) {
        return fallback;
    }
    return value * (1.f / length);
}

static ecs::CPrimitive::BufferView MakePrimitiveBufferView(uint32 start_idx, uint32 stride) {
    return ecs::CPrimitive::BufferView{
        .start_idx = start_idx,
        .stride    = stride,
        .is_valid  = true,
    };
}

static bool IsMatchingAttributeCount(size_t attribute_count, size_t position_count) {
    return attribute_count == 0 || attribute_count == position_count;
}

static Box3D BuildPrimitiveDataAABB(const Array<float3>& positions) {
    Box3D aabb;
    for (const float3& position : positions) {
        aabb.Expand(position);
    }
    return aabb;
}

static void RefreshSubtreeDepth(entt::registry& registry, entt::entity entity) {
    auto&        node       = registry.get<ecs::CNode>(entity);
    entt::entity child_entt = node.first_child_entt;
    while (child_entt != entt::null) {
        auto& child_node = registry.get<ecs::CNode>(child_entt);
        child_node.depth = node.depth + 1;
        RefreshSubtreeDepth(registry, child_entt);
        child_entt = child_node.next_sibling_entt;
    }
}

static entt::registry registry;

entt::registry& LogicalScene::r() {
    return registry;
}

const entt::registry& LogicalScene::r() const {
    return registry;
}

void LogicalScene::Update() {
    SUpdateAllNodeTransformAndAABB();
    SUpdateAllLightData();
}

LogicalScene::LogicalScene() {
    registry = entt::registry{};
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

void LogicalScene::SBuildMeshAABB() {
    // mesh
    r().view<ecs::CMesh>().each([&](auto& c_mesh) {
        c_mesh.d_aabb = Box3D();
        for (entt::entity p_entity : c_mesh.primitive_entts) {
            if (r().valid(p_entity) && r().all_of<ecs::CPrimitive>(p_entity)) {
                const auto& c_primitive = r().get<ecs::CPrimitive>(p_entity);
                c_mesh.d_aabb.Expand(c_primitive.aabb);
            }
        }
    });
}

void LogicalScene::SUpdateAllNodeTransformAndAABB() {

    // 按深度排序节点，确保父节点在前，子节点在后
    // TODO: cache is sorted
    registry.sort<ecs::CNode>([](const auto& lhs, const auto& rhs) {
        return lhs.depth < rhs.depth;
    });

    auto build_mesh_aabb = [&](const entt::entity entity_id, const ecs::CNode& c_node) {
        Box3D mesh_aabb = Box3D();
        if (!registry.all_of<ecs::CRenderable>(entity_id)) {
            return mesh_aabb;
        }

        const auto& c_renderable = registry.get<ecs::CRenderable>(entity_id);
        if (c_renderable.mesh_entt == entt::null || !registry.valid(c_renderable.mesh_entt) ||
            !registry.all_of<ecs::CMesh>(c_renderable.mesh_entt)) {
            return mesh_aabb;
        }

        const auto& c_mesh = registry.get<ecs::CMesh>(c_renderable.mesh_entt);
        if (!c_mesh.d_aabb.IsValid()) {
            return mesh_aabb;
        }

        const float3& min = c_mesh.d_aabb.min;
        const float3& max = c_mesh.d_aabb.max;
        Transform     transform_mat(c_node.d_world_transform);

        mesh_aabb.Expand(transform_mat * float3(min.x, min.y, min.z));
        mesh_aabb.Expand(transform_mat * float3(max.x, min.y, min.z));
        mesh_aabb.Expand(transform_mat * float3(min.x, max.y, min.z));
        mesh_aabb.Expand(transform_mat * float3(max.x, max.y, min.z));
        mesh_aabb.Expand(transform_mat * float3(min.x, min.y, max.z));
        mesh_aabb.Expand(transform_mat * float3(max.x, min.y, max.z));
        mesh_aabb.Expand(transform_mat * float3(min.x, max.y, max.z));
        mesh_aabb.Expand(transform_mat * float3(max.x, max.y, max.z));

        return mesh_aabb;
    };

    auto mark_aabb_update_chain = [&](UnorderedSet<entt::entity>& aabb_update_set, entt::entity entity_id) {
        while (entity_id != entt::null && registry.valid(entity_id) &&
               registry.all_of<ecs::CNode>(entity_id)) {
            aabb_update_set.emplace(entity_id);
            entity_id = registry.get<ecs::CNode>(entity_id).parent_entt;
        }
    };

    auto mark_light_update_if_needed = [&](const entt::entity entity_id) {
        if (registry.all_of<ecs::CLightDirectional>(entity_id)) {
            registry.get<ecs::CLightDirectional>(entity_id).is_dirty = true;
            if (!registry.all_of<ecs::CTagNeedUpdateLight>(entity_id)) {
                registry.emplace<ecs::CTagNeedUpdateLight>(entity_id);
            }
        }
        if (registry.all_of<ecs::CLightPoint>(entity_id)) {
            registry.get<ecs::CLightPoint>(entity_id).is_dirty = true;
            if (!registry.all_of<ecs::CTagNeedUpdateLight>(entity_id)) {
                registry.emplace<ecs::CTagNeedUpdateLight>(entity_id);
            }
        }
    };

    // 第一步：收集所有需要更新的 entity（同时处理 is_dirty 向下传递）
    Array<entt::entity>        dirty_entities;
    UnorderedSet<entt::entity> aabb_update_set;
    dirty_entities.reserve(registry.view<ecs::CNode>().size());
    registry.view<ecs::CNode>().each([&](auto entity_id, auto& c_node) {
        // 检查父节点是否 dirty，向下传递
        bool is_parent_dirty = false;
        if (c_node.parent_entt != entt::null) {
            const auto& parent_node = registry.get<ecs::CNode>(c_node.parent_entt);
            is_parent_dirty         = parent_node.is_dirty;
        }

        // is_dirty会向下传递
        c_node.is_dirty |= is_parent_dirty;
        if (c_node.is_dirty) {
            dirty_entities.push_back(entity_id);
            registry.emplace_or_replace<ecs::CTagNeedUpdateTransform>(entity_id);
            mark_aabb_update_chain(aabb_update_set, entity_id);
        }
    });

    Array<entt::entity> aabb_update_entities;
    aabb_update_entities.reserve(aabb_update_set.size());
    registry.view<ecs::CNode>().each([&](auto entity_id, auto&) {
        if (aabb_update_set.contains(entity_id)) {
            aabb_update_entities.push_back(entity_id);
        }
    });

    // 第二步：正向遍历更新变换矩阵（从浅到深）
    for (entt::entity entity_id : dirty_entities) {
        auto& c_node = registry.get<ecs::CNode>(entity_id);

        // 计算父节点的世界变换矩阵
        float4x4 parent_transform = float4x4::Identity();
        if (c_node.parent_entt != entt::null) {
            const auto& parent_node = registry.get<ecs::CNode>(c_node.parent_entt);
            parent_transform        = parent_node.d_world_transform;
        }

        // 更新变换矩阵
        float4x4 local_transform =
            Transform(c_node.translation, c_node.scale, c_node.rotation).GetMatrix4x4();
        c_node.d_world_transform = parent_transform * local_transform;

        // 如果有 Light 组件，则标记该 Light 需要更新派生数据
        mark_light_update_if_needed(entity_id);
    }

    for (entt::entity entity_id : aabb_update_entities) {
        auto& c_node  = registry.get<ecs::CNode>(entity_id);
        c_node.d_aabb = build_mesh_aabb(entity_id, c_node);
    }

    // 第三步：反向遍历合并子节点的 AABB（从深到浅，确保子节点先处理）
    for (auto it = aabb_update_entities.rbegin(); it != aabb_update_entities.rend(); ++it) {
        auto  entity_id = *it;
        auto& c_node    = registry.get<ecs::CNode>(entity_id);

        // 合并所有子节点的 AABB
        if (c_node.first_child_entt != entt::null) {
            entt::entity child_entt = c_node.first_child_entt;
            while (child_entt != entt::null) {
                const auto& child_node = registry.get<ecs::CNode>(child_entt);
                if (child_node.d_aabb.IsValid()) {
                    c_node.d_aabb.Expand(child_node.d_aabb);
                }
                child_entt = child_node.next_sibling_entt;
            }
        }
    }

    // 第四步：清空所有节点的is_dirty标记
    for (entt::entity entity_id : dirty_entities) {
        auto& c_node    = registry.get<ecs::CNode>(entity_id);
        c_node.is_dirty = false;
    }
}

void LogicalScene::SUpdateAllLightData() {
    r().view<ecs::CLightDirectional, ecs::CNode>().each(
        [&](auto, ecs::CLightDirectional& c_light, const ecs::CNode& c_node) {
            if (!c_light.is_dirty) {
                return;
            }

            const float4 world_direction = c_node.d_world_transform * float4(0.f, 0.f, -1.f, 0.f);
            c_light.d_direction          = Normalizef(float3(world_direction));
            c_light.is_dirty             = false;
        }
    );

    r().view<ecs::CLightPoint, ecs::CNode>().each(
        [&](auto, ecs::CLightPoint& c_light, const ecs::CNode& c_node) {
            if (!c_light.is_dirty) {
                return;
            }

            c_light.d_position = float3(c_node.d_world_transform.GetColumn(3));
            c_light.is_dirty   = false;
        }
    );
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

// 从父节点的 child 链表中摘除指定节点
void LogicalScene::UDetachNodeFromParent(entt::entity child_entt, CNode& child_node) {
    if (child_node.parent_entt == entt::null) {
        child_node.prev_sibling_entt = entt::null;
        child_node.next_sibling_entt = entt::null;
        return;
    }

    if (!r().valid(child_node.parent_entt) || !r().all_of<CNode>(child_node.parent_entt)) {
        LogSceneApiError("Cannot detach node because parent node is invalid.");
        child_node.parent_entt       = entt::null;
        child_node.prev_sibling_entt = entt::null;
        child_node.next_sibling_entt = entt::null;
        return;
    }

    auto& parent_node = r().get<CNode>(child_node.parent_entt);

    if (parent_node.first_child_entt == child_entt) {
        parent_node.first_child_entt = child_node.next_sibling_entt;
    }
    if (parent_node.last_child_entt == child_entt) {
        parent_node.last_child_entt = child_node.prev_sibling_entt;
    }
    if (parent_node.child_count > 0) {
        parent_node.child_count -= 1;
    }

    if (child_node.prev_sibling_entt != entt::null && r().valid(child_node.prev_sibling_entt) &&
        r().all_of<CNode>(child_node.prev_sibling_entt)) {
        auto& prev_sibling_node             = r().get<CNode>(child_node.prev_sibling_entt);
        prev_sibling_node.next_sibling_entt = child_node.next_sibling_entt;
    }

    if (child_node.next_sibling_entt != entt::null && r().valid(child_node.next_sibling_entt) &&
        r().all_of<CNode>(child_node.next_sibling_entt)) {
        auto& next_sibling_node             = r().get<CNode>(child_node.next_sibling_entt);
        next_sibling_node.prev_sibling_entt = child_node.prev_sibling_entt;
    }

    child_node.parent_entt       = entt::null;
    child_node.prev_sibling_entt = entt::null;
    child_node.next_sibling_entt = entt::null;
    child_node.depth             = 0;
}

entt::entity LogicalScene::UGetRootNodeEntity() {
    auto view = r().view<ecs::CTagRootNode>();
    auto it   = view.begin();
    if (it == view.end()) {
        LogSceneApiError("Cannot find root node because no CTagRootNode exists in the scene.");
        return entt::null;
    }
    return *it;
}

bool LogicalScene::UIsEntityWithNode(entt::entity entity) const {
    return entity != entt::null && r().valid(entity) && r().all_of<ecs::CNode>(entity);
}

entt::entity LogicalScene::UCreateEntity(std::string_view name) {
    entt::entity entity = r().create();
    SetEntityName(r(), entity, name);
    return entity;
}

entt::entity LogicalScene::UCreateEntityWithNode(const EntityWithNodeCreateInfo& create_info) {
    entt::entity parent_node_entt = create_info.parent_node_entt;
    if (parent_node_entt == entt::null) {
        parent_node_entt = UGetRootNodeEntity();
    }

    if (!UIsEntityWithNode(parent_node_entt)) {
        LogSceneApiError("Cannot create EntityWithNode because parent node is invalid or missing CNode.");
        return entt::null;
    }

    entt::entity entity = r().create();
    auto&        node   = r().emplace<ecs::CNode>(entity);
    SetEntityName(r(), entity, create_info.name);

    node.translation = create_info.translation;
    node.rotation    = create_info.rotation;
    node.scale       = create_info.scale;
    node.is_dirty    = true;

    auto& parent_node = r().get<ecs::CNode>(parent_node_entt);
    UEmplaceNodeToParent(parent_node_entt, parent_node, entity, node);
    return entity;
}

entt::entity LogicalScene::UCreateRenderableWithNode(const RenderableCreateInfo& create_info) {
    entt::entity parent_node_entt = create_info.parent_node_entt;
    if (parent_node_entt == entt::null) {
        parent_node_entt = UGetRootNodeEntity();
    }

    if (!UIsEntityWithNode(parent_node_entt)) {
        LogSceneApiError("Cannot create renderable because parent node is invalid or missing CNode.");
        return entt::null;
    }
    if (create_info.mesh_entt == entt::null || !r().valid(create_info.mesh_entt) ||
        !r().all_of<ecs::CMesh>(create_info.mesh_entt)) {
        LogSceneApiError("Cannot create renderable because mesh entity is invalid or missing CMesh.");
        return entt::null;
    }

    entt::entity entity      = r().create();
    auto&        node        = r().emplace<ecs::CNode>(entity);
    auto&        renderable  = r().emplace<ecs::CRenderable>(entity);
    renderable.mesh_entt     = create_info.mesh_entt;

    SetEntityName(r(), entity, create_info.name);

    node.translation = create_info.translation;
    node.rotation    = create_info.rotation;
    node.scale       = create_info.scale;
    node.is_dirty    = true;

    auto& parent_node = r().get<ecs::CNode>(parent_node_entt);
    UEmplaceNodeToParent(parent_node_entt, parent_node, entity, node);
    return entity;
}

entt::entity LogicalScene::UCreateMaterial(const MaterialCreateInfo& create_info) {
    entt::entity entity = r().create();
    auto&        material = r().emplace<ecs::CMaterial>(entity);
    SetEntityName(r(), entity, create_info.name);

    material.albedo_factor    = create_info.albedo_factor;
    material.emissive_factor  = create_info.emissive_factor;
    material.metallic_factor  = create_info.metallic_factor;
    material.roughness_factor = create_info.roughness_factor;
    material.alpha_mode       = create_info.alpha_mode;
    material.alpha_cutoff     = create_info.alpha_cutoff;

    return entity;
}

entt::entity LogicalScene::UCreatePrimitive(const PrimitiveCreateInfo& create_info) {
    if (create_info.positions.empty()) {
        LogSceneApiError("Cannot create primitive because positions are empty.");
        return entt::null;
    }
    if (create_info.indices.empty()) {
        LogSceneApiError("Cannot create primitive because indices are empty.");
        return entt::null;
    }
    if ((create_info.indices.size() % 3) != 0) {
        LogSceneApiError("Cannot create primitive because index count is not a multiple of 3.");
        return entt::null;
    }
    if (!IsMatchingAttributeCount(create_info.normals.size(), create_info.positions.size())) {
        LogSceneApiError("Cannot create primitive because normal count does not match position count.");
        return entt::null;
    }
    if (!IsMatchingAttributeCount(create_info.tangents.size(), create_info.positions.size())) {
        LogSceneApiError("Cannot create primitive because tangent count does not match position count.");
        return entt::null;
    }
    if (!IsMatchingAttributeCount(create_info.texcoord0.size(), create_info.positions.size())) {
        LogSceneApiError("Cannot create primitive because texcoord0 count does not match position count.");
        return entt::null;
    }
    if (create_info.material_entt == entt::null || !r().valid(create_info.material_entt) ||
        !r().all_of<ecs::CMaterial>(create_info.material_entt)) {
        LogSceneApiError("Cannot create primitive because material entity is invalid or missing CMaterial.");
        return entt::null;
    }

    for (uint32 index : create_info.indices) {
        if (index >= create_info.positions.size()) {
            LogSceneApiError("Cannot create primitive because an index is out of range.");
            return entt::null;
        }
    }

    if (!r().ctx().contains<ecs::CtxMegaBuffers>()) {
        r().ctx().emplace<ecs::CtxMegaBuffers>();
    }

    auto& mega_buffers = r().ctx().get<ecs::CtxMegaBuffers>();

    entt::entity entity = r().create();
    auto&        primitive = r().emplace<ecs::CPrimitive>(entity);
    SetEntityName(r(), entity, create_info.name);

    primitive.vertex_count  = static_cast<uint32>(create_info.positions.size());
    primitive.index_count   = static_cast<uint32>(create_info.indices.size());
    primitive.material_entt = create_info.material_entt;
    primitive.aabb          = create_info.has_aabb ? create_info.aabb : BuildPrimitiveDataAABB(create_info.positions);

    primitive.position = MakePrimitiveBufferView(
        static_cast<uint32>(mega_buffers.position.size()), sizeof(float3)
    );
    mega_buffers.position.insert(
        mega_buffers.position.end(), create_info.positions.begin(), create_info.positions.end()
    );

    if (!create_info.normals.empty()) {
        primitive.packed_normal = MakePrimitiveBufferView(
            static_cast<uint32>(mega_buffers.packed_normal.size()), sizeof(uint32)
        );
        for (const float3& normal : create_info.normals) {
            mega_buffers.packed_normal.emplace_back(
                Pack_Normal(SafeNormalizePrimitiveVector(normal, float3(0.f, 0.f, 1.f)))
            );
        }
    } else {
        LOG_WARNING("Runtime primitive '{}' was created without normal data.", create_info.name);
    }

    if (!create_info.tangents.empty()) {
        primitive.packed_tangent = MakePrimitiveBufferView(
            static_cast<uint32>(mega_buffers.packed_tangent.size()), sizeof(uint32)
        );
        for (const float3& tangent : create_info.tangents) {
            mega_buffers.packed_tangent.emplace_back(
                Pack_Normal(SafeNormalizePrimitiveVector(tangent, float3(1.f, 0.f, 0.f)))
            );
        }
    }

    if (!create_info.texcoord0.empty()) {
        primitive.texcoord0 = MakePrimitiveBufferView(
            static_cast<uint32>(mega_buffers.texcoord0.size()), sizeof(float2)
        );
        mega_buffers.texcoord0.insert(
            mega_buffers.texcoord0.end(), create_info.texcoord0.begin(), create_info.texcoord0.end()
        );
    }

    primitive.index = MakePrimitiveBufferView(static_cast<uint32>(mega_buffers.index.size()), sizeof(uint32));
    mega_buffers.index.insert(mega_buffers.index.end(), create_info.indices.begin(), create_info.indices.end());

    SBuildPrimitiveHash();

    LOG_INFO(
        "Runtime Primitive Data appended: vertices={}, indices={}, mega(position={}, normal={}, tangent={}, uv0={}, index={})",
        create_info.positions.size(),
        create_info.indices.size(),
        mega_buffers.position.size(),
        mega_buffers.packed_normal.size(),
        mega_buffers.packed_tangent.size(),
        mega_buffers.texcoord0.size(),
        mega_buffers.index.size()
    );

    return entity;
}

entt::entity LogicalScene::UCreateMesh(const MeshCreateInfo& create_info) {
    if (create_info.primitive_entts.empty()) {
        LogSceneApiError("Cannot create mesh because primitive list is empty.");
        return entt::null;
    }

    for (entt::entity primitive_entt : create_info.primitive_entts) {
        if (primitive_entt == entt::null || !r().valid(primitive_entt) ||
            !r().all_of<ecs::CPrimitive>(primitive_entt)) {
            LogSceneApiError("Cannot create mesh because a primitive entity is invalid or missing CPrimitive.");
            return entt::null;
        }
    }

    entt::entity entity = r().create();
    auto&        mesh   = r().emplace<ecs::CMesh>(entity);
    SetEntityName(r(), entity, create_info.name);

    mesh.primitive_entts = create_info.primitive_entts;

    SBuildPrimitiveHash();
    SBuildMeshHash();
    SBuildMeshAABB();

    return entity;
}

bool LogicalScene::USetLocalTransform(entt::entity entity, const Transform& local_transform) {
    if (!UIsEntityWithNode(entity)) {
        LogSceneApiError("Cannot set local transform because entity is invalid or missing CNode.");
        return false;
    }

    if (!local_transform.IsAffine()) {
        LogSceneApiError("Cannot set local transform because only affine Transform is supported.");
        return false;
    }

    const auto affine = local_transform.AffineDecomposition();

    auto& node       = r().get<ecs::CNode>(entity);
    node.translation = affine.translation;
    node.rotation    = affine.quaternion;
    node.scale       = affine.scaling;
    return true;
}

bool LogicalScene::UAttachToParent(
    entt::entity child_entt,
    entt::entity parent_entt,
    entt::entity* old_parent_entt,
    bool*         did_change
) {
    if (old_parent_entt) {
        *old_parent_entt = entt::null;
    }
    if (did_change) {
        *did_change = false;
    }

    if (!UIsEntityWithNode(child_entt) || !UIsEntityWithNode(parent_entt)) {
        LogSceneApiError("Cannot attach node because child or parent is invalid or missing CNode.");
        return false;
    }
    if (child_entt == parent_entt) {
        LogSceneApiError("Cannot attach node to itself.");
        return false;
    }
    if (r().all_of<ecs::CTagRootNode>(child_entt)) {
        LogSceneApiError("Cannot attach root node to another parent.");
        return false;
    }

    entt::entity current = parent_entt;
    while (current != entt::null && r().valid(current) && r().all_of<ecs::CNode>(current)) {
        if (current == child_entt) {
            LogSceneApiError("Cannot attach node to its descendant.");
            return false;
        }
        current = r().get<ecs::CNode>(current).parent_entt;
    }

    auto& child_node = r().get<ecs::CNode>(child_entt);
    if (old_parent_entt) {
        *old_parent_entt = child_node.parent_entt;
    }
    if (child_node.parent_entt == parent_entt) {
        return true;
    }

    UDetachNodeFromParent(child_entt, child_node);

    auto& parent_node = r().get<ecs::CNode>(parent_entt);
    UEmplaceNodeToParent(parent_entt, parent_node, child_entt, child_node);
    RefreshSubtreeDepth(r(), child_entt);

    if (did_change) {
        *did_change = true;
    }
    return true;
}

bool LogicalScene::UDetachFromParent(
    entt::entity child_entt,
    entt::entity* old_parent_entt,
    bool*         did_change
) {
    if (old_parent_entt) {
        *old_parent_entt = entt::null;
    }
    if (did_change) {
        *did_change = false;
    }

    if (!UIsEntityWithNode(child_entt)) {
        LogSceneApiError("Cannot detach node because entity is invalid or missing CNode.");
        return false;
    }
    if (r().all_of<ecs::CTagRootNode>(child_entt)) {
        LogSceneApiError("Cannot detach root node.");
        return false;
    }

    const entt::entity root_entt = UGetRootNodeEntity();
    if (!UIsEntityWithNode(root_entt)) {
        return false;
    }

    auto& child_node = r().get<ecs::CNode>(child_entt);
    if (old_parent_entt) {
        *old_parent_entt = child_node.parent_entt;
    }
    if (child_node.parent_entt == root_entt) {
        return true;
    }

    UDetachNodeFromParent(child_entt, child_node);

    auto& root_node = r().get<ecs::CNode>(root_entt);
    UEmplaceNodeToParent(root_entt, root_node, child_entt, child_node);
    RefreshSubtreeDepth(r(), child_entt);

    if (did_change) {
        *did_change = true;
    }
    return true;
}

bool LogicalScene::UDestroyEntity(entt::entity entity, entt::entity* old_parent_entt) {
    if (old_parent_entt) {
        *old_parent_entt = entt::null;
    }

    if (entity == entt::null || !r().valid(entity)) {
        LogSceneApiError("Cannot destroy entity because entity is invalid.");
        return false;
    }
    if (r().all_of<ecs::CLight>(entity)) {
        LogSceneApiError("Cannot destroy light entity through DestroyEntity. Use light-specific destroy API.");
        return false;
    }
    if (r().all_of<ecs::CRenderable>(entity)) {
        LogSceneApiError("Cannot destroy renderable entity through DestroyEntity yet.");
        return false;
    }
    if (r().all_of<ecs::CTagRootNode>(entity)) {
        LogSceneApiError("Cannot destroy root node.");
        return false;
    }

    if (r().all_of<ecs::CNode>(entity)) {
        auto& node = r().get<ecs::CNode>(entity);
        if (node.first_child_entt != entt::null || node.child_count != 0) {
            LogSceneApiError("Cannot destroy EntityWithNode because only leaf nodes are supported now.");
            return false;
        }

        if (old_parent_entt) {
            *old_parent_entt = node.parent_entt;
        }
        UDetachNodeFromParent(entity, node);
    }

    r().destroy(entity);
    return true;
}

bool LogicalScene::UDestroyRenderable(entt::entity entity, entt::entity* old_parent_entt) {
    if (old_parent_entt) {
        *old_parent_entt = entt::null;
    }

    if (entity == entt::null || !r().valid(entity)) {
        LogSceneApiError("Cannot destroy renderable because entity is invalid.");
        return false;
    }
    if (!r().all_of<ecs::CRenderable, ecs::CNode>(entity)) {
        LogSceneApiError("Cannot destroy renderable because entity is missing CRenderable or CNode.");
        return false;
    }
    if (r().all_of<ecs::CTagRootNode>(entity)) {
        LogSceneApiError("Cannot destroy root node through DestroyRenderable.");
        return false;
    }

    auto& node = r().get<ecs::CNode>(entity);
    if (node.first_child_entt != entt::null || node.child_count != 0) {
        LogSceneApiError("Cannot destroy renderable because only leaf renderable nodes are supported now.");
        return false;
    }

    if (old_parent_entt) {
        *old_parent_entt = node.parent_entt;
    }
    UDetachNodeFromParent(entity, node);

    r().destroy(entity);
    return true;
}

entt::entity LogicalScene::UCreatePointLight(const PointLightCreateInfo& create_info) {
    entt::entity parent_node_entt = create_info.parent_node_entt;
    if (parent_node_entt == entt::null) {
        parent_node_entt = UGetRootNodeEntity();
    }
    if (parent_node_entt == entt::null || !r().valid(parent_node_entt) ||
        !r().all_of<ecs::CNode>(parent_node_entt)) {
        LogSceneApiError("Cannot create point light because parent node is invalid or missing CNode.");
        return entt::null;
    }

    entt::entity light_entity = r().create();

    auto& c_node  = r().emplace<ecs::CNode>(light_entity);
    auto& c_light = r().emplace<ecs::CLight>(light_entity);
    auto& c_point = r().emplace<ecs::CLightPoint>(light_entity);

    r().emplace<ecs::CName>(light_entity).name = create_info.name;

    c_node.translation = create_info.position;
    c_node.is_dirty    = true;

    c_light.type       = ELightType::Point;
    c_point.color      = create_info.color;
    c_point.intensity  = create_info.intensity;
    c_point.is_dirty   = true;
    c_point.d_position = create_info.position;

    if (create_info.should_set_main_light) {
        r().emplace_or_replace<ecs::CTagMainLight>(light_entity);
    }

    auto& parent_node = r().get<ecs::CNode>(parent_node_entt);
    UEmplaceNodeToParent(parent_node_entt, parent_node, light_entity, c_node);

    return light_entity;
}

bool LogicalScene::UCanDestroyPointLight(entt::entity light_entity) {
    if (light_entity == entt::null || !r().valid(light_entity)) {
        LogSceneApiError("Cannot destroy point light because entity is invalid.");
        return false;
    }

    if (!r().all_of<ecs::CLight, ecs::CLightPoint, ecs::CNode>(light_entity)) {
        LogSceneApiError("Cannot destroy point light because entity is missing point light components.");
        return false;
    }

    const auto& node = r().get<ecs::CNode>(light_entity);
    if (node.first_child_entt != entt::null || node.child_count != 0) {
        LogSceneApiError("Cannot destroy point light because only leaf light nodes are supported now.");
        return false;
    }

    return true;
}

void LogicalScene::UCreateDefaultCamera(entt::entity parent_node_id, bool shuold_create_main_camera) {
    // 创建默认摄像机实体
    entt::entity camera_entity = r().create();

    auto& c_node   = r().emplace<ecs::CNode>(camera_entity);
    auto& c_camera = r().emplace<ecs::CCamera>(camera_entity);
    if (shuold_create_main_camera) {
        r().emplace<ecs::CTagMainCamera>(camera_entity);
    }

    // CNode
    if (parent_node_id == entt::null) {
        // 挂在根节点下
        parent_node_id = UGetRootNodeEntity();
        if (parent_node_id == entt::null) {
            return;
        }
    }

    auto& parent_node = r().get<ecs::CNode>(parent_node_id);

    UEmplaceNodeToParent(parent_node_id, parent_node, camera_entity, c_node);

    // CCamera
    c_camera.camera = Camera::CreateDefaultCamera();

    // TODO: Camera目前没有和CNode接入
}

void LogicalScene::UCreateDefaultLights(entt::entity parent_node_id, bool should_create_main_light) {

    if (parent_node_id == entt::null) {
        // 挂在根节点下
        parent_node_id = UGetRootNodeEntity();
        if (parent_node_id == entt::null) {
            return;
        }
    }
    auto& parent_node = r().get<ecs::CNode>(parent_node_id);

    auto create_light_entity = [&](ELightType light_type) -> entt::entity {
        entt::entity light_entity = r().create();

        auto& c_node  = r().emplace<ecs::CNode>(light_entity);
        auto& c_light = r().emplace<ecs::CLight>(light_entity);
        c_light.type  = light_type;

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

        auto& c_node    = r().get<ecs::CNode>(main_light_entity);
        c_node.rotation = Quaternion(
            float3(0.f, 0.f, -1.f),   // from
            float3(-1.f, -2.5f, -1.f) // to
        );
        c_node.is_dirty = true;
    }

    // TODO..
}

} // namespace Moer::ecs