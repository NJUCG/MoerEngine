#pragma once

#include "scene/cache/SceneCacheSerializer.h"
#include <entt/entt.hpp>

namespace Moer {
// v7 adds shadow flags/settings to the v6 extension; the v5 Mesh payload is unchanged.
bool WriteGaussianCacheExtension(
    SceneCacheBinaryWriter&    writer,
    const entt::registry&      registry,
    const Array<entt::entity>& entities
);
bool ReadGaussianCacheExtension(
    SceneCacheBinaryReader&    reader,
    entt::registry&            registry,
    const Array<entt::entity>& entities,
    uint32 version = 7
);
} // namespace Moer
