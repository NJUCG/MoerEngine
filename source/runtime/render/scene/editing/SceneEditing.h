#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"

namespace Moer {

class Scene;

namespace SceneEditing {

RENDER_API bool TryGetMainDirectionalLightDirection(const Scene& scene, float3& out_direction);
RENDER_API bool SetMainDirectionalLightDirection(Scene& scene, const float3& direction);

} // namespace SceneEditing

} // namespace Moer