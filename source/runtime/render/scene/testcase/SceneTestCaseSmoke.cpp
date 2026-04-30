/**
 * 实现第一批 Scene testcase smoke cases，用来验证 testcase 框架和现有 Scene API
 */
#include "scene/testcase/SceneTestCaseRegistry.h"
#include "scene/testcase/SceneTestCaseRunner.h"

#include "log/LogSystem.h"
#include "math/Transform.h"
#include "scene/LogicalComponents.h"
#include "scene/SceneCreateInfo.h"

#include <cmath>

namespace Moer {

namespace {

// 比较两个 float 是否足够接近
bool IsNear(float lhs, float rhs, float epsilon = 1e-4f) {
    return std::abs(lhs - rhs) <= epsilon;
}

// 比较两个 float3 是否足够接近
bool IsNear(const float3& lhs, const float3& rhs, float epsilon = 1e-4f) {
    return IsNear(lhs.x, rhs.x, epsilon) && IsNear(lhs.y, rhs.y, epsilon) && IsNear(lhs.z, rhs.z, epsilon);
}

// 从 node 的 world transform 中读取世界坐标
float3 GetWorldTranslation(const ecs::CNode& node) {
    return float3(node.d_world_transform.GetColumn(3));
}

// 查找当前 scene 的 root node entity
entt::entity FindRootNodeEntity(Scene& scene) {
    auto view = scene.r().view<ecs::CTagRootNode>();
    auto it   = view.begin();
    if (it == view.end()) {
        return entt::null;
    }
    return *it;
}

struct CreateDestroyPointLightPersistentState {
    entt::entity light_entity        = entt::null;
    uint         initial_light_count = 0;
};

struct CreateDestroyRenderablePersistentState {
    Array<entt::entity> clone_roots;
    Array<entt::entity> clone_renderables;
    Array<uint>         initial_instance_counts;
    Array<uint>         expected_instance_increments;
};

CreateDestroyPointLightPersistentState s_create_destroy_point_light_state{};
CreateDestroyRenderablePersistentState s_create_destroy_renderable_state{};

bool HasValidStoredPointLight(Scene& scene) {
    auto&      registry = scene.r();
    const auto light    = s_create_destroy_point_light_state.light_entity;
    return light != entt::null && registry.valid(light) &&
           registry.all_of<ecs::CLightPoint, ecs::CNode>(light);
}

bool HasValidStoredRenderableClones(Scene& scene) {
    auto& registry = scene.r();
    if (s_create_destroy_renderable_state.clone_roots.empty() ||
        s_create_destroy_renderable_state.clone_renderables.empty()) {
        return false;
    }

    for (const entt::entity entity : s_create_destroy_renderable_state.clone_roots) {
        if (entity == entt::null || !registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
            return false;
        }
    }

    for (const entt::entity entity : s_create_destroy_renderable_state.clone_renderables) {
        if (entity == entt::null || !registry.valid(entity) ||
            !registry.all_of<ecs::CRenderable, ecs::CNode>(entity)) {
            return false;
        }
    }
    return true;
}

Array<float3> BuildRenderableCloneRootOffsets(bool stress_create_enabled, const float3& single_clone_offset) {
    if (!stress_create_enabled) {
        return {single_clone_offset};
    }

    Array<float3> offsets;
    offsets.reserve(24);
    for (int grid_z = -2; grid_z <= 2; ++grid_z) {
        for (int grid_x = -2; grid_x <= 2; ++grid_x) {
            if (grid_x == 0 && grid_z == 0) {
                continue;
            }
            offsets.push_back(float3(float(grid_x) * 30.f, 0.f, float(grid_z) * 20.f));
        }
    }
    return offsets;
}

class SceneTestCaseBase : public ISceneTestCase {
public:
    // 返回 testcase 是否已经完成
    bool IsFinished() const override {
        return m_finished;
    }

protected:
    // 检查条件并在失败时记录错误
    bool Expect(bool condition, std::string_view message) {
        if (condition) {
            return true;
        }

        m_failed = true;
        LOG_ERROR("SceneTestCase '{}' failed: {}", Name(), message);
        return false;
    }

    // 标记 testcase 完成，并在成功时输出通过日志
    void Finish() {
        if (!m_failed) {
            LOG_INFO("SceneTestCase '{}' passed.", Name());
        }
        m_finished = true;
    }

    bool m_finished = false;
    bool m_failed   = false;
};

class FrameworkNoopTestCase final : public SceneTestCaseBase {
public:
    // 返回空运行验证 testcase 的名称
    std::string_view Name() const override {
        return "FrameworkNoop";
    }

    // 清空完成和失败状态
    void Reset(Scene&) override {
        m_finished = false;
        m_failed   = false;
    }

    // 保持空操作，用来验证 scene.Tick(true) 没有额外副作用
    void PreTick(Scene&, const SceneTestCaseContext&) override {}

    // 验证空操作不会产生 scene sync
    void PostTick(Scene&, const Scene::TickState& tick_state) override {
        Expect(!tick_state.did_sync, "FrameworkNoop should not create scene sync work.");
        Finish();
    }
};

class CreatePointLightOnceTestCase final : public SceneTestCaseBase {
public:
    // 返回单帧 point light 创建 testcase 的名称
    std::string_view Name() const override {
        return "CreatePointLightOnce";
    }

