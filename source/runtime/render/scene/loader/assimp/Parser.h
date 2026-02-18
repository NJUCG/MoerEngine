#pragma once

#include <filesystem>

namespace Moer { namespace ecs {
class LogicalScene;
}} // namespace Moer::ecs

namespace Moer::assimp {
class Parser {
public:
    Parser()  = default;
    ~Parser() = default;

    static bool
    LoadSceneFromFile(ecs::LogicalScene& out_logical_scene, const std::filesystem::path& file_path);
};
} // namespace Moer::assimp