/**
 * 实现第一批 Scene testcase smoke cases，用来验证 testcase 框架和现有 Scene API
 */
#include "scene/testcase/SceneTestCaseRegistry.h"

#include "log/LogSystem.h"
#include "scene/SceneCreateInfo.h"
#include "scene/LogicalComponents.h"

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

    // 记录初始 light 数量并重置运行阶段
    void Reset(Scene& scene) override {
        m_finished            = false;
        m_failed              = false;
        m_light_entity        = entt::null;
        m_initial_light_count = scene.cpu_scene().GetLightCount();
        m_stage               = Stage::CreateLight;
    }

    // 第一帧创建 point light，第二帧请求删除 point light
    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_stage == Stage::CreateLight) {
            PointLightCreateInfo create_info{};
            create_info.position  = float3(-0.35f, 2.f, 0.f);
            create_info.color     = float3(1.f, 0.8f, 0.2f);
            create_info.intensity = 10000.f;
            create_info.name      = "SceneTestCase CreateDestroyPointLight";
            m_light_entity        = scene.CreatePointLight(create_info);
            m_stage               = Stage::WaitCreateSync;
            return;
        }

        if (m_stage == Stage::DestroyLight) {
            if (!Expect(
                    m_light_entity != entt::null && scene.r().valid(m_light_entity),
                    "Destroy target point light is invalid."
                )) {
                m_stage = Stage::Done;
                Finish();
                return;
            }

            if (!Expect(scene.DestroyPointLight(m_light_entity), "DestroyPointLight request failed.")) {
                m_stage = Stage::Done;
                Finish();
                return;
            }

            m_stage = Stage::WaitDestroySync;
        }
    }

    // 分阶段验证 point light 创建和删除同步结果
    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (m_stage == Stage::WaitCreateSync) {
            auto& registry = scene.r();
            Expect(m_light_entity != entt::null, "Created point light entity should not be entt::null.");
            Expect(registry.valid(m_light_entity), "Created point light entity should be valid.");
            Expect(tick_state.did_sync, "Point light creation should trigger scene sync.");
            Expect(tick_state.created_light, "Point light creation should set created_light TickState.");
            Expect(
                scene.cpu_scene().GetLightCount() == m_initial_light_count + 1,
                "CpuScene light count should increase after point light creation."
            );
            m_stage = Stage::DestroyLight;
            return;
        }

        if (m_stage == Stage::WaitDestroySync) {
            auto& registry = scene.r();
            Expect(tick_state.did_sync, "Point light deletion should trigger scene sync.");
            Expect(tick_state.destroyed_light, "Point light deletion should set destroyed_light TickState.");
            Expect(!registry.valid(m_light_entity), "Destroyed point light entity should be invalid.");
            Expect(
                scene.cpu_scene().GetLightCount() == m_initial_light_count,
                "CpuScene light count should return to the initial baseline."
            );
            m_stage = Stage::Done;
            Finish();
        }
    }

private:
    enum class Stage {
        CreateLight,
        WaitCreateSync,
        DestroyLight,
        WaitDestroySync,
        Done,
    };

    entt::entity m_light_entity        = entt::null;
    uint         m_initial_light_count = 0;
    Stage        m_stage               = Stage::CreateLight;
};

class EntityWithNodeStructuralFlowTestCase final : public SceneTestCaseBase {
public:
    // 返回 EntityWithNode 结构主链路 testcase 的名称
    std::string_view Name() const override {
        return "EntityWithNodeStructuralFlow";
    }

    // 记录 root 基线并重置结构测试状态
    void Reset(Scene& scene) override {
        m_finished                  = false;
        m_failed                    = false;
        m_parent_a                  = entt::null;
        m_parent_b                  = entt::null;
        m_child                     = entt::null;
        m_root                      = FindRootNodeEntity(scene);
        m_initial_root_child_count  = 0;
        m_destroy_child_result      = false;
        m_destroy_parent_a_result   = false;
        m_destroy_parent_b_result   = false;
        m_stage                     = Stage::CreateHierarchy;

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
            if (!Expect(scene.AttachToParent(m_child, m_parent_b), "AttachToParent child -> parent B failed.")) {
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
            if (IsValidNode(registry, m_root) && IsValidNode(registry, m_parent_b) && IsValidNode(registry, m_child)) {
                const auto& root_node     = registry.get<ecs::CNode>(m_root);
                const auto& parent_b_node = registry.get<ecs::CNode>(m_parent_b);
                const auto& child_node    = registry.get<ecs::CNode>(m_child);
                Expect(parent_b_node.child_count == 0, "Parent B child count should decrease after detach.");
                Expect(child_node.parent_entt == m_root, "Child should detach back to root.");
                Expect(child_node.depth == root_node.depth + 1, "Detached child depth should follow root.");
                Expect(root_node.child_count == m_initial_root_child_count + 3, "Root child count after detach is incorrect.");
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
            Expect(tick_state.updated_transform, "DestroyEntity should update transform data for parent chains.");
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

    float3 m_parent_a_position   = float3(1.f, 0.f, 0.f);
    float3 m_parent_b_position   = float3(3.f, 0.f, 0.f);
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
        m_finished             = false;
        m_failed               = false;
        m_parent               = entt::null;
        m_child                = entt::null;
        m_attach_cycle_result  = true;
        m_attach_self_result   = true;
        m_destroy_parent_result = true;
        m_destroy_child_result = false;
        m_destroy_cleanup_result = false;
        m_stage                = Stage::CreateHierarchy;
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
        default:
            return nullptr;
    }
}

} // namespace Moer
