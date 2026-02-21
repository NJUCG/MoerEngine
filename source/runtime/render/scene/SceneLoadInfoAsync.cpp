#include "SceneLoadInfoAsync.h"

#include <cassert>
#include <mutex>

namespace Moer {

void SceneLoadInfoAsync::StartLoading() {
    std::lock_guard<std::mutex> lock(m_mutex);
    assert(
        !m_is_scene_start_loading &&
        "Scene is already started loading. Please Reset() before starting loading again."
    );
    assert(!m_is_scene_ready && "Scene is already ready. Please Reset() before starting loading again.");
    m_is_scene_start_loading = true;
}

void SceneLoadInfoAsync::FinishLoading() {
    std::lock_guard<std::mutex> lock(m_mutex);
    assert(m_is_scene_start_loading && "Scene must be started loading before finishing loading.");
    m_is_scene_ready = true;
}

void SceneLoadInfoAsync::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_scene_start_loading = false;
    m_is_scene_ready         = false;
}

bool SceneLoadInfoAsync::IsSceneStartLoading() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_is_scene_start_loading;
}

bool SceneLoadInfoAsync::IsSceneReady() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_is_scene_ready;
}

SceneLoadInfoAsync::SceneLoadInfoAsync() {
    Reset();
}

} // namespace Moer