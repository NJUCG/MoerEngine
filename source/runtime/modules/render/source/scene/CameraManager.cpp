#include "scene/CameraManager.h"

namespace Moer {
    CameraRef CameraManager::Get(Entity entity) noexcept {
        return m_manager[entity].camera;
    }
    CameraRef CameraManager::Create(Entity entity) noexcept {
        m_manager.AddComponent(entity);
        m_manager[entity].camera = MoerNew(Camera)();
        return m_manager[entity].camera;
    }
    void CameraManager::Put(Entity entity, CameraRef camera) noexcept {
        m_manager.AddComponent(entity);
        m_manager[entity].camera = camera;
    }
    bool CameraManager::HasComponent(Entity entity) const noexcept {
        return m_manager.HasComponent(entity);
    }
    void CameraManager::Destroy(Entity entity) noexcept {
        m_manager.RemoveComponent(entity);
    }
    CameraManager& CameraManager::Get() noexcept {
        static UniquePtr<CameraManager> m_instance = nullptr;

        if (m_instance == nullptr) {
            m_instance = std::move(UniquePtr<CameraManager>(MoerNew(CameraManager)()));
        }
        return *m_instance;
    }
}// namespace Moer