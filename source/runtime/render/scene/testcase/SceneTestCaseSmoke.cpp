// 集中实现 Scene API 的冒烟用例，并维护用例 ID、名称和实例之间的稳定映射。
#include "scene/testcase/SceneTestCaseRegistry.h"

#include "log/LogSystem.h"
#include "math/Transform.h"
#include "scene/LogicalComponents.h"
#include "scene/SceneCreateInfo.h"

#include <cmath>
#include <filesystem>
#include <iterator>
#include <unordered_set>

namespace Moer {

namespace {

struct SceneTestCaseDescriptor {
    ESceneTestCaseId id;
    std::string_view name;
};

// 顺序同时决定界面展示和 Suite 执行顺序，新增用例时应只在这里追加。
constexpr SceneTestCaseDescriptor kSceneTestCaseDescriptors[] = {
    {ESceneTestCaseId::SuiteSaveStateCache, "SuiteSaveStateCache"},
    {ESceneTestCaseId::FrameworkNoop, "FrameworkNoop"},
    {ESceneTestCaseId::CreatePointLightOnce, "CreatePointLightOnce"},
    {ESceneTestCaseId::PatchCreatedPointLightTransform, "PatchCreatedPointLightTransform"},
    {ESceneTestCaseId::CreateDestroyPointLight, "CreateDestroyPointLight"},
    {ESceneTestCaseId::EntityWithNodeStructuralFlow, "EntityWithNodeStructuralFlow"},
    {ESceneTestCaseId::EntityWithNodeRejectInvalidOps, "EntityWithNodeRejectInvalidOps"},
    {ESceneTestCaseId::CreateDestroyRenderable, "CreateDestroyRenderable"},
    {ESceneTestCaseId::CreateProceduralRenderable, "CreateProceduralRenderable"},
    {ESceneTestCaseId::SetNodeProperties, "SetNodeProperties"},
    {ESceneTestCaseId::QueryNodeAndLocalTransform, "QueryNodeAndLocalTransform"},
    {ESceneTestCaseId::DestroyNodeSubtree, "DestroyNodeSubtree"},
    {ESceneTestCaseId::DebugModifyMaterial, "DebugModifyMaterial"},
    {ESceneTestCaseId::ImportSceneFromFile, "ImportSceneFromFile"},
    {ESceneTestCaseId::SuiteLoadStateCache, "SuiteLoadStateCache"},
};

// 比较两个 float 是否足够接近
bool IsNear(float lhs, float rhs, float epsilon = 1e-4f) {
    return std::abs(lhs - rhs) <= epsilon;
}

// 比较两个 float3 是否足够接近
bool IsNear(const float3& lhs, const float3& rhs, float epsilon = 1e-4f) {
    return IsNear(lhs.x, rhs.x, epsilon) && IsNear(lhs.y, rhs.y, epsilon) && IsNear(lhs.z, rhs.z, epsilon);
}

bool IsNear(const Quaternion& lhs, const Quaternion& rhs, float epsilon = 1e-4f) {
    const float dot =
        lhs.vec.x * rhs.vec.x + lhs.vec.y * rhs.vec.y + lhs.vec.z * rhs.vec.z + lhs.vec.w * rhs.vec.w;
    return std::abs(1.f - std::abs(dot)) <= epsilon;
}

// 从 node 的 world transform 中读取世界坐标
float3 GetWorldTranslation(const ecs::CNode& node) {
    return float3(node.d_world_transform.GetColumn(3));
}

struct DebugMaterialPreset {
    float4           albedo_factor;
    float3           emissive_factor;
    float            roughness_factor;
    float            metallic_factor;
    std::string_view name;
};

DebugMaterialPreset GetDebugMaterialPreset(uint preset_index) {
    switch (preset_index % 4) {
        case 0:
            return {float4(1.f, 0.05f, 0.05f, 1.f), float3(0.2f, 0.f, 0.f), 0.15f, 0.0f, "Red Highlight"};
        case 1:
            return {float4(0.05f, 1.f, 0.05f, 1.f), float3(0.f, 0.2f, 0.f), 0.15f, 0.0f, "Green Highlight"};
        case 2:
            return {float4(0.05f, 0.2f, 1.f, 1.f), float3(0.f, 0.f, 0.2f), 0.15f, 0.0f, "Blue Highlight"};
        default:
            return {float4(1.f, 1.f, 1.f, 1.f), float3(0.f, 0.f, 0.f), 1.0f, 0.0f, "Reset"};
    }
}

Array<entt::entity> FindPrimitiveReferencedMaterialEntities(Scene& scene) {
    auto& registry = scene.r();
    auto  view     = registry.view<const ecs::CPrimitive>();

    Array<entt::entity>              material_entities;
    std::unordered_set<entt::entity> visited_materials;

    for (entt::entity primitive_entity : view) {
        const auto& primitive        = registry.get<ecs::CPrimitive>(primitive_entity);
        const auto  material_entity  = primitive.material_entt;
        const bool  has_valid_target = material_entity != entt::null && registry.valid(material_entity) &&
                                       registry.all_of<ecs::CMaterial>(material_entity);
        if (has_valid_target && visited_materials.emplace(material_entity).second) {
            material_entities.emplace_back(material_entity);
        }
    }

    return material_entities;
}

uint& DebugMaterialPresetCursor() {
    static uint s_debug_material_preset_cursor = 0;
    return s_debug_material_preset_cursor;
}

const std::filesystem::path& GetImportSceneTestFilePath() {
    static const std::filesystem::path s_import_scene_test_file_path =
        std::filesystem::path("asset") / "scenes" / "mizuki" / "mizuki.gltf";
    return s_import_scene_test_file_path;
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

class FrameworkNoopTestCase final : public SceneTestCaseBase {
public:
    std::string_view Name() const override {
        return "FrameworkNoop";
    }

    // 清空完成和失败状态
    void Reset(Scene&) override {
        ResetBaseState();
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
    std::string_view Name() const override {
        return "CreatePointLightOnce";
    }

    // 清理保存的 light entity 和运行状态
    void Reset(Scene&) override {
        ResetBaseState();
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
        Expect(m_light_entity != entt::null, "CreatePointLight should not return entt::null.");
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
    std::string_view Name() const override {
        return "PatchCreatedPointLightTransform";
    }

    // 重置 create 和 patch 两个阶段的状态
    void Reset(Scene&) override {
        ResetBaseState();
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
            Expect(m_light_entity != entt::null, "Created point light entity must not be entt::null.");
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

class DebugModifyMaterialTestCase final : public SceneTestCaseBase {
public:
    std::string_view Name() const override {
        return "DebugModifyMaterial";
    }

    // 清理运行状态
    void Reset(Scene&) override {
        ResetBaseState();
        m_applied        = false;
        m_material_count = 0;
        m_preset_name    = {};
    }

    // 修改场景中 primitive 引用过的材质
    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_applied || m_finished) {
            return;
        }

        const Array<entt::entity> material_entities = FindPrimitiveReferencedMaterialEntities(scene);
        if (material_entities.empty()) {
            Expect(false, "No material referenced by any primitive was found.");
            Finish();
            return;
        }

        const DebugMaterialPreset preset = GetDebugMaterialPreset(DebugMaterialPresetCursor());
        DebugMaterialPresetCursor()++;

        for (entt::entity material_entity : material_entities) {
            scene.Patch<ecs::CMaterial>(material_entity, [&](ecs::CMaterial& material) {
                material.albedo_map_entt  = entt::null;
                material.albedo_factor    = preset.albedo_factor;
                material.emissive_factor  = preset.emissive_factor;
                material.roughness_factor = preset.roughness_factor;
                material.metallic_factor  = preset.metallic_factor;
            });
        }

        m_applied        = true;
        m_material_count = material_entities.size();
        m_preset_name    = preset.name;
    }

    // 验证材质修改产生 Scene sync
    void PostTick(Scene&, const Scene::TickState& tick_state) override {
        if (m_finished) {
            return;
        }

        Expect(tick_state.did_sync, "DebugModifyMaterial should trigger scene sync.");
        Expect(tick_state.updated_material, "DebugModifyMaterial should set updated_material TickState.");
        LOG_INFO(
            "SceneTestCase DebugModifyMaterial applied preset '{}' to {} materials.",
            m_preset_name,
            m_material_count
        );
        Finish();
    }

private:
    bool             m_applied        = false;
    size_t           m_material_count = 0;
    std::string_view m_preset_name;
};

class CreateDestroyPointLightTestCase final : public SceneTestCaseBase {
public:
    std::string_view Name() const override {
        return "CreateDestroyPointLight";
    }

    // 第一次触发只创建 point light，下一次触发再执行删除
    void Reset(Scene& scene) override {
        ResetBaseState();
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
    std::string_view Name() const override {
        return "EntityWithNodeStructuralFlow";
    }

    // 记录 root 基线并重置结构测试状态
    void Reset(Scene& scene) override {
        ResetBaseState();
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
    std::string_view Name() const override {
        return "EntityWithNodeRejectInvalidOps";
    }

    // 重置非法操作 testcase 状态
    void Reset(Scene&) override {
        ResetBaseState();
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
    explicit CreateDestroyRenderableTestCase(bool stress_create_enabled) :
        m_stress_create_enabled(stress_create_enabled) {}

    std::string_view Name() const override {
        return "CreateDestroyRenderable";
    }

    // 第一次触发只创建 renderable 副本，下一次触发再执行删除
    void Reset(Scene& scene) override {
        ResetBaseState();

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
        m_clone_root_offsets =
            BuildRenderableCloneRootOffsets(m_stress_create_enabled, m_single_clone_root_offset);

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
            Expect(tick_state.did_sync, "Creating renderable clones should trigger scene sync.");
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

    ERunMode m_run_mode              = ERunMode::Create;
    bool     m_waiting_for_sync      = false;
    bool     m_stress_create_enabled = false;

    float3 m_single_clone_root_offset = float3(0.f, 0.f, 20.f);
};

class CreateProceduralRenderableTestCase final : public SceneTestCaseBase {
public:
    std::string_view Name() const override {
        return "CreateProceduralRenderable";
    }

    void Reset(Scene& scene) override {
        ResetBaseState();

        m_waiting_for_sync        = false;
        m_result                  = {};
        m_initial_primitive_count = scene.cpu_scene().GetPrimitiveCount();
        m_initial_material_count  = scene.r().view<const ecs::CMaterial>().size();
    }

    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_waiting_for_sync) {
            return;
        }

        ProceduralMeshCreateInfo create_info{};
        create_info.shape       = EProceduralPrimitiveShape::Cube;
        create_info.name        = "SceneTestCase Procedural Cube";
        create_info.translation = float3(0.f, 1.5f, 6.f);
        create_info.scale       = float3(1.5f, 1.5f, 1.5f);

        create_info.material.name             = "SceneTestCase Procedural Material";
        create_info.material.albedo_factor    = float4(0.1f, 0.65f, 1.f, 1.f);
        create_info.material.roughness_factor = 0.35f;
        create_info.material.metallic_factor  = 0.f;

        m_result = scene.CreateProceduralRenderable(create_info);
        if (!Expect(
                static_cast<bool>(m_result), "CreateProceduralRenderable should return a valid result."
            )) {
            Finish();
            return;
        }

        m_waiting_for_sync = true;
    }

    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (!m_waiting_for_sync) {
            return;
        }

        auto& registry = scene.r();
        Expect(tick_state.did_sync, "CreateProceduralRenderable should trigger scene sync.");
        Expect(tick_state.created_material, "CreateProceduralRenderable should create material cache.");
        Expect(tick_state.updated_transform, "CreateProceduralRenderable should update transform cache.");
        Expect(tick_state.rebuilt_mesh, "CreateProceduralRenderable should rebuild mesh resource cache.");

        Expect(
            registry.valid(m_result.material_entt) && registry.all_of<ecs::CMaterial>(m_result.material_entt),
            "Procedural material entity should be valid."
        );
        Expect(
            registry.valid(m_result.primitive_entt) &&
                registry.all_of<ecs::CPrimitive>(m_result.primitive_entt),
            "Procedural primitive entity should be valid."
        );
        Expect(
            registry.valid(m_result.mesh_entt) && registry.all_of<ecs::CMesh>(m_result.mesh_entt),
            "Procedural mesh entity should be valid."
        );
        Expect(
            registry.valid(m_result.renderable_entt) &&
                registry.all_of<ecs::CRenderable, ecs::CNode>(m_result.renderable_entt),
            "Procedural renderable entity should be valid."
        );

        const auto& cpu_scene    = scene.cpu_scene();
        const uint  primitive_id = cpu_scene.GetPrimitiveId(m_result.primitive_entt);
        Expect(primitive_id != UINT_MAX, "Procedural primitive should have a CpuScene primitive id.");
        Expect(
            cpu_scene.GetPrimitiveCount() == m_initial_primitive_count + 1,
            "CpuScene primitive count should increase by one."
        );
        if (primitive_id != UINT_MAX) {
            Expect(
                cpu_scene.GetInstanceCountForPrimitive(primitive_id) == 1,
                "Procedural primitive should have exactly one instance."
            );
        }
        Expect(
            registry.view<const ecs::CMaterial>().size() == m_initial_material_count + 1,
            "Logical material count should increase by one."
        );

        Finish();
    }

private:
    CreateProceduralRenderableResult m_result;
    uint                             m_initial_primitive_count = 0;
    size_t                           m_initial_material_count  = 0;
    bool                             m_waiting_for_sync        = false;
};

class SetNodePropertiesTestCase final : public SceneTestCaseBase {
public:
    std::string_view Name() const override {
        return "SetNodeProperties";
    }

    void Reset(Scene&) override {
        ResetBaseState();
        m_node_entity = entt::null;
        m_stage       = Stage::CreateNode;
    }

    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_stage == Stage::CreateNode) {
            EntityWithNodeCreateInfo create_info{};
            create_info.name        = m_initial_name;
            create_info.translation = m_initial_translation;
            create_info.scale       = m_initial_scale;

            m_node_entity = scene.CreateEntityWithNode(create_info);
            m_stage       = Stage::WaitCreateSync;
            return;
        }

        if (m_stage == Stage::EditNode) {
            const bool set_name_result        = scene.SetNodeName(m_node_entity, m_target_name);
            const bool set_translation_result = scene.SetNodeTranslation(m_node_entity, m_target_translation);
            const bool set_rotation_result    = scene.SetNodeRotation(m_node_entity, m_target_rotation);
            const bool set_scale_result       = scene.SetNodeScale(m_node_entity, m_target_scale);

            Expect(set_name_result, "SetNodeName should succeed for a valid node.");
            Expect(set_translation_result, "SetNodeTranslation should succeed for a valid node.");
            Expect(set_rotation_result, "SetNodeRotation should succeed for a valid node.");
            Expect(set_scale_result, "SetNodeScale should succeed for a valid node.");

            if (m_failed) {
                Finish();
                return;
            }

            m_stage = Stage::WaitEditSync;
            return;
        }

        if (m_stage == Stage::Cleanup) {
            Expect(scene.DestroyEntity(m_node_entity), "DestroyEntity cleanup should succeed.");
            if (m_failed) {
                Finish();
                return;
            }

            m_stage = Stage::WaitCleanupSync;
        }
    }

    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (m_stage == Stage::WaitCreateSync) {
            auto        local_transform = scene.TryGetNodeLocalTransform(m_node_entity);
            std::string node_name;

            Expect(tick_state.did_sync, "SetNodeProperties setup should trigger scene sync.");
            Expect(tick_state.updated_transform, "SetNodeProperties setup should update transform data.");
            Expect(scene.IsValidNodeEntity(m_node_entity), "Created node should be valid.");
            Expect(scene.TryGetNodeName(m_node_entity, node_name), "Created node should expose its name.");
            Expect(
                node_name == m_initial_name, "Created node name does not match the requested initial name."
            );
            Expect(local_transform.has_value(), "Created node should expose its local transform.");
            Expect(
                IsNear(local_transform->translation, m_initial_translation),
                "Created node local translation is incorrect."
            );
            Expect(IsNear(local_transform->scale, m_initial_scale), "Created node local scale is incorrect.");

            m_stage = Stage::EditNode;
            return;
        }

        if (m_stage == Stage::WaitEditSync) {
            auto        local_transform = scene.TryGetNodeLocalTransform(m_node_entity);
            std::string node_name;

            Expect(tick_state.did_sync, "SetNodeProperties should trigger scene sync.");
            Expect(tick_state.updated_transform, "SetNodeProperties should update transform data.");
            Expect(scene.TryGetNodeName(m_node_entity, node_name), "Edited node should expose its name.");
            Expect(node_name == m_target_name, "Edited node name does not match the requested value.");
            Expect(local_transform.has_value(), "Edited node should expose its local transform.");
            Expect(
                IsNear(local_transform->translation, m_target_translation),
                "Edited node local translation is incorrect."
            );
            Expect(
                IsNear(local_transform->rotation, m_target_rotation),
                "Edited node local rotation is incorrect."
            );
            Expect(IsNear(local_transform->scale, m_target_scale), "Edited node local scale is incorrect.");

            m_stage = Stage::Cleanup;
            return;
        }

        if (m_stage == Stage::WaitCleanupSync) {
            Expect(tick_state.did_sync, "SetNodeProperties cleanup should trigger scene sync.");
            Expect(!scene.IsValidNodeEntity(m_node_entity), "Cleanup node should be invalid.");
            Finish();
        }
    }

private:
    enum class Stage {
        CreateNode,
        WaitCreateSync,
        EditNode,
        WaitEditSync,
        Cleanup,
        WaitCleanupSync,
    };

    entt::entity m_node_entity = entt::null;
    Stage        m_stage       = Stage::CreateNode;

    std::string m_initial_name        = "SceneTestCase Editable Node";
    std::string m_target_name         = "SceneTestCase Edited Node";
    float3      m_initial_translation = float3(-2.f, 0.5f, 0.f);
    float3      m_target_translation  = float3(-1.25f, 2.f, 0.75f);
    float3      m_initial_scale       = float3(1.f, 1.f, 1.f);
    float3      m_target_scale        = float3(1.5f, 0.75f, 2.f);
    Quaternion  m_target_rotation     = Quaternion(float3(0.f, 0.f, -1.f), float3(1.f, 0.f, 0.f));
};

class QueryNodeAndLocalTransformTestCase final : public SceneTestCaseBase {
public:
    std::string_view Name() const override {
        return "QueryNodeAndLocalTransform";
    }

    void Reset(Scene& scene) override {
        ResetBaseState();
        m_root                     = scene.GetRootNodeEntity();
        m_plain_entity             = entt::null;
        m_parent                   = entt::null;
        m_child_a                  = entt::null;
        m_child_b                  = entt::null;
        m_initial_root_child_count = scene.GetNodeChildCount(m_root);
        m_stage                    = EStage::CreatePlainEntity;
    }

    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_stage == EStage::CreatePlainEntity) {
            m_plain_entity = scene.CreateEntity(m_plain_entity_name);
            m_stage        = EStage::WaitPlainEntityTick;
            return;
        }

        if (m_stage == EStage::CreateHierarchy) {
            EntityWithNodeCreateInfo parent_info{};
            parent_info.name        = m_parent_name;
            parent_info.translation = m_parent_translation;
            m_parent                = scene.CreateEntityWithNode(parent_info);

            EntityWithNodeCreateInfo child_a_info{};
            child_a_info.parent_node_entt = m_parent;
            child_a_info.name             = m_child_a_name;
            child_a_info.translation      = m_child_a_translation;
            m_child_a                     = scene.CreateEntityWithNode(child_a_info);

            EntityWithNodeCreateInfo child_b_info{};
            child_b_info.parent_node_entt = m_parent;
            child_b_info.name             = m_child_b_name;
            child_b_info.translation      = m_child_b_translation;
            m_child_b                     = scene.CreateEntityWithNode(child_b_info);

            m_stage = EStage::WaitHierarchySync;
            return;
        }

        if (m_stage == EStage::SetLocalTransform) {
            const Transform target_transform(m_target_translation, m_target_scale, m_target_rotation);
            Expect(
                scene.SetLocalTransform(m_child_a, target_transform),
                "SetLocalTransform should succeed for a valid node."
            );
            if (m_failed) {
                Finish();
                return;
            }

            m_stage = EStage::WaitLocalTransformSync;
            return;
        }

        if (m_stage == EStage::Cleanup) {
            const bool destroy_plain_entity_result = scene.DestroyEntity(m_plain_entity);
            const bool destroy_child_a_result      = scene.DestroyEntity(m_child_a);
            const bool destroy_child_b_result      = scene.DestroyEntity(m_child_b);
            const bool destroy_parent_result       = scene.DestroyEntity(m_parent);

            Expect(destroy_plain_entity_result, "DestroyEntity should succeed for the plain entity.");
            Expect(destroy_child_a_result, "DestroyEntity should succeed for child A.");
            Expect(destroy_child_b_result, "DestroyEntity should succeed for child B.");
            Expect(destroy_parent_result, "DestroyEntity should succeed for the parent node.");
            if (m_failed) {
                Finish();
                return;
            }

            m_stage = EStage::WaitCleanupSync;
        }
    }

    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (m_stage == EStage::WaitPlainEntityTick) {
            auto&       registry = scene.r();
            std::string plain_node_name;
            auto        plain_local_transform = scene.TryGetNodeLocalTransform(m_plain_entity);

            Expect(m_plain_entity != entt::null, "CreateEntity should return a valid entity.");
            Expect(registry.valid(m_plain_entity), "Plain entity should remain valid after creation.");
            Expect(!registry.all_of<ecs::CNode>(m_plain_entity), "CreateEntity should not attach CNode.");
            Expect(!tick_state.did_sync, "CreateEntity should not trigger scene sync.");
            Expect(!scene.IsValidNodeEntity(m_plain_entity), "Plain entity should not be treated as a node.");
            Expect(
                !scene.TryGetNodeName(m_plain_entity, plain_node_name),
                "TryGetNodeName should fail for a plain entity."
            );
            Expect(
                !plain_local_transform.has_value(), "TryGetNodeLocalTransform should fail for a plain entity."
            );

            m_stage = EStage::CreateHierarchy;
            return;
        }

