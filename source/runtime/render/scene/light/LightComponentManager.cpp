#include "scene/light/LightComponentManager.h"

#include "scene/ECS.h"
#include "scene/light/LightComponent.h"

namespace Moer {

LightComponentRef LightComponentManager::Get(Entity _entity) noexcept {
    return m_manager[_entity];
}

void LightComponentManager::Put(Entity _entity, LightComponentRef _light) noexcept {
    m_manager.AddComponent(_entity);
    m_manager[_entity] = _light;
}

bool LightComponentManager::HasComponent(Entity _entity) const noexcept {
    return m_manager.HasComponent(_entity);
}

void LightComponentManager::Destroy(Entity _entity) noexcept {
    m_manager.RemoveComponent(_entity);
}
uint LightComponentManager::GetLightCount() const noexcept {
    return m_manager.GetComponentCount();
}

LightComponentManager& LightComponentManager::Get() noexcept {
    static UniquePtr<LightComponentManager> m_instance = nullptr;

    if (m_instance == nullptr) {
        m_instance = std::move(UniquePtr<LightComponentManager>(MoerNew(LightComponentManager)()));
    }
    return *m_instance;
}

} // namespace Moer