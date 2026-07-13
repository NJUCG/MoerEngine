#include "Scene.h"
#include "SceneInternal.h"

#include "log/LogSystem.h"
#include "math/Function.h"
#include "rhi/RHI.h"

#include <cmath>

namespace Moer {

// 如果你对SceneModify.cpp有疑问，请参考Scene的类注释

namespace {

static void MarkSceneNodeDirtyIfValid(Scene& scene, entt::entity entity) {
    auto& registry = scene.r();
    if (entity == entt::null || !registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
        return;
    }

    scene.MarkDirty<ecs::CNode>(entity);
}

static void MarkSceneMeshRebuild(Scene& scene) {
    auto& registry = scene.r();
    auto  view     = registry.view<ecs::CTagRootNode>();
    auto  it       = view.begin();
    if (it == view.end()) {
        return;
    }

    registry.emplace_or_replace<ecs::CTagNeedRebuildMesh>(*it);
}

static void MarkSceneRtBlasRebuild(Scene& scene) {
    auto& registry = scene.r();
    auto  view     = registry.view<ecs::CTagRootNode>();
    auto  it       = view.begin();
    if (it == view.end()) {
        return;
    }

    registry.emplace_or_replace<ecs::CTagNeedRebuildRtBlas>(*it);
}

static void CollectNodeSubtreePostOrder(
    const entt::registry& registry,
    entt::entity          entity,
    Array<entt::entity>&  out_entities
) {
    const auto& node = registry.get<ecs::CNode>(entity);

    entt::entity child_entt = node.first_child_entt;
    while (child_entt != entt::null) {
        const entt::entity next_sibling_entt = registry.get<ecs::CNode>(child_entt).next_sibling_entt;
        CollectNodeSubtreePostOrder(registry, child_entt, out_entities);
        child_entt = next_sibling_entt;
    }

    out_entities.push_back(entity);
}

static void MarkNodeVisibilityRenderSync(Scene& scene, entt::entity entity) {
    auto& registry = scene.r();
    if (entity == entt::null || !registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
        return;
    }

    MarkSceneMeshRebuild(scene);

    Array<entt::entity> subtree_entities;
    CollectNodeSubtreePostOrder(registry, entity, subtree_entities);
    for (entt::entity subtree_entity : subtree_entities) {
        if (!registry.valid(subtree_entity)) {
            continue;
        }
        if (registry.all_of<ecs::CLight>(subtree_entity)) {
            registry.emplace_or_replace<ecs::CTagNeedUpdateLight>(subtree_entity);
        }
    }
}

static bool SetNodeVisibility(Scene& scene, entt::entity entity, bool visible, bool game_visibility) {
    if (!scene.IsValidNodeEntity(entity)) {
        return false;
    }

    Scene::NodeVisibility current_visibility = scene.GetNodeVisibility(entity);
    const bool current_local_visibility =
        game_visibility ? current_visibility.visible_in_game : current_visibility.visible_in_editor;
    if (current_local_visibility == visible) {
        return true;
    }

    auto& registry             = scene.r();
    auto& visibility_component = registry.get_or_emplace<ecs::CVisibility>(entity);
    if (game_visibility) {
        visibility_component.visible_in_game = visible;
    } else {
        visibility_component.visible_in_editor = visible;
    }

    if (visibility_component.visible_in_editor && visibility_component.visible_in_game) {
        registry.remove<ecs::CVisibility>(entity);
    }

    if (game_visibility) {
        MarkNodeVisibilityRenderSync(scene, entity);
    }
    return true;
}

static void EnsureMainCameraExists(ecs::LogicalScene& logical_scene, entt::registry& registry) {
    if (registry.view<ecs::CTagMainCamera>().front() != entt::null) {
        return;
    }

    if (const entt::entity camera_entity = registry.view<ecs::CCamera>().front();
        camera_entity != entt::null) {
        registry.emplace<ecs::CTagMainCamera>(camera_entity);
        return;
    }

    logical_scene.UCreateDefaultCamera();
}

static void EnsureMainLightExists(ecs::LogicalScene& logical_scene, entt::registry& registry) {
    if (registry.view<ecs::CLightDirectional, ecs::CTagMainLight>().front() != entt::null ||
        registry.view<ecs::CLightPoint, ecs::CTagMainLight>().front() != entt::null) {
        return;
    }

    if (const entt::entity directional_light_entity = registry.view<ecs::CLightDirectional>().front();
        directional_light_entity != entt::null) {
        registry.emplace<ecs::CTagMainLight>(directional_light_entity);
        return;
    }

    if (const entt::entity point_light_entity = registry.view<ecs::CLightPoint>().front();
        point_light_entity != entt::null) {
        registry.emplace<ecs::CTagMainLight>(point_light_entity);
        return;
    }

    logical_scene.UCreateDefaultLights();
}

static PrimitiveCreateInfo BuildCubePrimitiveData(entt::entity material_entt) {
    constexpr float h = 0.5f;

    PrimitiveCreateInfo info{};
    info.name          = "Runtime Cube Primitive";
    info.material_entt = material_entt;

    info.positions = {
        float3(-h, -h, h),  float3(h, -h, h),  float3(h, h, h),    float3(-h, h, h),  float3(h, -h, -h),
        float3(-h, -h, -h), float3(-h, h, -h), float3(h, h, -h),   float3(h, -h, h),  float3(h, -h, -h),
        float3(h, h, -h),   float3(h, h, h),   float3(-h, -h, -h), float3(-h, -h, h), float3(-h, h, h),
        float3(-h, h, -h),  float3(-h, h, h),  float3(h, h, h),    float3(h, h, -h),  float3(-h, h, -h),
        float3(-h, -h, -h), float3(h, -h, -h), float3(h, -h, h),   float3(-h, -h, h),
    };

    info.normals = {
        float3(0.f, 0.f, 1.f),  float3(0.f, 0.f, 1.f),  float3(0.f, 0.f, 1.f),  float3(0.f, 0.f, 1.f),
        float3(0.f, 0.f, -1.f), float3(0.f, 0.f, -1.f), float3(0.f, 0.f, -1.f), float3(0.f, 0.f, -1.f),
        float3(1.f, 0.f, 0.f),  float3(1.f, 0.f, 0.f),  float3(1.f, 0.f, 0.f),  float3(1.f, 0.f, 0.f),
        float3(-1.f, 0.f, 0.f), float3(-1.f, 0.f, 0.f), float3(-1.f, 0.f, 0.f), float3(-1.f, 0.f, 0.f),
        float3(0.f, 1.f, 0.f),  float3(0.f, 1.f, 0.f),  float3(0.f, 1.f, 0.f),  float3(0.f, 1.f, 0.f),
        float3(0.f, -1.f, 0.f), float3(0.f, -1.f, 0.f), float3(0.f, -1.f, 0.f), float3(0.f, -1.f, 0.f),
    };

    info.texcoord0 = {
        float2(0.f, 0.f), float2(1.f, 0.f), float2(1.f, 1.f), float2(0.f, 1.f), float2(0.f, 0.f),
        float2(1.f, 0.f), float2(1.f, 1.f), float2(0.f, 1.f), float2(0.f, 0.f), float2(1.f, 0.f),
        float2(1.f, 1.f), float2(0.f, 1.f), float2(0.f, 0.f), float2(1.f, 0.f), float2(1.f, 1.f),
        float2(0.f, 1.f), float2(0.f, 0.f), float2(1.f, 0.f), float2(1.f, 1.f), float2(0.f, 1.f),
        float2(0.f, 0.f), float2(1.f, 0.f), float2(1.f, 1.f), float2(0.f, 1.f),
    };

    for (uint32 face = 0; face < 6; ++face) {
        const uint32 base = face * 4;
        info.indices.push_back(base + 0);
        info.indices.push_back(base + 1);
        info.indices.push_back(base + 2);
        info.indices.push_back(base + 0);
        info.indices.push_back(base + 2);
        info.indices.push_back(base + 3);
    }

    info.aabb     = Box3D(float3(-h, -h, -h), float3(h, h, h));
    info.has_aabb = true;
    return info;
}

static PrimitiveCreateInfo BuildFacetedSpherePrimitiveData(entt::entity material_entt) {
    constexpr float  radius             = 0.5f;
    constexpr float  pi                 = 3.14159265358979323846f;
    constexpr uint32 longitude_segments = 16;
    constexpr uint32 latitude_segments  = 8;

    PrimitiveCreateInfo info{};
    info.name          = "Runtime FacetedSphere Primitive";
    info.material_entt = material_entt;

    auto sphere_point = [](uint32 latitude, uint32 longitude) {
        const float theta = pi * static_cast<float>(latitude) / static_cast<float>(latitude_segments);
        const float phi   = 2.f * pi * static_cast<float>(longitude) / static_cast<float>(longitude_segments);
        const float sin_theta = std::sin(theta);

        return float3(
            radius * sin_theta * std::cos(phi), radius * std::cos(theta), radius * sin_theta * std::sin(phi)
        );
    };

    auto add_flat_triangle = [&](const float3& a, const float3& b, const float3& c) {
        const float3 normal = Normalizef(Cross(b - a, c - a));
        const uint32 base   = static_cast<uint32>(info.positions.size());

        info.positions.push_back(a);
        info.positions.push_back(b);
        info.positions.push_back(c);

        info.normals.push_back(normal);
        info.normals.push_back(normal);
        info.normals.push_back(normal);

        info.indices.push_back(base + 0);
        info.indices.push_back(base + 1);
        info.indices.push_back(base + 2);
    };

    for (uint32 lon = 0; lon < longitude_segments; ++lon) {
        const float3 top  = sphere_point(0, 0);
        const float3 next = sphere_point(1, lon + 1);
        const float3 curr = sphere_point(1, lon);
        add_flat_triangle(top, next, curr);
    }

    for (uint32 lat = 1; lat + 1 < latitude_segments; ++lat) {
        for (uint32 lon = 0; lon < longitude_segments; ++lon) {
            const float3 north_curr = sphere_point(lat, lon);
            const float3 north_next = sphere_point(lat, lon + 1);
            const float3 south_curr = sphere_point(lat + 1, lon);
            const float3 south_next = sphere_point(lat + 1, lon + 1);

            add_flat_triangle(north_curr, north_next, south_curr);
            add_flat_triangle(south_curr, north_next, south_next);
        }
    }

    for (uint32 lon = 0; lon < longitude_segments; ++lon) {
        const float3 curr   = sphere_point(latitude_segments - 1, lon);
        const float3 next   = sphere_point(latitude_segments - 1, lon + 1);
        const float3 bottom = sphere_point(latitude_segments, 0);
        add_flat_triangle(curr, next, bottom);
    }

    info.aabb     = Box3D(float3(-radius, -radius, -radius), float3(radius, radius, radius));
    info.has_aabb = true;
    return info;
}

static PrimitiveCreateInfo
BuildProceduralPrimitiveData(EProceduralPrimitiveShape shape, entt::entity material_entt) {
    switch (shape) {
        case EProceduralPrimitiveShape::Cube:
            return BuildCubePrimitiveData(material_entt);
        case EProceduralPrimitiveShape::FacetedSphere:
            return BuildFacetedSpherePrimitiveData(material_entt);
    }
    return BuildCubePrimitiveData(material_entt);
}

} // namespace

////////////////////////
// MARK: 场景修改 API
////////////////////////

// 创建普通 entity，不接入 scene node 树，也不触发 scene sync
entt::entity Scene::CreateEntity(std::string_view name) {
    return logical_scene().UCreateEntity(name);
}

bool Scene::SetNodeName(entt::entity entity, std::string_view name) {
    if (!IsValidNodeEntity(entity)) {
        return false;
    }

    Patch<ecs::CNode>(entity, [&](ecs::CNode& c_node) {
        c_node.name = std::string(name);
    });
    return true;
}

bool Scene::SetNodeTranslation(entt::entity entity, const float3& value) {
    if (!IsValidNodeEntity(entity)) {
        return false;
    }

    Patch<ecs::CNode>(entity, [&](ecs::CNode& c_node) {
        c_node.translation = value;
    });
    return true;
}

bool Scene::SetNodeRotation(entt::entity entity, const Quaternion& value) {
    if (!IsValidNodeEntity(entity)) {
        return false;
    }

    Patch<ecs::CNode>(entity, [&](ecs::CNode& c_node) {
        c_node.rotation = value;
    });
    return true;
}

bool Scene::SetNodeScale(entt::entity entity, const float3& value) {
    if (!IsValidNodeEntity(entity)) {
        return false;
    }

    Patch<ecs::CNode>(entity, [&](ecs::CNode& c_node) {
        c_node.scale = value;
    });
    return true;
}

bool Scene::SetNodeVisibleInEditor(entt::entity entity, bool visible) {
    return SetNodeVisibility(*this, entity, visible, false);
}

bool Scene::SetNodeVisibleInGame(entt::entity entity, bool visible) {
    return SetNodeVisibility(*this, entity, visible, true);
}

// 创建带 CNode 的 entity，并接入 parent 或 root node
entt::entity Scene::CreateEntityWithNode(const EntityWithNodeCreateInfo& create_info) {
    entt::entity entity = logical_scene().UCreateEntityWithNode(create_info);
    if (entity != entt::null) {
        r().emplace_or_replace<ecs::CTagNeedUpdateTransform>(entity);
    }
    return entity;
}

// 创建带 CNode 和 CRenderable 的 entity，并复用已有 mesh 资源
entt::entity Scene::CreateRenderableWithNode(const RenderableCreateInfo& create_info) {
    entt::entity entity = logical_scene().UCreateRenderableWithNode(create_info);
    if (entity == entt::null) {
        return entt::null;
    }

    auto& registry = r();
    registry.emplace_or_replace<ecs::CTagNeedUpdateTransform>(entity);
    MarkSceneMeshRebuild(*this);
    return entity;
}

// 创建运行时 Material，并标记为需要创建 render-side material slot。
entt::entity Scene::CreateMaterial(const MaterialCreateInfo& create_info) {
    entt::entity entity = logical_scene().UCreateMaterial(create_info);
    if (entity != entt::null) {
        r().emplace_or_replace<ecs::CTagNeedCreateMaterial>(entity);
    }
    return entity;
}

// 创建运行时 Primitive Data，并 append 到 CtxMegaBuffers。
entt::entity Scene::CreatePrimitive(const PrimitiveCreateInfo& create_info) {
    entt::entity entity = logical_scene().UCreatePrimitive(create_info);
    if (entity != entt::null) {
        MarkSceneMeshRebuild(*this);
        MarkSceneRtBlasRebuild(*this);
    }
    return entity;
}

// 创建运行时 Mesh，第一版只做全量 mesh resource rebuild。
entt::entity Scene::CreateMesh(const MeshCreateInfo& create_info) {
    entt::entity entity = logical_scene().UCreateMesh(create_info);
    if (entity != entt::null) {
        MarkSceneMeshRebuild(*this);
        MarkSceneRtBlasRebuild(*this);
    }
    return entity;
}

// 创建简单 procedural material + primitive + mesh + renderable。
CreateProceduralRenderableResult
Scene::CreateProceduralRenderable(const ProceduralMeshCreateInfo& create_info) {
    CreateProceduralRenderableResult result{};

    MaterialCreateInfo material_info = create_info.material;
    if (material_info.name.empty()) {
        material_info.name = "Runtime Procedural Material";
    }

    result.material_entt = CreateMaterial(material_info);
    if (result.material_entt == entt::null) {
        return result;
    }

    PrimitiveCreateInfo primitive_info =
        BuildProceduralPrimitiveData(create_info.shape, result.material_entt);
    result.primitive_entt = CreatePrimitive(primitive_info);
    if (result.primitive_entt == entt::null) {
        return result;
    }

    MeshCreateInfo mesh_info{};
    mesh_info.name = "Runtime Procedural Mesh";
    mesh_info.primitive_entts.push_back(result.primitive_entt);
    result.mesh_entt = CreateMesh(mesh_info);
    if (result.mesh_entt == entt::null) {
        return result;
    }

    RenderableCreateInfo renderable_info{};
    renderable_info.mesh_entt        = result.mesh_entt;
    renderable_info.parent_node_entt = create_info.parent_node_entt;
    renderable_info.name             = create_info.name;
    renderable_info.translation      = create_info.translation;
    renderable_info.rotation         = create_info.rotation;
    renderable_info.scale            = create_info.scale;
    result.renderable_entt           = CreateRenderableWithNode(renderable_info);

    LOG_INFO(
        "Runtime procedural renderable created: material={}, primitive={}, mesh={}, renderable={}",
        static_cast<uint32>(entt::to_integral(result.material_entt)),
        static_cast<uint32>(entt::to_integral(result.primitive_entt)),
        static_cast<uint32>(entt::to_integral(result.mesh_entt)),
        static_cast<uint32>(entt::to_integral(result.renderable_entt))
    );

    return result;
}

// 修改已有 EntityWithNode 的 local transform，并标记 transform 同步
bool Scene::SetLocalTransform(entt::entity entity, const Transform& local_transform) {
    if (!logical_scene().USetLocalTransform(entity, local_transform)) {
        return false;
    }

    MarkDirty<ecs::CNode>(entity);
    return true;
}

// 将已有 EntityWithNode 重挂到新的 parent node 下
bool Scene::AttachToParent(entt::entity child_entt, entt::entity parent_entt) {
    entt::entity old_parent_entt = entt::null;
    bool         did_change      = false;
    if (!logical_scene().UAttachToParent(child_entt, parent_entt, &old_parent_entt, &did_change)) {
        return false;
    }
    if (!did_change) {
        return true;
    }

    MarkSceneNodeDirtyIfValid(*this, old_parent_entt);
    MarkDirty<ecs::CNode>(child_entt);
    return true;
}

// 将已有 EntityWithNode 从当前 parent 下移除，并挂回 root node
bool Scene::DetachFromParent(entt::entity child_entt) {
    entt::entity old_parent_entt = entt::null;
    bool         did_change      = false;
    if (!logical_scene().UDetachFromParent(child_entt, &old_parent_entt, &did_change)) {
        return false;
    }
    if (!did_change) {
        return true;
    }

    MarkSceneNodeDirtyIfValid(*this, old_parent_entt);
    MarkDirty<ecs::CNode>(child_entt);
    return true;
}

// 删除普通 entity 或 leaf EntityWithNode，复杂 render-side entity 暂不支持
bool Scene::DestroyEntity(entt::entity entity) {
    entt::entity old_parent_entt = entt::null;
    if (!logical_scene().UDestroyEntity(entity, &old_parent_entt)) {
        return false;
    }

    MarkSceneNodeDirtyIfValid(*this, old_parent_entt);
    return true;
}

bool Scene::DestroyNodeSubtree(entt::entity entity) {
    auto& registry = r();
    if (entity == entt::null || !registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
        LOG_ERROR(
            "[Scene API Error] Cannot destroy node subtree because entity is invalid or missing CNode."
        );
        return false;
    }
    if (registry.all_of<ecs::CTagRootNode>(entity)) {
        LOG_ERROR("[Scene API Error] Cannot destroy root node subtree.");
        return false;
    }

    Array<entt::entity> subtree_entities;
    CollectNodeSubtreePostOrder(registry, entity, subtree_entities);

    auto& root_node = registry.get<ecs::CNode>(entity);
    logical_scene().UDetachNodeFromParent(entity, root_node);

    for (entt::entity subtree_entity : subtree_entities) {
        if (registry.valid(subtree_entity)) {
            registry.destroy(subtree_entity);
        }
    }

    EnsureMainCameraExists(logical_scene(), registry);
    EnsureMainLightExists(logical_scene(), registry);

    logical_scene().SUpdateAllNodeTransformAndAABB();
    logical_scene().SUpdateAllLightData();

    // 这里会整体替换 GpuScene，先等待飞行中的命令完成，避免销毁仍在使用的资源。
    Render::RenderDevice::Get().WaitIdle();
    m_cpu_scene                      = MakeUnique<CpuScene>(*m_logical_scene);
    m_gpu_scene                      = MakeUnique<Render::GpuScene>(*m_cpu_scene, bindless_array());
    m_has_pending_gpu_scene_commands = true;
    SceneInternal::ClearSceneSyncTags(registry);
    return true;
}

// 删除 renderable 会在后续 Tick 中触发 mesh instance cache rebuild，当前先接受这部分开销
bool Scene::DestroyRenderable(entt::entity renderable_entity) {
    entt::entity old_parent_entt = entt::null;
    if (!logical_scene().UDestroyRenderable(renderable_entity, &old_parent_entt)) {
        return false;
    }

    MarkSceneNodeDirtyIfValid(*this, old_parent_entt);
    MarkSceneMeshRebuild(*this);
    return true;
}

// 创建运行时 PointLight，并标记为需要创建 render-side light slot。
entt::entity Scene::CreatePointLight(const PointLightCreateInfo& create_info) {
    entt::entity light_entity = logical_scene().UCreatePointLight(create_info);
    if (light_entity == entt::null) {
        return entt::null;
    }

    auto& registry = r();

    registry.emplace_or_replace<ecs::CTagNeedCreateLight>(light_entity);
    registry.emplace_or_replace<ecs::CTagNeedUpdateTransform>(light_entity);

    return light_entity;
}

// 删除 point light 会在后续 Tick 中触发 light cache rebuild，当前先接受这部分开销
bool Scene::DestroyPointLight(entt::entity light_entity) {
    auto& registry = r();

    if (!logical_scene().UCanDestroyPointLight(light_entity)) {
        return false;
    }

    if (registry.all_of<ecs::CTagNeedUpdateLight>(light_entity)) {
        registry.remove<ecs::CTagNeedUpdateLight>(light_entity);
    }
    if (registry.all_of<ecs::CTagNeedCreateLight>(light_entity)) {
        registry.remove<ecs::CTagNeedCreateLight>(light_entity);
    }
    if (registry.all_of<ecs::CTagNeedUpdateTransform>(light_entity)) {
        registry.remove<ecs::CTagNeedUpdateTransform>(light_entity);
    }

    registry.emplace_or_replace<ecs::CTagNeedDestroyLight>(light_entity);
    return true;
}

///////////////
// Scene Dirty
///////////////

// 标记 entity 的 Light 渲染数据需要同步到 CpuScene/GpuScene。
static void MarkNeedUpdateLight(entt::registry& registry, entt::entity entity) {
    registry.emplace_or_replace<ecs::CTagNeedUpdateLight>(entity);
}

// 标记 entity 的 Material 渲染数据需要同步到 CpuScene/GpuScene。
static void MarkNeedUpdateMaterial(entt::registry& registry, entt::entity entity) {
    registry.emplace_or_replace<ecs::CTagNeedUpdateMaterial>(entity);
}

// 标记方向光参数或派生数据需要刷新并同步到渲染场景。
template<>
void Scene::MarkDirty<ecs::CLightDirectional>(entt::entity entity) {
    auto& registry = r();
    auto& light    = registry.get<ecs::CLightDirectional>(entity);

    light.is_dirty = true;
    MarkNeedUpdateLight(registry, entity);
}

// 标记点光参数或派生数据需要刷新并同步到渲染场景。
template<>
void Scene::MarkDirty<ecs::CLightPoint>(entt::entity entity) {
    auto& registry = r();
    auto& light    = registry.get<ecs::CLightPoint>(entity);

    light.is_dirty = true;
    MarkNeedUpdateLight(registry, entity);
}

// 标记已有 Material slot 需要原地刷新，不涉及新增材质创建。
template<>
void Scene::MarkDirty<ecs::CMaterial>(entt::entity entity) {
    auto& registry = r();

    MarkNeedUpdateMaterial(registry, entity);
}

// 标记 Node 派生数据需要刷新，并联动同 entity 上的 Light 同步。
template<>
void Scene::MarkDirty<ecs::CNode>(entt::entity entity) {
    auto& registry = r();
    auto& node     = registry.get<ecs::CNode>(entity);

    node.is_dirty = true;
    registry.emplace_or_replace<ecs::CTagNeedUpdateTransform>(entity);

    if (registry.all_of<ecs::CLightDirectional>(entity)) {
        registry.get<ecs::CLightDirectional>(entity).is_dirty = true;
        MarkNeedUpdateLight(registry, entity);
    }
    if (registry.all_of<ecs::CLightPoint>(entity)) {
        registry.get<ecs::CLightPoint>(entity).is_dirty = true;
        MarkNeedUpdateLight(registry, entity);
    }
}

} // namespace Moer