        if (m_stage == EStage::WaitHierarchySync) {
            auto&                   registry           = scene.r();
            entt::entity            main_camera_entity = entt::null;
            Scene::NodeSubtreeStats parent_stats{};
            Array<entt::entity>     collected_children;
            std::string             parent_name;

            Expect(tick_state.did_sync, "Node query setup should trigger scene sync.");
            Expect(tick_state.updated_transform, "Node query setup should update transform data.");
            Expect(scene.IsValidNodeEntity(m_root), "Root node should be valid.");
            Expect(scene.IsRootNode(m_root), "GetRootNodeEntity should return a root node.");
            Expect(!scene.IsRootNode(m_parent), "Created parent should not be a root node.");
            Expect(scene.IsValidNodeEntity(m_parent), "Created parent should be a valid node.");
            Expect(scene.IsValidNodeEntity(m_child_a), "Child A should be a valid node.");
            Expect(scene.IsValidNodeEntity(m_child_b), "Child B should be a valid node.");
            Expect(
                scene.GetNodeChildCount(m_root) == m_initial_root_child_count + 1,
                "Root child count should increase by one after adding the parent node."
            );
            Expect(scene.GetNodeChildCount(m_parent) == 2, "Parent should have exactly two children.");
            Expect(
                scene.GetNodeDisplayName(m_parent) == m_parent_name,
                "GetNodeDisplayName should return the assigned node name."
            );
            Expect(
                scene.GetNode(m_parent).name == m_parent_name, "GetNode should expose the assigned node name."
            );
            Expect(
                scene.TryGetNodeName(m_parent, parent_name), "TryGetNodeName should succeed for a valid node."
            );
            Expect(parent_name == m_parent_name, "TryGetNodeName should return the assigned node name.");

            main_camera_entity = scene.GetMainCameraEntity();
            if (Expect(
                    main_camera_entity != entt::null, "GetMainCameraEntity should return a valid entity."
                )) {
                Expect(
                    registry.all_of<ecs::CCamera>(main_camera_entity),
                    "GetMainCameraEntity should point to an entity with CCamera."
                );
                if (registry.all_of<ecs::CCamera>(main_camera_entity)) {
                    Expect(
                        &scene.GetMainCamera() == &registry.get<ecs::CCamera>(main_camera_entity),
                        "GetMainCamera should return the component of GetMainCameraEntity."
                    );
                }
            }

            scene.ForEachNodeChild(m_parent, [&](entt::entity child_entt) {
                collected_children.push_back(child_entt);
            });
            Expect(collected_children.size() == 2, "ForEachNodeChild should enumerate both children.");

            bool found_child_a = false;
            bool found_child_b = false;
            for (const entt::entity child_entt : collected_children) {
                found_child_a = found_child_a || child_entt == m_child_a;
                found_child_b = found_child_b || child_entt == m_child_b;
            }
            Expect(found_child_a, "ForEachNodeChild should enumerate child A.");
            Expect(found_child_b, "ForEachNodeChild should enumerate child B.");

            parent_stats = scene.GetNodeSubtreeStats(m_parent);
            Expect(parent_stats.node_count == 3, "Parent subtree should contain parent plus two children.");
            Expect(parent_stats.renderable_count == 0, "Parent subtree should not contain renderables.");
            Expect(parent_stats.camera_count == 0, "Parent subtree should not contain cameras.");
            Expect(parent_stats.light_count == 0, "Parent subtree should not contain lights.");
            Expect(!parent_stats.contains_main_camera, "Parent subtree should not contain the main camera.");
            Expect(
                !parent_stats.contains_main_light_tag, "Parent subtree should not contain the main light tag."
            );

            if (Expect(
                    registry.all_of<ecs::CNode>(m_child_a), "Child A should still own a CNode component."
                )) {
                Expect(
                    IsNear(
                        GetWorldTranslation(registry.get<ecs::CNode>(m_child_a)),
                        m_parent_translation + m_child_a_translation
                    ),
                    "Child A world translation after hierarchy creation is incorrect."
                );
            }

            m_stage = EStage::SetLocalTransform;
            return;
        }

