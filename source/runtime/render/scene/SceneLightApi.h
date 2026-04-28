#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"

#include <entt/entity/entity.hpp>
#include <string>

namespace Moer {

struct PointLightCreateInfo {
    float3       position              = float3(0.f, 0.f, 0.f);
    float3       color                 = float3(1.f, 1.f, 1.f);
    float        intensity             = 1.f;
    std::string  name                  = "Runtime Point Light";
    entt::entity parent_node_entt      = entt::null;
    bool         should_set_main_light = false;
};

} // namespace Moer
