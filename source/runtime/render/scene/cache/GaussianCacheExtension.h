#pragma once

#include "scene/cache/SceneCacheSerializer.h"
#include <entt/entt.hpp>

namespace Moer {
// v6 appends Gaussian data after the unchanged v5 entity/mesh payload.
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