        if (m_stage == EStage::WaitLocalTransformSync) {
            auto local_transform = scene.TryGetNodeLocalTransform(m_child_a);

            Expect(tick_state.did_sync, "SetLocalTransform should trigger scene sync.");
            Expect(tick_state.updated_transform, "SetLocalTransform should update transform data.");
            Expect(local_transform.has_value(), "TryGetNodeLocalTransform should succeed for child A.");
            Expect(
                IsNear(local_transform->translation, m_target_translation),
                "SetLocalTransform should update local translation."
            );
            Expect(
                IsNear(local_transform->rotation, m_target_rotation),
                "SetLocalTransform should update local rotation."
            );
            Expect(
                IsNear(local_transform->scale, m_target_scale), "SetLocalTransform should update local scale."
            );
            Expect(
                IsNear(
                    GetWorldTranslation(scene.GetNode(m_child_a)), m_parent_translation + m_target_translation
                ),
                "SetLocalTransform should update the derived world translation."
            );

            m_stage = EStage::Cleanup;
            return;
        }

        if (m_stage == EStage::WaitCleanupSync) {
            auto& registry = scene.r();
            Expect(
                tick_state.did_sync, "Cleanup should trigger scene sync because node entities were destroyed."
            );
            Expect(
                tick_state.updated_transform,
                "Cleanup should update transform data because node entities were destroyed."
            );
            Expect(!registry.valid(m_plain_entity), "Plain entity should be invalid after cleanup.");
            Expect(!scene.IsValidNodeEntity(m_parent), "Parent should be invalid after cleanup.");
            Expect(!scene.IsValidNodeEntity(m_child_a), "Child A should be invalid after cleanup.");
            Expect(!scene.IsValidNodeEntity(m_child_b), "Child B should be invalid after cleanup.");
            Expect(
                scene.GetNodeChildCount(m_root) == m_initial_root_child_count,
                "Root child count should return to baseline after cleanup."
            );
            Finish();
        }
    }

private:
    enum class EStage {
        CreatePlainEntity,
        WaitPlainEntityTick,
        CreateHierarchy,
        WaitHierarchySync,
        SetLocalTransform,
        WaitLocalTransformSync,
        Cleanup,
        WaitCleanupSync,
    };

