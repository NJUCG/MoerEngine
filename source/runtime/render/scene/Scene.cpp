#include "Scene.h"

#include "loader/LoaderInterface.h"
#include "rhi/RHI.h"
#include "taskgraph/TaskGraph.h"

namespace Moer {

Scene::Scene() {
    m_bindless_array = Render::RenderDevice::Get().CreateBindlessArray();
}

void Scene::LoadSceneFromFileAsync(const std::filesystem::path& file_path) {
    LambdaTask::Dispatch([this, file_path]() {
        // start
        this->m_scene_load_info.StartLoading();

        // 1. logical scene
        this->m_logical_scene = MakeUnique<ecs::LogicalScene>();
        bool result           = LoaderInterface::LoadSceneFromFile(*this->m_logical_scene, file_path);

        // failed in LogicalScene loading
        if (!result) {
            this->m_scene_load_info.Reset();
            return;
        }

        // 2. cpu scene
        this->m_cpu_scene = MakeUnique<CpuScene>(*this->m_logical_scene);

        // 3. gpu scene
        this->m_gpu_scene = MakeUnique<Render::GpuScene>(*this->m_cpu_scene, this->bindless_array());

        // finish
        this->m_scene_load_info.FinishLoading();
    });
}

bool Scene::IsStartLoading() const {
    return m_scene_load_info.IsSceneStartLoading();
}

bool Scene::IsReady() const {
    return m_scene_load_info.IsSceneReady();
}

void Scene::Tick() {
    // TODO
}

void Scene::Reset() {
    m_bindless_array = nullptr;

    m_logical_scene.reset();
    m_cpu_scene.reset();
    m_gpu_scene.reset();

    m_scene_load_info.Reset();
}

// MARK: 一系列public getter

ecs::LogicalScene& Scene::logical_scene() {
    assert(m_logical_scene && "Scene is not ready");
    return *m_logical_scene;
}

ecs::LogicalScene& Scene::GetLogicalScene() {
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

const Render::GpuScene::Res& Scene::gpu_scene_res() const {
    assert(m_gpu_scene && "Scene is not ready");
    return m_gpu_scene->res();
}

const Render::GpuScene::Res& Scene::GetGpuSceneRes() const {
    return gpu_scene_res();
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

/**
 * MARK: 封装一些常用逻辑
 */

entt::entity Scene::GetMainCameraEntity() const {
    return r().view<ecs::CTagMainCamera>().front();
}

entt::entity Scene::GetMainDirectionalLightEntity() const {
    return r().view<ecs::CLightDirectional, ecs::CTagMainLight>().front();
}

entt::entity Scene::GetMainPointLightEntity() const {
    return r().view<ecs::CLightPoint, ecs::CTagMainLight>().front();
}

ecs::CCamera& Scene::GetMainCamera() {
    return r().get<ecs::CCamera>(GetMainCameraEntity());
}

const ecs::CLightDirectional& Scene::GetMainDirectionalLight() const {
    return r().get<ecs::CLightDirectional>(GetMainDirectionalLightEntity());
}

const ecs::CLightPoint& Scene::GetMainPointLight() const {
    return r().get<ecs::CLightPoint>(GetMainPointLightEntity());
}

const ecs::CTransform& Scene::GetTransform(entt::entity entity) const {
    return r().get<ecs::CTransform>(entity);
}

} // namespace Moer