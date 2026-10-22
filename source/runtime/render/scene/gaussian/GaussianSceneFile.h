#pragma once
#include "RenderAPI.h"
#include <filesystem>
#include <string>
namespace Moer {
namespace ecs {
class LogicalScene;
}
// Read legacy .moergs documents for migration; new saves use the unified .moca cache.
RENDER_API bool LoadGaussianSceneFile(
    const std::filesystem::path& path,
    ecs::LogicalScene&           out_scene,
    std::string&                 out_error
);
} // namespace Moer
