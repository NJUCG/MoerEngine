#pragma once

#include "scene/LogicalComponents.h"

#include <entt/entt.hpp>

namespace Moer::SceneInternal {

inline void ClearSceneSyncTags(entt::registry& registry) {
    registry.clear<ecs::CTagNeedUpdateLight>();
    registry.clear<ecs::CTagNeedUpdateMaterial>();
    registry.clear<ecs::CTagNeedUpdateTransform>();
    registry.clear<ecs::CTagNeedCreateLight>();
    registry.clear<ecs::CTagNeedCreateMaterial>();
    registry.clear<ecs::CTagNeedCreateTransform>();
    registry.clear<ecs::CTagNeedDestroyLight>();
    registry.clear<ecs::CTagNeedRebuildMesh>();
    registry.clear<ecs::CTagNeedRebuildRtBlas>();
}

} // namespace Moer::SceneInternal