    entt::entity m_root         = entt::null;
    entt::entity m_plain_entity = entt::null;
    entt::entity m_parent       = entt::null;
    entt::entity m_child_a      = entt::null;
    entt::entity m_child_b      = entt::null;

    uint   m_initial_root_child_count = 0;
    EStage m_stage                    = EStage::CreatePlainEntity;

    std::string m_plain_entity_name = "SceneTestCase Plain Entity";
    std::string m_parent_name       = "SceneTestCase Query Parent";
    std::string m_child_a_name      = "SceneTestCase Query Child A";
    std::string m_child_b_name      = "SceneTestCase Query Child B";

    float3     m_parent_translation  = float3(-3.f, 0.f, -2.f);
    float3     m_child_a_translation = float3(0.f, 1.f, 0.f);
    float3     m_child_b_translation = float3(1.f, 0.5f, 0.f);
    float3     m_target_translation  = float3(0.5f, 2.f, -0.25f);
    float3     m_target_scale        = float3(0.75f, 1.25f, 1.5f);
    Quaternion m_target_rotation     = Quaternion(float3(0.f, 1.f, 0.f), float3(0.f, 0.f, 1.f));
};

class DestroyNodeSubtreeTestCase final : public SceneTestCaseBase {
public:
    std::string_view Name() const override {
        return "DestroyNodeSubtree";
    }

