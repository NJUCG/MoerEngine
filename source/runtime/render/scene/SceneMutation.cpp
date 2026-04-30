#include "Scene.h"

namespace Moer {

///////////////
// Scene Mutation
///////////////

static void MarkSceneNodeDirtyIfValid(Scene& scene, entt::entity entity) {
    auto& registry = scene.r();
    if (entity == entt::null || !registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
        return;
    }

    scene.MarkDirty<ecs::CNode>(entity);
}

// 创建普通 entity，不接入 scene node 树，也不触发 scene sync
entt::entity Scene::CreateEntity(std::string_view name) {
    return logical_scene().UCreateEntity(name);
}

// 创建带 CNode 的 entity，并接入 parent 或 root node
entt::entity Scene::CreateEntityWithNode(const EntityWithNodeCreateInfo& create_info) {
    entt::entity entity = logical_scene().UCreateEntityWithNode(create_info);
    if (entity != entt::null) {
        r().emplace_or_replace<ecs::CTagNeedUpdateTransform>(entity);
    }
    return entity;
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