#pragma once

#include "Core.h"
#include "misc/Traits.h"

#include <entt/entity/entity.hpp>

namespace Moer {

class Scene;

class InspectorUI {
public:
    void ShowWindow(bool* p_open, Scene* scene, entt::entity& selected_node);

private:
    entt::entity m_rotation_cache_entity = entt::null;
    float3       m_rotation_euler{};
};

} // namespace Moer