    void Reset(Scene& scene) override {
        ResetBaseState();
        m_root                     = scene.GetRootNodeEntity();
        m_parent                   = entt::null;
        m_child                    = entt::null;
        m_grandchild               = entt::null;
        m_initial_root_child_count = scene.GetNodeChildCount(m_root);
        m_stage                    = Stage::CreateHierarchy;
    }

    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_stage == Stage::CreateHierarchy) {
            EntityWithNodeCreateInfo parent_info{};
            parent_info.name        = "SceneTestCase Subtree Parent";
            parent_info.translation = m_parent_translation;
            m_parent                = scene.CreateEntityWithNode(parent_info);

            EntityWithNodeCreateInfo child_info{};
            child_info.parent_node_entt = m_parent;
            child_info.name             = "SceneTestCase Subtree Child";
            child_info.translation      = m_child_translation;
            m_child                     = scene.CreateEntityWithNode(child_info);

            EntityWithNodeCreateInfo grandchild_info{};
            grandchild_info.parent_node_entt = m_child;
            grandchild_info.name             = "SceneTestCase Subtree Grandchild";
            grandchild_info.translation      = m_grandchild_translation;
            m_grandchild                     = scene.CreateEntityWithNode(grandchild_info);

            m_stage = Stage::WaitCreateSync;
            return;
        }

        if (m_stage == Stage::DestroyHierarchy) {
            Expect(
                scene.DestroyNodeSubtree(m_parent), "DestroyNodeSubtree should succeed for a test subtree."
            );
            if (m_failed) {
                Finish();
                return;
            }

            m_stage = Stage::WaitDestroySync;
        }
    }

    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (m_stage == Stage::WaitCreateSync) {
            Expect(tick_state.did_sync, "DestroyNodeSubtree setup should trigger scene sync.");
            Expect(tick_state.updated_transform, "DestroyNodeSubtree setup should update transform data.");
            Expect(scene.IsValidNodeEntity(m_root), "Scene root node should remain valid after creation.");
            Expect(scene.IsValidNodeEntity(m_parent), "Subtree parent should be valid after creation.");
            Expect(scene.IsValidNodeEntity(m_child), "Subtree child should be valid after creation.");
            Expect(
                scene.IsValidNodeEntity(m_grandchild), "Subtree grandchild should be valid after creation."
            );
            Expect(
                scene.GetNodeChildCount(m_root) == m_initial_root_child_count + 1,
                "Root child count should increase by one after adding the subtree root."
            );
            Expect(scene.GetNodeChildCount(m_parent) == 1, "Subtree parent should own exactly one child.");
            Expect(scene.GetNodeChildCount(m_child) == 1, "Subtree child should own exactly one child.");

            m_stage = Stage::DestroyHierarchy;
            return;
        }

        if (m_stage == Stage::WaitDestroySync) {
            Expect(
                !tick_state.did_sync,
                "DestroyNodeSubtree should rebuild scene immediately instead of using tag-driven Tick sync."
            );
            Expect(
                scene.HasPendingGpuSceneUpdate(),
                "DestroyNodeSubtree should leave a pending GPU scene update after immediate rebuild."
            );
            Expect(!scene.IsValidNodeEntity(m_parent), "Destroyed subtree parent should be invalid.");
            Expect(!scene.IsValidNodeEntity(m_child), "Destroyed subtree child should be invalid.");
            Expect(!scene.IsValidNodeEntity(m_grandchild), "Destroyed subtree grandchild should be invalid.");
            Expect(
                scene.GetNodeChildCount(m_root) == m_initial_root_child_count,
                "Root child count should return to its initial baseline after subtree destroy."
            );
            Finish();
        }
    }

