#include "scene/editing/SceneEditing.h"

#include "log/LogSystem.h"
#include "math/Function.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"

namespace Moer::SceneEditing {

namespace {

entt::entity FindMainDirectionalLightEntity(const Scene& scene) {
    const auto& registry = scene.r();

    auto main_light_view = registry.view<ecs::CLightDirectional, ecs::CTagMainLight, ecs::CNode>();
    auto main_light_it   = main_light_view.begin();
    if (main_light_it != main_light_view.end()) {
        return *main_light_it;
    }

    auto fallback_view = registry.view<ecs::CLightDirectional, ecs::CNode>();
    auto fallback_it   = fallback_view.begin();
    if (fallback_it != fallback_view.end()) {
        return *fallback_it;
    }

    return entt::null;
}

float3 SanitizeDirectionalLightDirection(const float3& direction, const float3& fallback_direction) {
    if (Lengthf(direction) <= 1e-5f) {
        return Normalizef(fallback_direction);
    }
    return Normalizef(direction);
}

} // namespace

bool TryGetMainDirectionalLightDirection(const Scene& scene, float3& out_direction) {
    const entt::entity light_entity = FindMainDirectionalLightEntity(scene);
    if (light_entity == entt::null) {
        return false;
    }

    out_direction = Normalizef(scene.r().get<ecs::CLightDirectional>(light_entity).d_direction);
    return true;
}

bool SetMainDirectionalLightDirection(Scene& scene, const float3& direction) {
    const entt::entity light_entity = FindMainDirectionalLightEntity(scene);
    if (light_entity == entt::null) {
        return false;
    }

    const float3 current_direction =
        Normalizef(scene.r().get<ecs::CLightDirectional>(light_entity).d_direction);
    const float3 desired_direction = SanitizeDirectionalLightDirection(direction, current_direction);
    if (Lengthf(desired_direction - current_direction) <= 1e-4f) {
        return true;
    }

    scene.Patch<ecs::CNode>(light_entity, [&](ecs::CNode& c_node) {
        c_node.rotation = Quaternion(float3(0.f, 0.f, -1.f), desired_direction);
    });
    return true;
}

bool AddPointLight(Scene& scene, const float3& position, const float3& color, float intensity) {
    PointLightCreateInfo create_info{};
    create_info.position  = position;
    create_info.color     = color;
    create_info.intensity = intensity;
    create_info.name      = "Editing Point Light";

    const entt::entity light_entity = scene.CreatePointLight(create_info);
    if (light_entity == entt::null) {
        LOG_ERROR(
            "SceneEditing AddPointLight failed: position=({}, {}, {}), color=({}, {}, {}), intensity={}",
            position.x,
            position.y,
            position.z,
            color.x,
            color.y,
            color.z,
            intensity
        );
        return false;
    }

    LOG_INFO(
        "SceneEditing AddPointLight created entity {}: position=({}, {}, {}), color=({}, {}, {}), "
        "intensity={}",
        static_cast<uint32>(entt::to_integral(light_entity)),
        position.x,
        position.y,
        position.z,
        color.x,
        color.y,
        color.z,
        intensity
    );
    return true;
}

} // namespace Moer::SceneEditing