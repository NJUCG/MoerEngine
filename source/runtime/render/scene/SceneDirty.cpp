#include "Scene.h"

#include <entt/entt.hpp>

namespace Moer {

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

// 标记 Transform 派生数据需要刷新，并联动同 entity 上的 Light 同步。
template<>
void Scene::MarkDirty<ecs::CTransform>(entt::entity entity) {
    auto& registry  = r();
    auto& transform = registry.get<ecs::CTransform>(entity);

    transform.is_dirty = true;
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