private:
    enum class Stage {
        CreateHierarchy,
        WaitCreateSync,
        DestroyHierarchy,
        WaitDestroySync,
    };

    entt::entity m_root       = entt::null;
    entt::entity m_parent     = entt::null;
    entt::entity m_child      = entt::null;
    entt::entity m_grandchild = entt::null;

    uint  m_initial_root_child_count = 0;
    Stage m_stage                    = Stage::CreateHierarchy;

    float3 m_parent_translation     = float3(2.f, 0.f, -2.f);
    float3 m_child_translation      = float3(0.f, 1.f, 0.f);
    float3 m_grandchild_translation = float3(0.f, 0.5f, 1.f);
};

class ImportSceneFromFileTestCase final : public SceneTestCaseBase {
public:
    std::string_view Name() const override {
        return "ImportSceneFromFile";
    }

    void Reset(Scene& scene) override {
        ResetBaseState();
        m_root                     = scene.GetRootNodeEntity();
        m_import_root              = entt::null;
        m_import_result            = {};
        m_initial_root_child_count = scene.GetNodeChildCount(m_root);
        m_stage                    = EStage::ImportScene;
    }

    void PreTick(Scene& scene, const SceneTestCaseContext&) override {
        if (m_stage == EStage::ImportScene) {
            const auto& import_scene_path = GetImportSceneTestFilePath();
            if (!Expect(
                    std::filesystem::exists(import_scene_path),
                    "Import fixture scene file should exist under asset/scenes/mizuki."
                )) {
                Finish();
                return;
            }

            m_import_result = scene.ImportSceneFromFileSync(import_scene_path);
            if (!m_import_result) {
                const std::string_view failure_message =
                    m_import_result.error_message.empty() ?
                        std::string_view("ImportSceneFromFileSync should succeed for the mizuki fixture.") :
                        std::string_view(m_import_result.error_message);
                Expect(false, failure_message);
                Finish();
                return;
            }

            m_import_root = m_import_result.import_root_entt;
            m_stage       = EStage::WaitImportTick;
            return;
        }

        if (m_stage == EStage::CleanupImport) {
            Expect(
                scene.DestroyNodeSubtree(m_import_root),
                "DestroyNodeSubtree should succeed for the imported fixture root."
            );
            if (m_failed) {
                Finish();
                return;
            }

            m_stage = EStage::WaitCleanupTick;
        }
    }

