#pragma once
#include "RenderAPI.h"
#include <filesystem>
#include <string>
namespace Moer {
namespace ecs {
class LogicalScene;
}
// Portable source references and editable state; never serializes derived GPU data or modifies PLY.
RENDER_API bool SaveGaussianSceneFile(
    const std::filesystem::path& path,
    const ecs::LogicalScene&     scene,
    std::string&                 out_error
);
RENDER_API bool LoadGaussianSceneFile(
    const std::filesystem::path& path,
    ecs::LogicalScene&           out_scene,
    std::string&                 out_error
);
} // namespace Moer