    // 清理保存的 light entity 和运行状态
    void Reset(Scene&) override {
        m_finished     = false;
        m_failed       = false;
        m_light_entity = entt::null;
        m_created      = false;
    }

    // 创建 point light，让本帧 Tick 采样 create tag
    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_created) {
            return;
        }

        PointLightCreateInfo create_info{};
        create_info.position  = float3(0.f, 2.f, 0.f);
        create_info.color     = float3(1.f, 0.35f, 0.05f);
        create_info.intensity = 10000.f;
        create_info.name      = "SceneTestCase CreatePointLightOnce";

        m_light_entity = scene.CreatePointLight(create_info);
        m_created      = true;
    }

    // 验证 point light 创建请求已被本帧 TickState 捕获
    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        auto& registry = scene.r();
        Expect(m_light_entity != entt::null, "CreatePointLight returned entt::null.");
        Expect(registry.valid(m_light_entity), "Created point light entity is invalid.");
        Expect(
            registry.all_of<ecs::CLightPoint, ecs::CNode>(m_light_entity),
            "Created point light is missing CLightPoint or CNode."
        );
        Expect(tick_state.did_sync, "CreatePointLight should trigger scene sync.");
        Expect(tick_state.created_light, "CreatePointLight should set created_light TickState.");
        Expect(tick_state.updated_transform, "CreatePointLight should set updated_transform TickState.");
        Finish();
    }

private:
    entt::entity m_light_entity = entt::null;
    bool         m_created      = false;
};

class PatchCreatedPointLightTransformTestCase final : public SceneTestCaseBase {
public:
    // 返回跨帧 point light transform patch testcase 的名称
    std::string_view Name() const override {
        return "PatchCreatedPointLightTransform";
    }

    // 重置 create 和 patch 两个阶段的状态
    void Reset(Scene&) override {
        m_finished     = false;
        m_failed       = false;
        m_light_entity = entt::null;
        m_stage        = Stage::CreateLight;
    }

    // 第一帧创建 point light，第二帧通过 Scene::Patch 修改 node transform
    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_stage == Stage::CreateLight) {
            PointLightCreateInfo create_info{};
            create_info.position  = m_initial_position;
            create_info.color     = float3(0.2f, 0.65f, 1.f);
            create_info.intensity = 10000.f;
            create_info.name      = "SceneTestCase PatchCreatedPointLightTransform";
            m_light_entity        = scene.CreatePointLight(create_info);
            m_stage               = Stage::WaitCreateSync;
            return;
        }

        if (m_stage == Stage::PatchTransform) {
            if (!Expect(
                    m_light_entity != entt::null && scene.r().valid(m_light_entity),
                    "Patch target light is invalid."
                )) {
                m_stage = Stage::Done;
                Finish();
                return;
            }

            scene.Patch<ecs::CNode>(m_light_entity, [&](ecs::CNode& node) {
                node.translation = m_patched_position;
            });
            m_stage = Stage::WaitPatchSync;
        }
    }

    // 分阶段验证 create sync 和 patch sync
    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (m_stage == Stage::WaitCreateSync) {
            auto& registry = scene.r();
            Expect(m_light_entity != entt::null, "Created point light entity should not be entt::null.");
            Expect(
                registry.valid(m_light_entity),
                "Created point light entity should be valid before patch stage."
            );
            Expect(
                registry.all_of<ecs::CLightPoint, ecs::CNode>(m_light_entity),
                "Created point light should have CLightPoint and CNode before patch stage."
            );
            Expect(tick_state.did_sync, "Point light creation should trigger scene sync.");
            Expect(tick_state.created_light, "Point light creation should set created_light TickState.");
            Expect(
                tick_state.updated_transform, "Point light creation should set updated_transform TickState."
            );
            m_stage = Stage::PatchTransform;
            return;
        }

        if (m_stage == Stage::WaitPatchSync) {
            auto& registry = scene.r();
            Expect(tick_state.did_sync, "Point light transform patch should trigger scene sync.");
            Expect(
                tick_state.updated_transform,
                "Point light transform patch should set updated_transform TickState."
            );
            if (Expect(
                    registry.all_of<ecs::CLightPoint>(m_light_entity), "Patched light is missing CLightPoint."
                )) {
                const auto& point_light = registry.get<ecs::CLightPoint>(m_light_entity);
                Expect(
                    IsNear(point_light.d_position, m_patched_position),
                    "CLightPoint derived position was not updated."
                );
            }
            m_stage = Stage::Done;
            Finish();
        }
    }

    // 返回 create 和 patch 两个阶段是否都完成
    bool IsFinished() const override {
        return m_finished;
    }

private:
    enum class Stage {
        CreateLight,
        WaitCreateSync,
        PatchTransform,
        WaitPatchSync,
        Done,
    };

    entt::entity m_light_entity     = entt::null;
    Stage        m_stage            = Stage::CreateLight;
    float3       m_initial_position = float3(0.35f, 2.f, 0.f);
    float3       m_patched_position = float3(0.35f, 2.75f, 0.45f);
};