    void PostTick(Scene& scene, const Scene::TickState& tick_state) override {
        if (m_stage == EStage::WaitImportTick) {
            Scene::NodeSubtreeStats import_stats{};
            std::string             import_root_name;

            Expect(
                !tick_state.did_sync,
                "ImportSceneFromFileSync should rebuild scene immediately instead of using tag-driven Tick "
                "sync."
            );
            Expect(
                scene.HasPendingGpuSceneUpdate(),
                "ImportSceneFromFileSync should leave a pending GPU scene update after immediate rebuild."
            );
            Expect(
                m_import_result.import_root_entt != entt::null, "Import should return a valid import root."
            );
            Expect(
                m_import_result.imported_entity_count > 0,
                "Import should report at least one imported entity from the mizuki fixture."
            );
            auto import_root_local_transform = scene.TryGetNodeLocalTransform(m_import_root);

            Expect(scene.IsValidNodeEntity(m_root), "Scene root should remain valid after import.");
            Expect(scene.IsValidNodeEntity(m_import_root), "Imported fixture root should be a valid node.");
            Expect(
                scene.GetNodeChildCount(m_root) == m_initial_root_child_count + 1,
                "Import should append exactly one new direct child under the scene root."
            );
            Expect(
                scene.TryGetNodeName(m_import_root, import_root_name),
                "Imported fixture root should expose its generated node name."
            );
            Expect(
                import_root_local_transform.has_value(),
                "Imported fixture root should expose the preserved source root local transform."
            );
            Expect(
                import_root_name == "Imported: mizuki.gltf",
                "Imported fixture root name should match the generated import prefix."
            );
            Expect(
                IsNear(import_root_local_transform->translation, float3(0.f, 0.f, 0.f)),
                "Imported fixture root translation should preserve the source root local translation."
            );
            Expect(
                IsNear(import_root_local_transform->scale, float3(0.25f, 0.25f, 0.25f)),
                "Imported fixture root scale should preserve the mizuki fixture root scale."
            );
            Expect(
                IsNear(
                    import_root_local_transform->rotation,
                    Quaternion(float3(1.f, 0.f, 0.f), Angle::MakeFromDegree(-90.f))
                ),
                "Imported fixture root rotation should preserve the mizuki fixture root rotation."
            );
            Expect(
                scene.GetNodeChildCount(m_import_root) > 0,
                "Imported fixture root should own at least one imported child node."
            );

            import_stats = scene.GetNodeSubtreeStats(m_import_root);
            Expect(
                import_stats.node_count > 1, "Imported fixture subtree should contain imported child nodes."
            );
            Expect(
                import_stats.renderable_count > 0,
                "Imported fixture subtree should contain at least one renderable node."
            );
            Expect(
                m_import_result.imported_entity_count >= static_cast<uint64>(import_stats.node_count - 1),
                "Imported entity count should cover the imported node subtree, excluding the wrapper root."
            );

            m_stage = EStage::CleanupImport;
            return;
        }

        if (m_stage == EStage::WaitCleanupTick) {
            Expect(
                !tick_state.did_sync,
                "Import fixture cleanup should rebuild scene immediately instead of using tag-driven Tick "
                "sync."
            );
            Expect(
                scene.HasPendingGpuSceneUpdate(),
                "Import fixture cleanup should leave a pending GPU scene update after immediate rebuild."
            );
            Expect(
                !scene.IsValidNodeEntity(m_import_root),
                "Imported fixture root should be invalid after cleanup."
            );
            Expect(
                scene.GetNodeChildCount(m_root) == m_initial_root_child_count,
                "Scene root child count should return to its initial baseline after import cleanup."
            );
            Finish();
        }
    }

private:
    enum class EStage {
        ImportScene,
        WaitImportTick,
        CleanupImport,
        WaitCleanupTick,
    };

