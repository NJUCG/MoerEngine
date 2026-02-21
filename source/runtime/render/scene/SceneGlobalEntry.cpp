#include "SceneGlobalEntry.h"
#include "Scene.h"
#include "log/LogSystem.h"
#include <mutex>

namespace Moer {

namespace {
// 静态 mutex，用于保护 m_scene 的访问
std::mutex& GetMutex() {
    static std::mutex s_mutex;
    return s_mutex;
}
} // namespace

SceneGlobalEntry& SceneGlobalEntry::Get() {
    static SceneGlobalEntry instance;
    return instance;
}

void SceneGlobalEntry::BindScene(Scene* scene) {
    std::lock_guard<std::mutex> lock(GetMutex());
    m_scene = scene;
}

Scene* SceneGlobalEntry::GetScene() {
    std::lock_guard<std::mutex> lock(GetMutex());
    if (m_scene == nullptr) {
        LOG_ERROR("SceneGlobalEntry::GetScene() called but no Scene is bound");
    }
    return m_scene;
}

const Scene* SceneGlobalEntry::GetScene() const {
    std::lock_guard<std::mutex> lock(GetMutex());
    if (m_scene == nullptr) {
        LOG_ERROR("SceneGlobalEntry::GetScene() called but no Scene is bound");
    }
    return m_scene;
}

} // namespace Moer
