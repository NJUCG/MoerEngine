#include "Scene.h"

#include "log/LogSystem.h"
#include "scene/testcase/SceneTestCaseRunner.h"

namespace Moer {

namespace {

// 在 scene sync 完成后销毁所有 pending destroy light entity
void FinalizeDestroyedLights(Scene& scene) {
    auto& registry = scene.r();

    Array<entt::entity> destroyed_lights;
    auto view = registry.view<const ecs::CTagNeedDestroyLight, const ecs::CLightPoint, ecs::CNode>();
    destroyed_lights.reserve(view.size_hint());
    for (const auto entity : view) {
        destroyed_lights.push_back(entity);
    }

    for (const entt::entity entity : destroyed_lights) {
        if (!registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
            continue;
        }

        auto& node = registry.get<ecs::CNode>(entity);
        if (node.first_child_entt != entt::null || node.child_count != 0) {
            LOG_ERROR("Cannot finalize destroyed point light because it still has children.");
            registry.remove<ecs::CTagNeedDestroyLight>(entity);
            continue;
        }

        scene.logical_scene().UDetachNodeFromParent(entity, node);
        registry.destroy(entity);
    }
}

} // namespace

/////////////////////////
// MARK: 场景 Tick / 同步 API
/////////////////////////

const Scene::TickState& Scene::Tick(bool is_run_test_case) {
    assert(m_logical_scene && m_cpu_scene && m_gpu_scene && "Scene is not ready");

    if (is_run_test_case) {
        SceneTestCaseRunner::Get().PreTick(*this);
    }

    m_last_tick_state = BuildPendingTickState();
    if (m_last_tick_state) {
        m_logical_scene->Update();

        m_cpu_scene->Update();

        m_gpu_scene->Update(
            *m_logical_scene,
            *m_cpu_scene,
            m_last_tick_state.rebuilt_mesh,
            m_last_tick_state.rebuilt_rt_blas
        );

        FinalizeDestroyedLights(*this);
    }

    if (is_run_test_case) {
        SceneTestCaseRunner::Get().PostTick(*this, m_last_tick_state);
    }

    return m_last_tick_state;
}

const Scene::TickState& Scene::GetLastTickState() const {
    return m_last_tick_state;
}

Render::GpuScene::PendingCommandList&& Scene::PopPendingCommandList() {
    return std::move(m_gpu_scene->PopPendingCommandList());
}

bool Scene::HasPendingGpuSceneCommands() const {
    return m_has_pending_gpu_scene_commands;
}

void Scene::ConsumePendingGpuSceneCommands() {
    m_has_pending_gpu_scene_commands = false;
}

Scene::TickState Scene::BuildPendingTickState() const {
    TickState state{};
    if (!m_logical_scene || !m_cpu_scene || !m_gpu_scene) {
        return state;
    }

    const auto& registry    = r();
    state.updated_light     = !registry.view<const ecs::CTagNeedUpdateLight>().empty();
    state.updated_material  = !registry.view<const ecs::CTagNeedUpdateMaterial>().empty();
    state.updated_transform = !registry.view<const ecs::CTagNeedUpdateTransform>().empty();
    state.created_light     = !registry.view<const ecs::CTagNeedCreateLight>().empty();
    state.created_material  = !registry.view<const ecs::CTagNeedCreateMaterial>().empty();
    state.created_transform = !registry.view<const ecs::CTagNeedCreateTransform>().empty();
    state.destroyed_light   = !registry.view<const ecs::CTagNeedDestroyLight>().empty();
    state.rebuilt_mesh      = !registry.view<const ecs::CTagNeedRebuildMesh>().empty();
    state.rebuilt_rt_blas   = !registry.view<const ecs::CTagNeedRebuildRtBlas>().empty();
    state.did_sync          = state.updated_light || state.updated_material || state.updated_transform ||
                              state.created_light || state.created_material || state.created_transform ||
                              state.destroyed_light || state.rebuilt_mesh || state.rebuilt_rt_blas;
    return state;
}

} // namespace Moer