    entt::entity                     m_root        = entt::null;
    entt::entity                     m_import_root = entt::null;
    Scene::ImportSceneFromFileResult m_import_result{};

    uint   m_initial_root_child_count = 0;
    EStage m_stage                    = EStage::ImportScene;
};

} // namespace

std::string_view GetSceneTestCaseName(ESceneTestCaseId test_case_id) {
    if (test_case_id == ESceneTestCaseId::None) {
        return "None";
    }

    for (const SceneTestCaseDescriptor& descriptor : kSceneTestCaseDescriptors) {
        if (descriptor.id == test_case_id) {
            return descriptor.name;
        }
    }
    return "Unknown";
}

const Array<ESceneTestCaseId>& GetAllSceneTestCaseIds() {
    static const Array<ESceneTestCaseId> s_all_case_ids = [] {
        Array<ESceneTestCaseId> case_ids;
        case_ids.reserve(std::size(kSceneTestCaseDescriptors));
        for (const SceneTestCaseDescriptor& descriptor : kSceneTestCaseDescriptors) {
            case_ids.push_back(descriptor.id);
        }
        return case_ids;
    }();

    return s_all_case_ids;
}

// 根据 testcase 请求创建对应 testcase 实例
UniquePtr<ISceneTestCase> CreateSceneTestCase(const SceneTestCaseRequest& request) {
    switch (request.test_case_id) {
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
            return MakeUnique<CreateDestroyRenderableTestCase>(request.renderable_stress_create_enabled);
        case ESceneTestCaseId::CreateProceduralRenderable:
            return MakeUnique<CreateProceduralRenderableTestCase>();
        case ESceneTestCaseId::SetNodeProperties:
            return MakeUnique<SetNodePropertiesTestCase>();
        case ESceneTestCaseId::QueryNodeAndLocalTransform:
            return MakeUnique<QueryNodeAndLocalTransformTestCase>();
        case ESceneTestCaseId::DestroyNodeSubtree:
            return MakeUnique<DestroyNodeSubtreeTestCase>();
        case ESceneTestCaseId::DebugModifyMaterial:
            return MakeUnique<DebugModifyMaterialTestCase>();
        case ESceneTestCaseId::ImportSceneFromFile:
            return MakeUnique<ImportSceneFromFileTestCase>();
        default:
            return nullptr;
    }
}

} // namespace Moer
