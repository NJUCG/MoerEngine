#pragma once

#include "RenderAPI.h"
#include <filesystem>

namespace Moer::ecs {
class LogicalScene;
}

namespace Moer::Gltf {

class Parser {
public:
    Parser() noexcept;
    ~Parser() noexcept;

    // PImpl模式
    static RENDER_API bool
    LoadSceneFromFile(ecs::LogicalScene& out_logical_scene, const std::filesystem::path& file_path) noexcept;

private:
    // PImpl模式
    struct Impl;
    Impl* m_impl = nullptr;
};
} // namespace Moer::Gltf