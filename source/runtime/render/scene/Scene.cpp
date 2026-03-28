#include "Scene.h"

#include "loader/LoaderInterface.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "taskgraph/TaskGraph.h"

namespace Moer {

Scene::Scene() {
    m_bindless_array = Render::RenderDevice::Get().CreateBindlessArray();
}

void Scene::LoadSceneInternal(const std::filesystem::path& file_path) {
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
}

void Scene::LoadSceneFromFileAsync(const std::filesystem::path& file_path) {
    LambdaTask::Dispatch([this, file_path]() {
        this->LoadSceneInternal(file_path);
    });
}

void Scene::LoadSceneFromFile(const std::filesystem::path& file_path) {
    this->LoadSceneInternal(file_path);
}

bool Scene::IsStartLoading() const {
    return m_scene_load_info.IsSceneStartLoading();
}

bool Scene::IsReady() const {
    return m_scene_load_info.IsSceneReady();
}

Render::GpuScene::PendingCommandList&& Scene::PopPendingCommandList() {
    return std::move(m_gpu_scene->PopPendingCommandList());
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

const Render::GpuScene::Res& Scene::gpu_scene_res() const {
    assert(m_gpu_scene && "Scene is not ready");
    return m_gpu_scene->res();
}

const Render::GpuScene::Res& Scene::GetGpuSceneRes() const {
    return gpu_scene_res();
}

const CpuScene& Scene::cpu_scene() const {
    assert(m_cpu_scene && "Scene is not ready");
    return *m_cpu_scene;
}

const CpuScene& Scene::GetCpuScene() const {
    return cpu_scene();
}

void Scene::RestoreDrawCommands(Render::CommandList& cmd_list) {
    assert(m_gpu_scene && "Scene is not ready");
    m_gpu_scene->RestoreDrawCommands(cmd_list);
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
    auto entity = r().view<ecs::CTagMainCamera>().front();
    if (entity == entt::null) {
        LOG_ERROR("No main camera found in scene");
    }
    return entity;
}

entt::entity Scene::GetMainDirectionalLightEntity() const {
    // 先尝试找带 MainTag 的
    auto entity = r().view<ecs::CLightDirectional, ecs::CTagMainLight>().front();
    if (entity == entt::null) {
        // 如果没找到，忽略 MainTag，继续找
        entity = r().view<ecs::CLightDirectional>().front();
        if (entity == entt::null) {
            LOG_ERROR("No directional light found in scene");
        }
    }
    return entity;
}

entt::entity Scene::GetMainPointLightEntity() const {
    // 先尝试找带 MainTag 的
    auto entity = r().view<ecs::CLightPoint, ecs::CTagMainLight>().front();
    if (entity == entt::null) {
        // 如果没找到，忽略 MainTag，继续找
        entity = r().view<ecs::CLightPoint>().front();
        if (entity == entt::null) {
            LOG_ERROR("No point light found in scene");
        }
    }
    return entity;
}

ecs::CCamera& Scene::GetMainCamera() {
    auto entity = GetMainCameraEntity();
    if (entity == entt::null || !r().valid(entity) || !r().all_of<ecs::CCamera>(entity)) {
        LOG_ERROR("Invalid main camera entity or missing CCamera component");
        assert(false && "Invalid main camera entity");
    }
    return r().get<ecs::CCamera>(entity);
}

const ecs::CLightDirectional& Scene::GetMainDirectionalLight() const {
    auto entity = GetMainDirectionalLightEntity();
    if (entity == entt::null || !r().valid(entity) || !r().all_of<ecs::CLightDirectional>(entity)) {
        LOG_ERROR("Invalid main directional light entity or missing CLightDirectional component");
        assert(false && "Invalid main directional light entity");
    }
    return r().get<ecs::CLightDirectional>(entity);
}

const ecs::CLightPoint& Scene::GetMainPointLight() const {
    auto entity = GetMainPointLightEntity();
    if (entity == entt::null || !r().valid(entity) || !r().all_of<ecs::CLightPoint>(entity)) {
        LOG_ERROR("Invalid main point light entity or missing CLightPoint component");
        assert(false && "Invalid main point light entity");
    }
    return r().get<ecs::CLightPoint>(entity);
}

const ecs::CTransform& Scene::GetTransform(entt::entity entity) const {
    if (entity == entt::null || !r().valid(entity) || !r().all_of<ecs::CTransform>(entity)) {
        LOG_ERROR("Invalid entity or missing CTransform component");
        assert(false && "Invalid entity for GetTransform");
    }
    return r().get<ecs::CTransform>(entity);
}

} // namespace Moer