#pragma once

#include "RenderAPI.h"
#include "misc/STL.h"
#include <filesystem>

namespace Moer::ecs {
class LogicalScene;
}

namespace Moer {

enum class ESceneLoadSource {
    None,
    StateCache,
    OriginCache,
    Parser,
};

struct RENDER_API SceneLoadRequest {
    std::filesystem::path file_path;
    bool                  use_state_cache          = true;
    bool                  use_origin_cache         = true;
    bool                  allow_write_origin_cache = true;
};

struct RENDER_API SceneImportResult {
    bool                         success = false;
    ESceneLoadSource             source = ESceneLoadSource::None;
    UniquePtr<ecs::LogicalScene> logical_scene;

    explicit operator bool() const {
        return success;
    }
};

/**
 * Loader Interface
 * 
 * 一个纯static类，负责根据 SceneLoadRequest 加载 LogicalScene。
 * 当前公共入口只有同步接口 LoadScene(...)
 */
class RENDER_API LoaderInterface {

public:
    static SceneImportResult LoadScene(const SceneLoadRequest& request);

private:
    static bool LoadSceneFromFileCommon(
        ecs::LogicalScene&      out_logical_scene,
        const SceneLoadRequest& request,
        ESceneLoadSource*       out_source = nullptr
    );
};

} // namespace Moer
