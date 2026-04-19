#include "Scene.h"

#include "Core.h"
#include "loader/LoaderInterface.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "taskgraph/TaskGraph.h"

namespace Moer {

namespace {

bool BuildSceneState(
    const std::filesystem::path&     file_path,
    Render::BindlessArrayRef         bindless_array,
    UniquePtr<ecs::LogicalScene>&    logical_scene,
    UniquePtr<CpuScene>&             cpu_scene,
    UniquePtr<Render::GpuScene>&     gpu_scene
) {
    logical_scene = MakeUnique<ecs::LogicalScene>();
    if (!LoaderInterface::LoadSceneFromFile(*logical_scene, file_path)) {
        logical_scene.reset();
        return false;
    }

    cpu_scene = MakeUnique<CpuScene>(*logical_scene);
    gpu_scene = MakeUnique<Render::GpuScene>(*cpu_scene, std::move(bindless_array));
    return true;
}

} // namespace

Scene::Scene() {
    m_bindless_array = Render::RenderDevice::Get().CreateBindlessArray();
}

void Scene::LoadSceneInternal(const std::filesystem::path& file_path) {
    this->m_scene_load_info.StartLoading();

    UniquePtr<ecs::LogicalScene> logical_scene;
    UniquePtr<CpuScene>          cpu_scene;
    UniquePtr<Render::GpuScene>  gpu_scene;
    if (!BuildSceneState(file_path, this->bindless_array(), logical_scene, cpu_scene, gpu_scene)) {
        this->m_scene_load_info.Reset();
        return;
    }

    this->m_logical_scene = std::move(logical_scene);
    this->m_cpu_scene     = std::move(cpu_scene);
    this->m_gpu_scene     = std::move(gpu_scene);
    this->m_scene_load_info.FinishLoading();
}

void Scene::LoadSceneFromFileAsync(const std::filesystem::path& file_path) {
    this->m_scene_load_info.StartLoading();
    {
        std::lock_guard<std::mutex> lock(m_async_payload_mutex);
        m_pending_async_payload = AsyncLoadPayload{};
    }
    m_has_pending_async_payload.store(false, std::memory_order_release);

    Render::BindlessArrayRef bindless_array = this->bindless_array();
    m_load_event = LambdaTask::Create([this, file_path, bindless_array]() mutable {
                       UniquePtr<ecs::LogicalScene> logical_scene;
                       UniquePtr<CpuScene>          cpu_scene;
                       UniquePtr<Render::GpuScene>  gpu_scene;
                       if (!BuildSceneState(file_path, std::move(bindless_array), logical_scene, cpu_scene, gpu_scene)) {
                           this->m_scene_load_info.Reset();
                           return;
                       }

                       {
                           std::lock_guard<std::mutex> lock(this->m_async_payload_mutex);
                           this->m_pending_async_payload.logical_scene = std::move(logical_scene);
                           this->m_pending_async_payload.cpu_scene     = std::move(cpu_scene);
                           this->m_pending_async_payload.gpu_scene     = std::move(gpu_scene);
                       }
                       this->m_has_pending_async_payload.store(true, std::memory_order_release);
                   }).Dispatch();
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

bool Scene::AdoptPendingAsyncLoad() {
    assert(Moer::IsCurrentlyGameThread());

    if (!m_has_pending_async_payload.load(std::memory_order_acquire)) {
        return false;
    }

    AsyncLoadPayload payload;
    {
        std::lock_guard<std::mutex> lock(m_async_payload_mutex);
        payload = std::move(m_pending_async_payload);
        m_pending_async_payload = AsyncLoadPayload{};
    }
    m_has_pending_async_payload.store(false, std::memory_order_release);

    m_logical_scene = std::move(payload.logical_scene);
    m_cpu_scene     = std::move(payload.cpu_scene);
    m_gpu_scene     = std::move(payload.gpu_scene);
    m_scene_load_info.FinishLoading();
    return true;
}

void Scene::Tick() {
    // TODO
}

void Scene::Reset() {
    if (m_load_event) {
        m_load_event->Wait();
        m_load_event = nullptr;
    }

    m_bindless_array = nullptr;

    m_logical_scene.reset();
    m_cpu_scene.reset();
    m_gpu_scene.reset();

    {
        std::lock_guard<std::mutex> lock(m_async_payload_mutex);
        m_pending_async_payload = AsyncLoadPayload{};
    }
    m_has_pending_async_payload.store(false, std::memory_order_release);

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