class CreateDestroyPointLightTestCase final : public SceneTestCaseBase {
public:
    // 返回 point light 创建删除 testcase 的名称
    std::string_view Name() const override {
        return "CreateDestroyPointLight";
    }

    // 第一次触发只创建 point light，下一次触发再执行删除
    void Reset(Scene& scene) override {
        m_finished         = false;
        m_failed           = false;
        m_light_entity     = entt::null;
        m_waiting_for_sync = false;

        if (HasValidStoredPointLight(scene)) {
            m_run_mode            = ERunMode::Destroy;
            m_light_entity        = s_create_destroy_point_light_state.light_entity;
            m_initial_light_count = s_create_destroy_point_light_state.initial_light_count;
            return;
        }

        s_create_destroy_point_light_state = {};
        m_run_mode                         = ERunMode::Create;
        m_initial_light_count              = scene.cpu_scene().GetLightCount();
    }

    // 单次运行只执行创建或删除其中之一，便于用户观察效果
    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_waiting_for_sync) {
            return;
        }

        if (m_run_mode == ERunMode::Create) {
            PointLightCreateInfo create_info{};
            create_info.position  = float3(-0.35f, 2.f, 0.f);
            create_info.color     = float3(1.f, 0.8f, 0.2f);
            create_info.intensity = 10000.f;
            create_info.name      = "SceneTestCase CreateDestroyPointLight";
            m_light_entity        = scene.CreatePointLight(create_info);
        } else {
            if (!Expect(
                    m_light_entity != entt::null && scene.r().valid(m_light_entity),
                    "Destroy target point light is invalid."
                )) {
                Finish();
                return;
            }

            if (!Expect(scene.DestroyPointLight(m_light_entity), "DestroyPointLight request failed.")) {
                Finish();
                return;
            }
        }

        m_waiting_for_sync = true;
    }

    // 创建阶段验证新增对象，删除阶段验证对象消失并恢复基线
    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (!m_waiting_for_sync) {
            return;
        }

        if (m_run_mode == ERunMode::Create) {
            auto& registry = scene.r();
            Expect(m_light_entity != entt::null, "Created point light entity should not be entt::null.");
            Expect(registry.valid(m_light_entity), "Created point light entity should be valid.");
            Expect(tick_state.did_sync, "Point light creation should trigger scene sync.");
            Expect(tick_state.created_light, "Point light creation should set created_light TickState.");
            Expect(
                scene.cpu_scene().GetLightCount() == m_initial_light_count + 1,
                "CpuScene light count should increase after point light creation."
            );
            if (!m_failed) {
                s_create_destroy_point_light_state.light_entity        = m_light_entity;
                s_create_destroy_point_light_state.initial_light_count = m_initial_light_count;
            }
            Finish();
            return;
        }

        auto& registry = scene.r();
        Expect(tick_state.did_sync, "Point light deletion should trigger scene sync.");
        Expect(tick_state.destroyed_light, "Point light deletion should set destroyed_light TickState.");
        Expect(!registry.valid(m_light_entity), "Destroyed point light entity should be invalid.");
        Expect(
            scene.cpu_scene().GetLightCount() == m_initial_light_count,
            "CpuScene light count should return to the initial baseline."
        );
        if (!m_failed) {
            s_create_destroy_point_light_state = {};
        }
        Finish();
    }

private:
    enum class ERunMode {
        Create,
        Destroy,
    };

    entt::entity m_light_entity        = entt::null;
    uint         m_initial_light_count = 0;
    ERunMode     m_run_mode            = ERunMode::Create;
    bool         m_waiting_for_sync    = false;
};

class EntityWithNodeStructuralFlowTestCase final : public SceneTestCaseBase {
public:
    // 返回 EntityWithNode 结构主链路 testcase 的名称
    std::string_view Name() const override {
        return "EntityWithNodeStructuralFlow";
    }

    // 记录 root 基线并重置结构测试状态
    void Reset(Scene& scene) override {
        m_finished                 = false;
        m_failed                   = false;
        m_parent_a                 = entt::null;
        m_parent_b                 = entt::null;
        m_child                    = entt::null;
        m_root                     = FindRootNodeEntity(scene);
        m_initial_root_child_count = 0;
        m_destroy_child_result     = false;
        m_destroy_parent_a_result  = false;
        m_destroy_parent_b_result  = false;
        m_stage                    = Stage::CreateHierarchy;

        if (m_root != entt::null && scene.r().valid(m_root) && scene.r().all_of<ecs::CNode>(m_root)) {
            m_initial_root_child_count = scene.r().get<ecs::CNode>(m_root).child_count;
        }
    }

