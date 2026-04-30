/**
 * 实现第一批 Scene testcase smoke cases，用来验证 testcase 框架和现有 Scene API
 */
#include "scene/testcase/SceneTestCaseRegistry.h"

#include "log/LogSystem.h"
#include "scene/LogicalComponents.h"
#include "scene/SceneLightApi.h"

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
        case ESceneTestCaseId::CreateEntityAddRemoveTransform:
            return "CreateEntityAddRemoveTransform";
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
        default:
            return nullptr;
    }
}

} // namespace Moer
