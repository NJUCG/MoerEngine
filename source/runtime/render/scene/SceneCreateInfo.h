#pragma once

#include "math/Quaternion.h"
#include "misc/Traits.h"

#include <entt/entity/entity.hpp>
#include <string>
#include <string_view>

namespace Moer {

struct PointLightCreateInfo {
    float3       position              = float3(0.f, 0.f, 0.f);
    float3       color                 = float3(1.f, 1.f, 1.f);
    float        intensity             = 1.f;
    std::string  name                  = "Runtime Point Light";
    entt::entity parent_node_entt      = entt::null;
    bool         should_set_main_light = false;
};

struct EntityWithNodeCreateInfo {
    entt::entity     parent_node_entt = entt::null;
    std::string_view name             = {};
    float3           translation      = float3(0.f, 0.f, 0.f);
    Quaternion       rotation         = Quaternion();
    float3           scale            = float3(1.f, 1.f, 1.f);
};

struct RenderableCreateInfo {
    entt::entity     mesh_entt        = entt::null;
    entt::entity     parent_node_entt = entt::null;
    std::string_view name             = {};
    float3           translation      = float3(0.f, 0.f, 0.f);
    Quaternion       rotation         = Quaternion();
    float3           scale            = float3(1.f, 1.f, 1.f);
};

} // namespace Moer