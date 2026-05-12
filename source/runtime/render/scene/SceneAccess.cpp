#include "Scene.h"

#include "rhi/RHI.h"

namespace Moer {

///////////////////////////
// MARK: CPU / GPU Scene API
///////////////////////////

const Render::GpuScene::Res& Scene::gpu_scene_res() const {
    assert(m_gpu_scene && "Scene is not ready");
    return m_gpu_scene->res();
}

const Render::GpuScene::Res& Scene::GetGpuSceneRes() const {
    return gpu_scene_res();
}

void Scene::RestoreDrawCommands(Render::CommandList& cmd_list) {
    assert(m_gpu_scene && "Scene is not ready");
    m_gpu_scene->RestoreDrawCommands(cmd_list);
}

const CpuScene& Scene::cpu_scene() const {
    assert(m_cpu_scene && "Scene is not ready");
    return *m_cpu_scene;
}

const CpuScene& Scene::GetCpuScene() const {
    return cpu_scene();
}

Render::BindlessArrayRef Scene::bindless_array() {
    if (!m_bindless_array) {
        m_bindless_array = Render::RenderDevice::Get().CreateBindlessArray();
    }
    return m_bindless_array;
}

Render::BindlessArrayRef Scene::GetBindlessArray() {
    return bindless_array();
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