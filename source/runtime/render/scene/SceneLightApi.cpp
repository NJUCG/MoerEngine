#include "Scene.h"

#include "log/LogSystem.h"
#include "scene/SceneLightApi.h"

#include <entt/entt.hpp>

namespace Moer {

// 查找运行时创建节点默认挂载的 root node。
static entt::entity GetRootNodeEntity(entt::registry& registry) {
    auto view = registry.view<ecs::CTagRootNode>();
    auto it   = view.begin();
    if (it == view.end()) {
        LOG_ERROR("Cannot create point light because no CTagRootNode exists in the scene.");
        return entt::null;
    }
    return *it;
}

// 创建运行时 PointLight，并标记为需要创建 render-side light slot。
entt::entity Scene::CreatePointLight(const PointLightCreateInfo& create_info) {
    auto& registry = r();

    entt::entity parent_node_entt = create_info.parent_node_entt;
    if (parent_node_entt == entt::null) {
        parent_node_entt = GetRootNodeEntity(registry);
    }
    if (parent_node_entt == entt::null || !registry.valid(parent_node_entt) ||
        !registry.all_of<ecs::CNode>(parent_node_entt)) {
        LOG_ERROR("Cannot create point light because parent node is invalid or missing CNode.");
        return entt::null;
    }

    entt::entity light_entity = registry.create();

    auto& c_node      = registry.emplace<ecs::CNode>(light_entity);
    auto& c_transform = registry.emplace<ecs::CTransform>(light_entity);
    auto& c_light     = registry.emplace<ecs::CLight>(light_entity);
    auto& c_point     = registry.emplace<ecs::CLightPoint>(light_entity);

    registry.emplace<ecs::CName>(light_entity).name = create_info.name;

    c_transform.translation = create_info.position;
    c_transform.is_dirty    = true;

    c_light.type       = ELightType::Point;
    c_point.color      = create_info.color;
    c_point.intensity  = create_info.intensity;
    c_point.is_dirty   = true;
    c_point.d_position = create_info.position;

    if (create_info.should_set_main_light) {
        registry.emplace_or_replace<ecs::CTagMainLight>(light_entity);
    }

    auto& parent_node = registry.get<ecs::CNode>(parent_node_entt);
    logical_scene().UEmplaceNodeToParent(parent_node_entt, parent_node, light_entity, c_node);

    registry.emplace_or_replace<ecs::CTagNeedCreateLight>(light_entity);
    registry.emplace_or_replace<ecs::CTagNeedUpdateTransform>(light_entity);

    return light_entity;
}

} // namespace Moer