    // 按阶段创建、重挂、脱离和删除 EntityWithNode
    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_stage == Stage::CreateHierarchy) {
            EntityWithNodeCreateInfo parent_a_info{};
            parent_a_info.name        = "SceneTestCase EntityWithNode ParentA";
            parent_a_info.translation = m_parent_a_position;
            m_parent_a                = scene.CreateEntityWithNode(parent_a_info);

            EntityWithNodeCreateInfo parent_b_info{};
            parent_b_info.name        = "SceneTestCase EntityWithNode ParentB";
            parent_b_info.translation = m_parent_b_position;
            m_parent_b                = scene.CreateEntityWithNode(parent_b_info);

            EntityWithNodeCreateInfo child_info{};
            child_info.parent_node_entt = m_parent_a;
            child_info.name             = "SceneTestCase EntityWithNode Child";
            child_info.translation      = m_child_local_position;
            m_child                     = scene.CreateEntityWithNode(child_info);

            m_stage = Stage::WaitCreateSync;
            return;
        }

        if (m_stage == Stage::AttachToOtherParent) {
            if (!Expect(
                    scene.AttachToParent(m_child, m_parent_b), "AttachToParent child -> parent B failed."
                )) {
                Finish();
                return;
            }
            m_stage = Stage::WaitAttachSync;
            return;
        }

        if (m_stage == Stage::DetachToRoot) {
            if (!Expect(scene.DetachFromParent(m_child), "DetachFromParent child -> root failed.")) {
                Finish();
                return;
            }
            m_stage = Stage::WaitDetachSync;
            return;
        }

        if (m_stage == Stage::DestroyEntities) {
            m_destroy_child_result    = scene.DestroyEntity(m_child);
            m_destroy_parent_a_result = scene.DestroyEntity(m_parent_a);
            m_destroy_parent_b_result = scene.DestroyEntity(m_parent_b);
            m_stage                   = Stage::WaitDestroySync;
        }
    }

    // 验证每个结构性操作后的树关系和 derived transform
    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (m_stage == Stage::WaitCreateSync) {
            auto& registry = scene.r();
            Expect(tick_state.did_sync, "EntityWithNode creation should trigger scene sync.");
            Expect(tick_state.updated_transform, "EntityWithNode creation should update transform data.");
            Expect(IsValidNode(registry, m_parent_a), "Parent A should be a valid EntityWithNode.");
            Expect(IsValidNode(registry, m_parent_b), "Parent B should be a valid EntityWithNode.");
            Expect(IsValidNode(registry, m_child), "Child should be a valid EntityWithNode.");

            if (IsValidNode(registry, m_parent_a) && IsValidNode(registry, m_child)) {
                const auto& parent_a_node = registry.get<ecs::CNode>(m_parent_a);
                const auto& child_node    = registry.get<ecs::CNode>(m_child);
                Expect(parent_a_node.child_count == 1, "Parent A should have exactly one child.");
                Expect(child_node.parent_entt == m_parent_a, "Child should initially attach to parent A.");
                Expect(
                    IsNear(GetWorldTranslation(child_node), m_parent_a_position + m_child_local_position),
                    "Child world position after creation is incorrect."
                );
            }

            m_stage = Stage::AttachToOtherParent;
            return;
        }

        if (m_stage == Stage::WaitAttachSync) {
            auto& registry = scene.r();
            Expect(tick_state.did_sync, "AttachToParent should trigger scene sync.");
            Expect(tick_state.updated_transform, "AttachToParent should update transform data.");
            if (IsValidNode(registry, m_parent_a) && IsValidNode(registry, m_parent_b) &&
                IsValidNode(registry, m_child)) {
                const auto& parent_a_node = registry.get<ecs::CNode>(m_parent_a);
                const auto& parent_b_node = registry.get<ecs::CNode>(m_parent_b);
                const auto& child_node    = registry.get<ecs::CNode>(m_child);
                Expect(parent_a_node.child_count == 0, "Parent A child count should decrease after attach.");
                Expect(parent_b_node.child_count == 1, "Parent B child count should increase after attach.");
                Expect(child_node.parent_entt == m_parent_b, "Child should attach to parent B.");
                Expect(child_node.depth == parent_b_node.depth + 1, "Child depth should follow parent B.");
                Expect(
                    IsNear(GetWorldTranslation(child_node), m_parent_b_position + m_child_local_position),
                    "Child world position after attach is incorrect."
                );
            }

            m_stage = Stage::DetachToRoot;
            return;
        }

        if (m_stage == Stage::WaitDetachSync) {
            auto& registry = scene.r();
            Expect(tick_state.did_sync, "DetachFromParent should trigger scene sync.");
            Expect(tick_state.updated_transform, "DetachFromParent should update transform data.");
            if (IsValidNode(registry, m_root) && IsValidNode(registry, m_parent_b) &&
                IsValidNode(registry, m_child)) {
                const auto& root_node     = registry.get<ecs::CNode>(m_root);
                const auto& parent_b_node = registry.get<ecs::CNode>(m_parent_b);
                const auto& child_node    = registry.get<ecs::CNode>(m_child);
                Expect(parent_b_node.child_count == 0, "Parent B child count should decrease after detach.");
                Expect(child_node.parent_entt == m_root, "Child should detach back to root.");
                Expect(child_node.depth == root_node.depth + 1, "Detached child depth should follow root.");
                Expect(
                    root_node.child_count == m_initial_root_child_count + 3,
                    "Root child count after detach is incorrect."
                );
                Expect(
                    IsNear(GetWorldTranslation(child_node), m_child_local_position),
                    "Child world position after detach is incorrect."
                );
            }

            m_stage = Stage::DestroyEntities;
            return;
        }

        if (m_stage == Stage::WaitDestroySync) {
            auto& registry = scene.r();
            Expect(m_destroy_child_result, "DestroyEntity child should succeed.");
            Expect(m_destroy_parent_a_result, "DestroyEntity parent A should succeed.");
            Expect(m_destroy_parent_b_result, "DestroyEntity parent B should succeed.");
            Expect(tick_state.did_sync, "DestroyEntity should trigger scene sync for detached parents.");
            Expect(
                tick_state.updated_transform, "DestroyEntity should update transform data for parent chains."
            );
            Expect(!registry.valid(m_child), "Destroyed child should be invalid.");
            Expect(!registry.valid(m_parent_a), "Destroyed parent A should be invalid.");
            Expect(!registry.valid(m_parent_b), "Destroyed parent B should be invalid.");
            if (IsValidNode(registry, m_root)) {
                Expect(
                    registry.get<ecs::CNode>(m_root).child_count == m_initial_root_child_count,
                    "Root child count should return to baseline after cleanup."
                );
            }
            Finish();
        }
    }

