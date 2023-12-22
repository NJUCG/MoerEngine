#include "scene/CameraManager.h"

namespace Moer {
    CameraRef CameraManager::Get(Entity entity) noexcept {
        return m_manager[entity].camera;
    }
    CameraRef CameraManager::Create(Entity entity) noexcept {
        m_manager.AddComponent(entity);
        m_manager[entity].camera = new Camera();
        return m_manager[entity].camera;
    }
    bool CameraManager::HasComponent(Entity entity) const noexcept {
        return m_manager.HasComponent(entity);
    }
    void CameraManager::Destroy(Entity entity) noexcept {
        m_manager.RemoveComponent(entity);
    }
    CameraManager& CameraManager::Get() noexcept {
        if (m_instance == nullptr) {
            m_instance = std::make_unique<CameraManager>();
        }
        return *m_instance;
    }
}