#include "Scene.h"

namespace Moer {

///////////////////////////
// MARK: CPU Scene / Bindless API
///////////////////////////

const CpuScene& Scene::cpu_scene() const {
    assert(m_cpu_scene && "Scene is not ready");
    return *m_cpu_scene;
}

const CpuScene& Scene::GetCpuScene() const {
    return cpu_scene();
}

Render::BindlessArrayRef Scene::bindless_array() {
    assert(m_bindless_array && "Renderer must assign the Scene bindless array before use.");
    return m_bindless_array;
}

Render::BindlessArrayRef Scene::GetBindlessArray() {
    return bindless_array();
}

void Scene::SetBindlessArray(Render::BindlessArrayRef bindless_array) {
    assert(bindless_array);
    m_bindless_array = std::move(bindless_array);
}

///////////////////////////////////////////
// MARK: Logical Scene API
///////////////////////////////////////////

ecs::LogicalScene& Scene::logical_scene() {
    assert(m_logical_scene && "Scene is not ready");
    return *m_logical_scene;
}

const ecs::LogicalScene& Scene::logical_scene() const {
    assert(m_logical_scene && "Scene is not ready");
    return *m_logical_scene;
}

ecs::LogicalScene& Scene::GetLogicalScene() {
    return logical_scene();
}

const ecs::LogicalScene& Scene::GetLogicalScene() const {
    return logical_scene();
}

entt::registry& Scene::r() {
    return logical_scene().r();
}

const entt::registry& Scene::r() const {
    return logical_scene().r();
}

entt::registry& Scene::GetRegistry() {
    return r();
}

const entt::registry& Scene::GetRegistry() const {
    return r();
}

} // namespace Moer