private:
    enum class Stage {
        CreateHierarchy,
        WaitCreateSync,
        AttachToOtherParent,
        WaitAttachSync,
        DetachToRoot,
        WaitDetachSync,
        DestroyEntities,
        WaitDestroySync,
    };

    // 判断 entity 当前是否是有效 CNode
    bool IsValidNode(entt::registry& registry, entt::entity entity) const {
        return entity != entt::null && registry.valid(entity) && registry.all_of<ecs::CNode>(entity);
    }

    entt::entity m_parent_a = entt::null;
    entt::entity m_parent_b = entt::null;
    entt::entity m_child    = entt::null;
    entt::entity m_root     = entt::null;

    uint  m_initial_root_child_count = 0;
    bool  m_destroy_child_result     = false;
    bool  m_destroy_parent_a_result  = false;
    bool  m_destroy_parent_b_result  = false;
    Stage m_stage                    = Stage::CreateHierarchy;

    float3 m_parent_a_position    = float3(1.f, 0.f, 0.f);
    float3 m_parent_b_position    = float3(3.f, 0.f, 0.f);
    float3 m_child_local_position = float3(0.f, 2.f, 0.f);
};

class EntityWithNodeRejectInvalidOpsTestCase final : public SceneTestCaseBase {
public:
    // 返回 EntityWithNode 非法操作 testcase 的名称
    std::string_view Name() const override {
        return "EntityWithNodeRejectInvalidOps";
    }

    // 重置非法操作 testcase 状态
    void Reset(Scene&) override {
        m_finished               = false;
        m_failed                 = false;
        m_parent                 = entt::null;
        m_child                  = entt::null;
        m_attach_cycle_result    = true;
        m_attach_self_result     = true;
        m_destroy_parent_result  = true;
        m_destroy_child_result   = false;
        m_destroy_cleanup_result = false;
        m_stage                  = Stage::CreateHierarchy;
    }

    // 创建父子节点、执行非法操作，并在最后清理测试 entity
    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_stage == Stage::CreateHierarchy) {
            EntityWithNodeCreateInfo parent_info{};
            parent_info.name        = "SceneTestCase Invalid Parent";
            parent_info.translation = float3(-1.f, 0.f, 0.f);
            m_parent                = scene.CreateEntityWithNode(parent_info);

            EntityWithNodeCreateInfo child_info{};
            child_info.parent_node_entt = m_parent;
            child_info.name             = "SceneTestCase Invalid Child";
            child_info.translation      = float3(0.f, 1.f, 0.f);
            m_child                     = scene.CreateEntityWithNode(child_info);

            m_stage = Stage::WaitCreateSync;
            return;
        }

        if (m_stage == Stage::RunInvalidOps) {
            m_attach_cycle_result   = scene.AttachToParent(m_parent, m_child);
            m_attach_self_result    = scene.AttachToParent(m_child, m_child);
            m_destroy_parent_result = scene.DestroyEntity(m_parent);
            m_stage                 = Stage::WaitInvalidResult;
            return;
        }

        if (m_stage == Stage::Cleanup) {
            m_destroy_child_result   = scene.DestroyEntity(m_child);
            m_destroy_cleanup_result = scene.DestroyEntity(m_parent);
            m_stage                  = Stage::WaitCleanupSync;
        }
    }

    // 验证非法操作未破坏树结构，并清理测试 entity
    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (m_stage == Stage::WaitCreateSync) {
            Expect(tick_state.did_sync, "Invalid ops setup should trigger scene sync.");
            Expect(tick_state.updated_transform, "Invalid ops setup should update transform data.");
            m_stage = Stage::RunInvalidOps;
            return;
        }

        if (m_stage == Stage::WaitInvalidResult) {
            auto& registry = scene.r();
            Expect(!m_attach_cycle_result, "AttachToParent should reject cycle attach.");
            Expect(!m_attach_self_result, "AttachToParent should reject self attach.");
            Expect(!m_destroy_parent_result, "DestroyEntity should reject non-leaf node.");
            Expect(!tick_state.did_sync, "Rejected structural operations should not trigger scene sync.");
            if (IsValidNode(registry, m_parent) && IsValidNode(registry, m_child)) {
                const auto& parent_node = registry.get<ecs::CNode>(m_parent);
                const auto& child_node  = registry.get<ecs::CNode>(m_child);
                Expect(parent_node.child_count == 1, "Parent child count should remain unchanged.");
                Expect(child_node.parent_entt == m_parent, "Child parent should remain unchanged.");
            }
            m_stage = Stage::Cleanup;
            return;
        }

        if (m_stage == Stage::WaitCleanupSync) {
            auto& registry = scene.r();
            Expect(m_destroy_child_result, "Cleanup DestroyEntity child should succeed.");
            Expect(m_destroy_cleanup_result, "Cleanup DestroyEntity parent should succeed.");
            Expect(tick_state.did_sync, "Cleanup should trigger scene sync.");
            Expect(!registry.valid(m_child), "Cleanup child should be invalid.");
            Expect(!registry.valid(m_parent), "Cleanup parent should be invalid.");
            Finish();
        }
    }

