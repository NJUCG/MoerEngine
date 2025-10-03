#include "scene/TransformManager.h"
#include <memory>
namespace Moer {
const Transform& TransformManager::Get(Entity entity) noexcept {
    return m_manager[entity].transform;
}
Transform& TransformManager::Create(Entity entity) noexcept {
    m_manager.AddComponent(entity);
    return m_manager[entity].transform;
}
void TransformManager::Set(Entity entity, Transform transform) noexcept {
    m_manager[entity].transform = transform;
}
bool TransformManager::HasComponent(Entity entity) const noexcept {
    return m_manager.HasComponent(entity);
}
void TransformManager::Destroy(Entity entity) noexcept {
    m_manager.RemoveComponent(entity);
}
TransformManager& TransformManager::Get() noexcept {
    static UniquePtr<TransformManager> m_instance = nullptr;
    if (m_instance == nullptr) {
        m_instance = std::move(UniquePtr<TransformManager>(MoerNew(TransformManager)()));
    }
    return *m_instance;
}
} // namespace Moer