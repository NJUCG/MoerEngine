#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"

namespace Moer {

class Scene;

namespace SceneEditing {

/**
 * SceneEditing 是编辑器 / 工具层语义封装。
 *
 * 这里承接的是“工具意图”级别操作，例如灯光工具、批量编辑、编辑器命令等。
 * 通用的 scene lifecycle / query / mutation 能力应优先落在 Scene，避免把 SceneEditing 变成 registry 包装层。
 */

RENDER_API bool TryGetMainDirectionalLightDirection(const Scene& scene, float3& out_direction);
RENDER_API bool SetMainDirectionalLightDirection(Scene& scene, const float3& direction);
RENDER_API bool AddPointLight(Scene& scene, const float3& position, const float3& color, float intensity);

} // namespace SceneEditing

} // namespace Moer