private:
    enum class Stage {
        CreateHierarchy,
        WaitCreateSync,
        RunInvalidOps,
        WaitInvalidResult,
        Cleanup,
        WaitCleanupSync,
    };

    // 判断 entity 当前是否是有效 CNode
    bool IsValidNode(entt::registry& registry, entt::entity entity) const {
        return entity != entt::null && registry.valid(entity) && registry.all_of<ecs::CNode>(entity);
    }

    entt::entity m_parent = entt::null;
    entt::entity m_child  = entt::null;

    bool  m_attach_cycle_result    = true;
    bool  m_attach_self_result     = true;
    bool  m_destroy_parent_result  = true;
    bool  m_destroy_child_result   = false;
    bool  m_destroy_cleanup_result = false;
    Stage m_stage                  = Stage::CreateHierarchy;
};

class CreateDestroyRenderableTestCase final : public SceneTestCaseBase {
public:
    // 返回 renderable 创建删除 testcase 的名称
    std::string_view Name() const override {
        return "CreateDestroyRenderable";
    }

    // 第一次触发只创建 renderable 副本，下一次触发再执行删除
    void Reset(Scene& scene) override {
        m_finished = false;
        m_failed   = false;

        m_waiting_for_sync = false;

        m_clone_roots.clear();
        m_source_renderables.clear();
        m_clone_renderables.clear();
        m_destroy_clone_results.clear();
        m_destroy_root_results.clear();
        m_clone_root_offsets.clear();

        if (HasValidStoredRenderableClones(scene)) {
            m_run_mode                     = ERunMode::Destroy;
            m_clone_roots                  = s_create_destroy_renderable_state.clone_roots;
            m_clone_renderables            = s_create_destroy_renderable_state.clone_renderables;
            m_initial_instance_counts      = s_create_destroy_renderable_state.initial_instance_counts;
            m_expected_instance_increments = s_create_destroy_renderable_state.expected_instance_increments;
            return;
        }

        s_create_destroy_renderable_state = {};
        m_run_mode                        = ERunMode::Create;
        m_clone_root_offsets              = BuildRenderableCloneRootOffsets(
            SceneTestCaseRunner::Get().IsCreateDestroyRenderableStressEnabled(), m_single_clone_root_offset
        );

        const auto& cpu_scene = scene.cpu_scene();
        m_initial_instance_counts.clear();
        m_expected_instance_increments.clear();
        m_initial_instance_counts.resize(cpu_scene.GetPrimitiveCount(), 0);
        m_expected_instance_increments.resize(cpu_scene.GetPrimitiveCount(), 0);

        for (uint primitive_id = 0; primitive_id < cpu_scene.GetPrimitiveCount(); ++primitive_id) {
            m_initial_instance_counts[primitive_id] = cpu_scene.GetInstanceCountForPrimitive(primitive_id);
        }

        auto& registry = scene.r();
        auto  view     = registry.view<ecs::CRenderable, ecs::CNode>();
        for (const entt::entity entity : view) {
            const auto& renderable = registry.get<ecs::CRenderable>(entity);
            if (renderable.mesh_entt == entt::null || !registry.valid(renderable.mesh_entt) ||
                !registry.all_of<ecs::CMesh>(renderable.mesh_entt)) {
                continue;
            }

            m_source_renderables.push_back(entity);

            const auto& mesh = registry.get<ecs::CMesh>(renderable.mesh_entt);
            for (const entt::entity primitive_entt : mesh.primitive_entts) {
                const uint primitive_id = cpu_scene.GetPrimitiveId(primitive_entt);
                if (primitive_id == UINT_MAX || primitive_id >= m_expected_instance_increments.size()) {
                    continue;
                }
                m_expected_instance_increments[primitive_id] += 1;
            }
        }
    }

