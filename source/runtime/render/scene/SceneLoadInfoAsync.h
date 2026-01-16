#pragma once

#include <mutex>

namespace Moer {

class SceneLoadInfoAsync {

public:
    void StartLoading();

    void FinishLoading();

    void Reset();

    bool IsSceneStartLoading() const;

    bool IsSceneReady() const;

    SceneLoadInfoAsync();

    ~SceneLoadInfoAsync() = default;

    SceneLoadInfoAsync(const SceneLoadInfoAsync&)            = delete;
    SceneLoadInfoAsync& operator=(const SceneLoadInfoAsync&) = delete;

private:
    mutable std::mutex m_mutex;
    bool               m_is_scene_start_loading = false;
    bool               m_is_scene_ready         = false;
};

} // namespace Moer