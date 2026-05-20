#include "Scene.h"

#include "log/LogSystem.h"
#include "scene/NodeNameUtils.h"

namespace Moer {

namespace {

void CollectNodeSubtreeStatsRecursive(
    const entt::registry&    registry,
    entt::entity             entity,
    Scene::NodeSubtreeStats& stats
) {
    if (entity == entt::null || !registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
        return;
    }

    stats.node_count += 1;
    stats.renderable_count += registry.all_of<ecs::CRenderable>(entity) ? 1u : 0u;
    stats.camera_count += registry.all_of<ecs::CCamera>(entity) ? 1u : 0u;
    stats.light_count += registry.all_of<ecs::CLight>(entity) ? 1u : 0u;
    stats.contains_main_camera = stats.contains_main_camera || registry.all_of<ecs::CTagMainCamera>(entity);
    stats.contains_main_light_tag =
        stats.contains_main_light_tag || registry.all_of<ecs::CTagMainLight>(entity);

    entt::entity child_entt = registry.get<ecs::CNode>(entity).first_child_entt;
    while (child_entt != entt::null) {
        const entt::entity next_sibling_entt = registry.get<ecs::CNode>(child_entt).next_sibling_entt;
        CollectNodeSubtreeStatsRecursive(registry, child_entt, stats);
        child_entt = next_sibling_entt;
    }
}

entt::entity FindNodeEntityByNameRecursive(const Scene& scene, entt::entity entity, std::string_view name) {
    if (!scene.IsValidNodeEntity(entity)) {
        return entt::null;
    }

    const auto& node = scene.GetNode(entity);
    if (node.name == name || ecs::GetNodeDisplayName(node, entity) == name) {
        return entity;
    }

    entt::entity matched_entity = entt::null;
    scene.ForEachNodeChild(entity, [&](entt::entity child_entt) {
        if (matched_entity != entt::null) {
            return;
        }

        matched_entity = FindNodeEntityByNameRecursive(scene, child_entt, name);
    });
    return matched_entity;
}

} // namespace

///////////////////////
// MARK: 场景查询 API
///////////////////////

entt::entity Scene::GetRootNodeEntity() const {
    if (!m_logical_scene) {
        return entt::null;
    }
    return m_logical_scene->UGetRootNodeEntity();
}

bool Scene::IsValidNodeEntity(entt::entity entity) const {
    return m_logical_scene && entity != entt::null && r().valid(entity) && r().all_of<ecs::CNode>(entity);
}

bool Scene::IsRootNode(entt::entity entity) const {
    return IsValidNodeEntity(entity) && r().all_of<ecs::CTagRootNode>(entity);
}

uint32 Scene::GetNodeChildCount(entt::entity entity) const {
    if (!IsValidNodeEntity(entity)) {
        return 0;
    }
    return r().get<ecs::CNode>(entity).child_count;
}

std::string Scene::GetNodeDisplayName(entt::entity entity) const {
    if (!IsValidNodeEntity(entity)) {
        return {};
    }

    const auto& node = r().get<ecs::CNode>(entity);
    return ecs::GetNodeDisplayName(node, entity);
}

entt::entity Scene::FindNodeEntityByName(std::string_view name) const {
    if (!m_logical_scene || name.empty()) {
        return entt::null;
    }

    return FindNodeEntityByNameRecursive(*this, GetRootNodeEntity(), name);
}

Scene::NodeSubtreeStats Scene::GetNodeSubtreeStats(entt::entity entity) const {
    NodeSubtreeStats stats{};
    if (!IsValidNodeEntity(entity)) {
        return stats;
    }

    CollectNodeSubtreeStatsRecursive(r(), entity, stats);
    return stats;
}

bool Scene::TryGetNodeName(entt::entity entity, std::string& out_name) const {
    if (!IsValidNodeEntity(entity)) {
        return false;
    }

    out_name = r().get<ecs::CNode>(entity).name;
    return true;
}

std::optional<Scene::NodeLocalTransform> Scene::TryGetNodeLocalTransform(entt::entity entity) const {
    if (!IsValidNodeEntity(entity)) {
        return std::nullopt;
    }

    const auto& node = r().get<ecs::CNode>(entity);
    return NodeLocalTransform{
        .translation = node.translation,
        .rotation    = node.rotation,
        .scale       = node.scale,
    };
}

/////////////////////////////
// MARK: 场景查询 API（常用封装）
/////////////////////////////

entt::entity Scene::GetMainCameraEntity() const {
    auto entity = r().view<ecs::CTagMainCamera>().front();
    if (entity == entt::null) {
        entity = r().view<ecs::CCamera>().front();
        if (entity != entt::null) {
            LOG_WARNING("No main camera tag found in scene. Falling back to the first camera entity.");
            return entity;
        }
        LOG_ERROR("No main camera found in scene");
    }
    return entity;
}

entt::entity Scene::GetMainDirectionalLightEntity() const {
    // 先尝试找带 MainTag 的
    auto entity = r().view<ecs::CLightDirectional, ecs::CTagMainLight>().front();
    if (entity == entt::null) {
        // 如果没找到，忽略 MainTag，继续找
        entity = r().view<ecs::CLightDirectional>().front();
        if (entity == entt::null) {
            LOG_ERROR("No directional light found in scene");
        }
    }
    return entity;
}

entt::entity Scene::GetMainPointLightEntity() const {
    // 先尝试找带 MainTag 的
    auto entity = r().view<ecs::CLightPoint, ecs::CTagMainLight>().front();
    if (entity == entt::null) {
        // 如果没找到，忽略 MainTag，继续找
        entity = r().view<ecs::CLightPoint>().front();
        if (entity == entt::null) {
            LOG_ERROR("No point light found in scene");
        }
    }
    return entity;
}

ecs::CCamera& Scene::GetMainCamera() {
    auto entity = GetMainCameraEntity();
    if (entity == entt::null || !r().valid(entity) || !r().all_of<ecs::CCamera>(entity)) {
        LOG_ERROR("Invalid main camera entity or missing CCamera component");
        assert(false && "Invalid main camera entity");
    }
    return r().get<ecs::CCamera>(entity);
}

const ecs::CLightDirectional& Scene::GetMainDirectionalLight() const {
    auto entity = GetMainDirectionalLightEntity();
    if (entity == entt::null || !r().valid(entity) || !r().all_of<ecs::CLightDirectional>(entity)) {
        LOG_ERROR("Invalid main directional light entity or missing CLightDirectional component");
        assert(false && "Invalid main directional light entity");
    }
    return r().get<ecs::CLightDirectional>(entity);
}

const ecs::CLightPoint& Scene::GetMainPointLight() const {
    auto entity = GetMainPointLightEntity();
    if (entity == entt::null || !r().valid(entity) || !r().all_of<ecs::CLightPoint>(entity)) {
        LOG_ERROR("Invalid main point light entity or missing CLightPoint component");
        assert(false && "Invalid main point light entity");
    }
    return r().get<ecs::CLightPoint>(entity);
}

const ecs::CNode& Scene::GetNode(entt::entity entity) const {
    if (entity == entt::null || !r().valid(entity) || !r().all_of<ecs::CNode>(entity)) {
        LOG_ERROR("Invalid entity or missing CNode component");
        assert(false && "Invalid entity for GetNode");
    }
    return r().get<ecs::CNode>(entity);
}

} // namespace Moer