    // 单次运行只执行创建或删除其中之一，便于用户观察 clone 场景
    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_waiting_for_sync) {
            return;
        }

        if (m_run_mode == ERunMode::Create) {
            if (!Expect(
                    !m_source_renderables.empty(), "Scene should contain at least one source renderable."
                )) {
                Finish();
                return;
            }

            m_clone_roots.clear();
            m_clone_roots.reserve(m_clone_root_offsets.size());
            m_clone_renderables.clear();
            m_clone_renderables.reserve(m_source_renderables.size() * m_clone_root_offsets.size());

            auto& registry = scene.r();
            for (const float3& clone_root_offset : m_clone_root_offsets) {
                EntityWithNodeCreateInfo clone_root_info{};
                clone_root_info.name          = "SceneTestCase Renderable Clone Root";
                clone_root_info.translation   = clone_root_offset;
                const entt::entity clone_root = scene.CreateEntityWithNode(clone_root_info);
                m_clone_roots.push_back(clone_root);

                for (const entt::entity source_entity : m_source_renderables) {
                    if (!Expect(
                            registry.valid(source_entity) &&
                                registry.all_of<ecs::CRenderable, ecs::CNode>(source_entity),
                            "Source renderable became invalid before clone creation."
                        )) {
                        Finish();
                        return;
                    }

                    const auto& source_node       = registry.get<ecs::CNode>(source_entity);
                    const auto& source_renderable = registry.get<ecs::CRenderable>(source_entity);
                    const auto  affine = Transform(source_node.d_world_transform).AffineDecomposition();

                    RenderableCreateInfo create_info{};
                    create_info.mesh_entt        = source_renderable.mesh_entt;
                    create_info.parent_node_entt = clone_root;
                    create_info.name             = "SceneTestCase Renderable Clone";
                    create_info.translation      = affine.translation;
                    create_info.rotation         = affine.quaternion;
                    create_info.scale            = affine.scaling;

                    m_clone_renderables.push_back(scene.CreateRenderableWithNode(create_info));
                }
            }
        } else {
            m_destroy_clone_results.clear();
            m_destroy_clone_results.reserve(m_clone_renderables.size());
            for (const entt::entity clone_entity : m_clone_renderables) {
                m_destroy_clone_results.push_back(scene.DestroyRenderable(clone_entity));
            }

            m_destroy_root_results.clear();
            m_destroy_root_results.reserve(m_clone_roots.size());
            for (const entt::entity clone_root : m_clone_roots) {
                m_destroy_root_results.push_back(scene.DestroyEntity(clone_root));
            }
        }

        m_waiting_for_sync = true;
    }

    // 创建阶段验证 clone 和 instance 增量，删除阶段验证恢复到基线
    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (!m_waiting_for_sync) {
            return;
        }

        if (m_run_mode == ERunMode::Create) {
            auto& registry = scene.r();
            Expect(tick_state.did_sync, "Renderable clone creation should trigger scene sync.");
            Expect(tick_state.rebuilt_mesh, "Renderable clone creation should rebuild mesh instance cache.");
            Expect(
                m_clone_roots.size() == m_clone_root_offsets.size(),
                "Clone root count should match the configured clone batch count."
            );
            Expect(
                m_clone_renderables.size() == m_source_renderables.size() * m_clone_root_offsets.size(),
                "Clone renderable count should match source renderables times batch count."
            );

            for (size_t root_index = 0;
                 root_index < m_clone_roots.size() && root_index < m_clone_root_offsets.size();
                 ++root_index) {
                const entt::entity clone_root = m_clone_roots[root_index];
                if (!Expect(
                        clone_root != entt::null && registry.valid(clone_root) &&
                            registry.all_of<ecs::CNode>(clone_root),
                        "Every clone root should be a valid EntityWithNode."
                    )) {
                    continue;
                }

                const auto& clone_root_node = registry.get<ecs::CNode>(clone_root);
                Expect(
                    IsNear(GetWorldTranslation(clone_root_node), m_clone_root_offsets[root_index]),
                    "Clone root world position should match the configured batch offset."
                );

                for (size_t source_index = 0; source_index < m_source_renderables.size(); ++source_index) {
                    const entt::entity source_entity = m_source_renderables[source_index];
                    const size_t       clone_index = root_index * m_source_renderables.size() + source_index;
                    if (clone_index >= m_clone_renderables.size()) {
                        Expect(false, "Clone renderable index is out of range.");
                        continue;
                    }

                    const entt::entity clone_entity = m_clone_renderables[clone_index];
                    if (!Expect(
                            registry.valid(clone_entity) &&
                                registry.all_of<ecs::CRenderable, ecs::CNode>(clone_entity),
                            "Clone renderable should be valid after scene sync."
                        )) {
                        continue;
                    }

                    const auto& source_node       = registry.get<ecs::CNode>(source_entity);
                    const auto& clone_node        = registry.get<ecs::CNode>(clone_entity);
                    const auto& source_renderable = registry.get<ecs::CRenderable>(source_entity);
                    const auto& clone_renderable  = registry.get<ecs::CRenderable>(clone_entity);

                    Expect(
                        clone_renderable.mesh_entt == source_renderable.mesh_entt,
                        "Clone renderable should reuse the source mesh entity."
                    );
                    Expect(
                        IsNear(
                            GetWorldTranslation(clone_node),
                            GetWorldTranslation(source_node) + m_clone_root_offsets[root_index]
                        ),
                        "Clone renderable world position should equal source world position plus batch "
                        "offset."
                    );
                }
            }

            const auto& cpu_scene = scene.cpu_scene();
            for (uint primitive_id = 0; primitive_id < cpu_scene.GetPrimitiveCount(); ++primitive_id) {
                Expect(
                    cpu_scene.GetInstanceCountForPrimitive(primitive_id) ==
                        m_initial_instance_counts[primitive_id] +
                            m_expected_instance_increments[primitive_id] * m_clone_root_offsets.size(),
                    "Primitive instance count after renderable clone creation is incorrect."
                );
            }

            if (!m_failed) {
                s_create_destroy_renderable_state.clone_roots             = m_clone_roots;
                s_create_destroy_renderable_state.clone_renderables       = m_clone_renderables;
                s_create_destroy_renderable_state.initial_instance_counts = m_initial_instance_counts;
                s_create_destroy_renderable_state.expected_instance_increments =
                    m_expected_instance_increments;
            }
            Finish();
            return;
        }

        auto& registry = scene.r();
        for (const bool destroy_result : m_destroy_clone_results) {
            Expect(destroy_result, "DestroyRenderable should succeed for every clone renderable.");
        }
        for (const bool destroy_result : m_destroy_root_results) {
            Expect(destroy_result, "DestroyEntity should succeed for every clone root.");
        }
        Expect(tick_state.did_sync, "Renderable clone cleanup should trigger scene sync.");
        Expect(tick_state.rebuilt_mesh, "Renderable clone cleanup should rebuild mesh instance cache.");

        for (const entt::entity clone_entity : m_clone_renderables) {
            Expect(!registry.valid(clone_entity), "Destroyed clone renderable should be invalid.");
        }
        for (const entt::entity clone_root : m_clone_roots) {
            Expect(!registry.valid(clone_root), "Destroyed clone root should be invalid.");
        }

        const auto& cpu_scene = scene.cpu_scene();
        for (uint primitive_id = 0; primitive_id < cpu_scene.GetPrimitiveCount(); ++primitive_id) {
            Expect(
                cpu_scene.GetInstanceCountForPrimitive(primitive_id) ==
                    m_initial_instance_counts[primitive_id],
                "Primitive instance count should return to the initial baseline after clone cleanup."
            );
        }

        if (!m_failed) {
            s_create_destroy_renderable_state = {};
        }
        Finish();
    }

