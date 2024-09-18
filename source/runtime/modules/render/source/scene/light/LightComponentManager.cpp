#include "scene/light/LightComponentManager.h"

#include "scene/light/LightComponent.h"
#include "scene/ECS.h"

namespace Moer {

    LightComponentRef LightComponentManager::Get(Entity entity) noexcept {
        return m_manager[entity];
    }

    void LightComponentManager::Put(Entity entity, LightComponentRef light) noexcept {
        m_manager.AddComponent(entity);
        m_manager[entity] = light;
    }

    bool LightComponentManager::HasComponent(Entity entity) const noexcept {
        return m_manager.HasComponent(entity);
    }

    void LightComponentManager::Destroy(Entity entity) noexcept {
        m_manager.RemoveComponent(entity);
    }

    LightComponentManager& LightComponentManager::Get() noexcept {
        static UniquePtr<LightComponentManager> m_instance = nullptr;

        if (m_instance == nullptr) {
            m_instance = std::move(UniquePtr<LightComponentManager>(MoerNew(LightComponentManager)()));
        }
        return *m_instance;
    }

}// namespace Moer