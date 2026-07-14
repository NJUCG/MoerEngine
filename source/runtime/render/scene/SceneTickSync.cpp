#include "Scene.h"

#include "log/LogSystem.h"
#include "math/Transform.h"
#include "scene/testcase/SceneTestCaseRunner.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Moer {

namespace {

bool IsFinite(float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Box3D TransformBounds(const Box3D& local_bounds, const float4x4& world_transform) {
    Box3D bounds;
    if (!local_bounds.IsValid() || !IsFinite(local_bounds.min) || !IsFinite(local_bounds.max)) {
        return bounds;
    }

    const float3& min = local_bounds.min;
    const float3& max = local_bounds.max;
    const Transform transform(world_transform);
    bounds.Expand(transform * float3(min.x, min.y, min.z));
    bounds.Expand(transform * float3(max.x, min.y, min.z));
    bounds.Expand(transform * float3(min.x, max.y, min.z));
    bounds.Expand(transform * float3(max.x, max.y, min.z));
    bounds.Expand(transform * float3(min.x, min.y, max.z));
    bounds.Expand(transform * float3(max.x, min.y, max.z));
    bounds.Expand(transform * float3(min.x, max.y, max.z));
    bounds.Expand(transform * float3(max.x, max.y, max.z));
    if (!IsFinite(bounds.min) || !IsFinite(bounds.max)) {
        return Box3D();
    }
    return bounds;
}

uint64 HashTransform(const float4x4& transform) {
    uint64 hash = 14695981039346656037ull;
    for (float component : transform.e) {
        hash ^= static_cast<uint64>(std::bit_cast<uint32>(component));
        hash *= 1099511628211ull;
    }
    return hash;
}

SceneGeometrySnapshot CaptureGeometrySnapshot(const Scene& scene) {
    SceneGeometrySnapshot snapshot;
    const auto& registry = scene.r();

    registry.view<const ecs::CRenderable, const ecs::CNode>().each(
        [&](const auto renderable_entity, const ecs::CRenderable& renderable, const ecs::CNode& node) {
            if (renderable.mesh_entt == entt::null || !registry.valid(renderable.mesh_entt) ||
                !registry.all_of<ecs::CMesh>(renderable.mesh_entt)) {
                ++snapshot.skipped_invalid_count;
                return;
            }

            ++snapshot.renderable_instance_count;
            const auto& mesh = registry.get<const ecs::CMesh>(renderable.mesh_entt);
            const uint leaf_count = mesh.num_leaf_clusters > 0u ?
                                        Min(
                                            mesh.num_leaf_clusters,
                                            static_cast<uint>(mesh.primitive_entts.size())
                                        ) :
                                        static_cast<uint>(mesh.primitive_entts.size());
            snapshot.leaf_primitive_count += leaf_count;

            Box3D instance_bounds;
            for (uint primitive_index = 0u; primitive_index < leaf_count; ++primitive_index) {
                const entt::entity primitive_entity = mesh.primitive_entts[primitive_index];
                if (!registry.valid(primitive_entity) ||
                    !registry.all_of<ecs::CPrimitive>(primitive_entity)) {
                    ++snapshot.skipped_invalid_count;
                    continue;
                }

                const auto& primitive = registry.get<const ecs::CPrimitive>(primitive_entity);
                const Box3D world_bounds = TransformBounds(primitive.aabb, node.d_world_transform);
                if (!world_bounds.IsValid()) {
                    ++snapshot.skipped_invalid_count;
                    continue;
                }

                snapshot.primitive_bounds.push_back(world_bounds);
                instance_bounds.Expand(world_bounds);
            }

            if (instance_bounds.IsValid()) {
                snapshot.instances.push_back(
                    {
                        static_cast<uint64>(entt::to_integral(renderable_entity)),
                        instance_bounds,
                        HashTransform(node.d_world_transform)
                    }
                );
            }
        }
    );

    std::sort(
        snapshot.instances.begin(),
        snapshot.instances.end(),
        [](const SceneGeometryInstanceSnapshot& lhs, const SceneGeometryInstanceSnapshot& rhs) {
            return lhs.key < rhs.key;
        }
    );
    return snapshot;
}

template<typename Light, typename MainLightTag>
entt::entity FindMainLightEntity(const entt::registry& registry) {
    entt::entity entity = registry.view<const Light, const MainLightTag>().front();
    if (entity == entt::null) {
        entity = registry.view<const Light>().front();
    }
    return entity;
}

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

SceneUpdateBatch Scene::PrepareUpdateBatch(bool is_run_test_case, bool capture_geometry_snapshot) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());

    SceneUpdateBatch batch;
    batch.scene_ready = IsReady();
    if (!batch.scene_ready) {
        return batch;
    }

    if (HasPendingGpuSceneCommands()) {
        batch.initial_gpu_commands.emplace(PopPendingCommandList());
        ConsumePendingGpuSceneCommands();
    }

    batch.tick_state = Tick(is_run_test_case);
    if (batch.tick_state || HasPendingGpuSceneCommands()) {
        batch.update_gpu_commands.emplace(PopPendingCommandList());
        ConsumePendingGpuSceneCommands();
    }

    batch.main_camera = GetMainCamera().camera;
    batch.light_count = m_cpu_scene->GetLightCount();

    const auto& registry = r();
    const entt::entity directional_entity =
        FindMainLightEntity<ecs::CLightDirectional, ecs::CTagMainLight>(registry);
    if (directional_entity != entt::null) {
        batch.main_directional_light = registry.get<const ecs::CLightDirectional>(directional_entity);
    }

    const entt::entity point_entity =
        FindMainLightEntity<ecs::CLightPoint, ecs::CTagMainLight>(registry);
    if (point_entity != entt::null) {
        batch.main_point_light = registry.get<const ecs::CLightPoint>(point_entity);
    }

    const bool geometry_changed = batch.tick_state.updated_transform ||
                                  batch.tick_state.created_transform || batch.tick_state.rebuilt_mesh ||
                                  batch.tick_state.rebuilt_rt_blas;
    if (capture_geometry_snapshot || geometry_changed) {
        batch.geometry = CaptureGeometrySnapshot(*this);
    }

    static bool s_logged_batch_boundary = false;
    if (!s_logged_batch_boundary) {
        const size_t geometry_instance_count = batch.geometry ? batch.geometry->instances.size() : 0u;
        LOG_INFO(
            "[Threading] SceneUpdateBatch prepared on {} Thread; initial_commands={}, geometry_instances={}.",
            IsCurrentlyGameThread() ? "Game" : "Render",
            batch.initial_gpu_commands.has_value() ? 1 : 0,
            geometry_instance_count
        );
        s_logged_batch_boundary = true;
    }

    return batch;
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