private:
    enum class ERunMode {
        Create,
        Destroy,
    };

    Array<entt::entity> m_source_renderables;
    Array<entt::entity> m_clone_roots;
    Array<entt::entity> m_clone_renderables;
    Array<bool>         m_destroy_clone_results;
    Array<bool>         m_destroy_root_results;
    Array<uint>         m_initial_instance_counts;
    Array<uint>         m_expected_instance_increments;
    Array<float3>       m_clone_root_offsets;

    ERunMode m_run_mode         = ERunMode::Create;
    bool     m_waiting_for_sync = false;

    float3 m_single_clone_root_offset = float3(0.f, 0.f, 20.f);
};

} // namespace

// 根据 testcase ID 返回日志可读名称
std::string_view GetSceneTestCaseName(ESceneTestCaseId test_case_id) {
    switch (test_case_id) {
        case ESceneTestCaseId::None:
            return "None";
        case ESceneTestCaseId::FrameworkNoop:
            return "FrameworkNoop";
        case ESceneTestCaseId::CreatePointLightOnce:
            return "CreatePointLightOnce";
        case ESceneTestCaseId::PatchCreatedPointLightTransform:
            return "PatchCreatedPointLightTransform";
        case ESceneTestCaseId::CreateDestroyPointLight:
            return "CreateDestroyPointLight";
        case ESceneTestCaseId::EntityWithNodeStructuralFlow:
            return "EntityWithNodeStructuralFlow";
        case ESceneTestCaseId::EntityWithNodeRejectInvalidOps:
            return "EntityWithNodeRejectInvalidOps";
        case ESceneTestCaseId::CreateDestroyRenderable:
            return "CreateDestroyRenderable";
    }
    return "Unknown";
}

// 根据 testcase ID 创建对应 testcase 实例
UniquePtr<ISceneTestCase> CreateSceneTestCase(ESceneTestCaseId test_case_id) {
    switch (test_case_id) {
        case ESceneTestCaseId::FrameworkNoop:
            return MakeUnique<FrameworkNoopTestCase>();
        case ESceneTestCaseId::CreatePointLightOnce:
            return MakeUnique<CreatePointLightOnceTestCase>();
        case ESceneTestCaseId::PatchCreatedPointLightTransform:
            return MakeUnique<PatchCreatedPointLightTransformTestCase>();
        case ESceneTestCaseId::CreateDestroyPointLight:
            return MakeUnique<CreateDestroyPointLightTestCase>();
        case ESceneTestCaseId::EntityWithNodeStructuralFlow:
            return MakeUnique<EntityWithNodeStructuralFlowTestCase>();
        case ESceneTestCaseId::EntityWithNodeRejectInvalidOps:
            return MakeUnique<EntityWithNodeRejectInvalidOpsTestCase>();
        case ESceneTestCaseId::CreateDestroyRenderable:
            return MakeUnique<CreateDestroyRenderableTestCase>();
        default:
            return nullptr;
    }
}

} // namespace Moer
