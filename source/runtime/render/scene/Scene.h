#pragma once

#include "CpuScene.h"
#include "GpuSceneUpdate.h"
#include "LogicalScene.h"
#include "RenderAPI.h"
#include "SceneLoadInfoAsync.h"
#include "rhi/RHIResource.h"
#include "scene/SceneCreateInfo.h"

#include "scene/LogicalComponents.h"

#include <cassert>
#include <entt/entt.hpp>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Moer {

struct SceneUpdateBatch;

/**
 * Scene 是 Game Thread 的运行时场景入口，负责维护 LogicalScene / CpuScene 并生成值语义更新快照。
 *
 * 结构:
 * - LogicalScene: ECS 数据、节点树、导入后的逻辑结果
 * - CpuScene: shader 需要的 CPU 连续缓冲和 entity->slot 映射
 * - GpuSceneUpdate: 交给 RenderScene 的纯值数据，不包含 ECS 引用或 GPU 资源
 * - RenderScene / GpuScene: renderer 侧所有权，不属于 Scene
 *
 * 改这里:
 * - 加新的对外场景 API: Scene.h + SceneQuery.cpp / SceneModify.cpp
 * - 改加载 / import / reset / cache 入口: SceneLifecycle.cpp + loader 目录
 * - 改每帧同步流程: SceneTickSync.cpp + CpuScene / GpuSceneUpdate
 * - 改 CPU Scene、bindless 或 Logical Scene bridge: SceneAccess.cpp
 *
 * 用法:
 * - 外部只通过 LoadSceneFromFile / Tick / Patch / Create* / Destroy* 操作场景
 * - 不直接改 CpuScene / RenderScene / GpuScene，先改 LogicalScene 或 Scene API
 * - Editor / tooling 优先依赖 Scene 和 scene/editing/SceneEditing，不直接碰 registry / LogicalScene
 *
 * ===============================================================
 *
 * 因为class Scene过于复杂，所以我们将Scene.cpp实现拆分为多个cpp文件：
 *
 * 文件职责划分：
 * - Scene.h：正式 runtime scene 接口，负责生命周期、tick/sync、通用 query/mutation、renderer handoff
 * - LogicalScene：内部 ECS / system 实现层
 * - SceneCreateInfo.h：负责 Scene API 的 CreateInfo 定义
 * - Scene.cpp拆分：
 *   - SceneLifecycle.cpp：负责生命周期 API，包括 load / import / reset / cache
 *   - SceneTickSync.cpp：负责 Tick / sync 主流程与 GpuSceneUpdate 快照生成
 *   - SceneQuery.cpp：负责 Scene query API
 *   - SceneModify.cpp：负责 Scene 修改 API，调用 LogicalScene，并维护同步数据与 Dirty 标记
 *   - SceneAccess.cpp：负责 CPU Scene、bindless 与 Logical Scene API
 * - scene/editing/SceneEditing：负责编辑器/工具层意图封装，把 UI 操作翻译为正式 Scene API 调用；不直接维护同步 tag
 *
 * 边界约束：
 * - Scene 是 Editor / Python / MCP 这类前端调用方应该优先依赖的正式入口
 * - LogicalScene / registry 仍存在，但属于 runtime 内部实现层
 * - logical_scene() / r() / GetRegistry() 这类接口仅保留给 runtime 内部和迁移阶段，不应继续向前端扩散
 * 
 * 【重要！】关于 pybind11
 * - 实现新的 Scene Query / Modify API 时，优先直接把正式 Scene API 设计成 pybind11 可绑定
 * - 数学值类型优先直接绑定到 pybind11，不要为了脚本接入把正式 API 改写成 STL 过渡层
 * - `entt::entity` 继续作为正式 API 类型；Python 侧通过 pybind11 type_caster 映射为 entity id / handle
 * - 避免继续扩散 `bool + out 参数` 这类不友好签名
 * - 复杂结果优先把正式 public struct / DTO 设计为可直接绑定，而不是在 scripting 层额外补 wrapper
 * - 当前不满足规范的旧 API，在实际接入脚本白名单时逐个整改原签名，不急着一次性全量重构
 */
class RENDER_API Scene {

    /*
     * Scene 类目录：
     * 1. Runtime Scene Update Feature Checklist
     * 2. 类型
     * 3. 生命周期 API / 私有 Helpers【实现位于：SceneLifecycle.cpp】
     * 4. 场景 Tick / 同步 API / 私有 Helpers【实现位于：SceneTickSync.cpp】
     * 5. 场景查询 API【实现位于：SceneQuery.cpp】
     * 6. 场景修改 API【实现位于：SceneModify.cpp】
     * 7. CPU Scene / Bindless API【实现位于：SceneAccess.cpp】
     * 8. Logical Scene API【实现位于：SceneAccess.cpp】
     * 9. 私有 State
     */

    /**
     * Runtime Scene Update Feature Checklist
     *
     * - Update
     *   - Light【done】
     *   - Material
     *   - Transform【done】
     *   - Mesh / Renderable
     * - Add
     *   - Light(PointLight)【done】
     *   - Material
     *   - Mesh / Renderable
     *   - Entity / Node
     * - Remove
     *   - Light(PointLight)【done】
     *   - Material
     *   - Mesh / Renderable
     *   - Entity / Node
     * - Rebuild
     *   - Material / Texture Handle
     *   - Mesh / Primitive / Instance Buffer
     *   - Raytracing BLAS / TLAS
     * - Infrastructure
     *   - Patch Entry【done】
     *   - Dirty / NeedUpdate Tag【done】
     *   - NeedCreate Tag【done】
     *   - Per-frame Guarded Tick【done】
     */

public:
    ////////////
    // MARK: 类型
    ////////////

    struct TickState {
        bool did_sync          = false;
        bool updated_light     = false;
        bool updated_material  = false;
        bool updated_transform = false;
        bool created_light     = false;
        bool created_material  = false;
        bool created_transform = false;
        bool destroyed_light   = false;
        bool rebuilt_mesh      = false;
        bool rebuilt_rt_blas   = false;

        explicit operator bool() const {
            return did_sync;
        }
    };

    struct ImportSceneFromFileResult {
        bool         success = false;
        std::string  error_message;
        entt::entity import_root_entt      = entt::null;
        uint64       imported_entity_count = 0;

        explicit operator bool() const {
            return success;
        }
    };

    struct NodeLocalTransform {
        float3     translation = float3(0.f, 0.f, 0.f);
        Quaternion rotation    = Quaternion();
        float3     scale       = float3(1.f, 1.f, 1.f);
    };

    struct NodeSubtreeStats {
        uint32 node_count              = 0;
        uint32 renderable_count        = 0;
        uint32 camera_count            = 0;
        uint32 light_count             = 0;
        bool   contains_main_camera    = false;
        bool   contains_main_light_tag = false;
    };

    struct EntityComponentFlags {
        bool is_valid_entity      = false;
        bool is_node              = false;
        bool is_root_node         = false;
        bool is_renderable        = false;
        bool is_camera            = false;
        bool is_light             = false;
        bool is_directional_light = false;
        bool is_point_light       = false;
        bool is_main_camera       = false;
        bool is_main_light        = false;
    };

    struct NodeVisibility {
        bool has_visibility_component           = false;
        bool visible_in_editor                  = true;
        bool visible_in_game                    = true;
        bool effectively_visible_in_editor      = true;
        bool effectively_visible_in_game        = true;
    };

    Scene();
    ~Scene() = default;

    Scene(const Scene&)            = delete;
    Scene& operator=(const Scene&) = delete;

public:
    ///////////////////////
    // MARK: 生命周期 API
    ///////////////////////

    /**
     * 异步从文件加载场景
     * 
     * 读取数据位于 SceneLoadInfo 中，可以通过 SceneLoadInfo::Get() 访问
     */
    void LoadSceneFromFileAsync(const std::filesystem::path& file_path);

    /**
     * 同步从文件加载场景，阻塞直到加载完成
     */
    void LoadSceneFromFile(const std::filesystem::path& file_path);

    bool SaveStateCache() const;

    bool ResetToSourceScene();

    ImportSceneFromFileResult ImportSceneFromFileSync(const std::filesystem::path& file_path);

    const std::filesystem::path& GetSourceFilePath() const;

    /**
     * 重置所有场景数据
     * 
     * 之后，需要手动调用 LoadSceneFromFileAsync 重新加载场景
     */
    void Reset();

    /**
     * 场景是否开始加载
     */
    bool IsStartLoading() const;

    /**
     * 场景是否加载完成
     */
    bool IsReady() const;

private:
    ////////////////////////////
    // MARK: 生命周期 私有 Helpers
    ////////////////////////////

    /**
     * 内部实现：从文件加载场景（同步执行）
     * 
     * LoadSceneFromFileAsync 和 LoadSceneFromFile 的公共实现
     */
    void LoadSceneInternal(const std::filesystem::path& file_path);

public:
    ///////////////////////////
    // MARK: 场景 Tick / 同步 API
    ///////////////////////////

    /**
     * 每帧调用，有需要时更新 CpuScene 数据。
     */
    const TickState& Tick(bool is_run_test_case = false);

    const TickState& GetLastTickState() const;

    SceneUpdateBatch PrepareUpdateBatch(bool is_run_test_case, bool capture_geometry_snapshot);

    bool HasPendingGpuSceneUpdate() const;
    void ConsumePendingGpuSceneUpdate();

private:
    /////////////////////////////////
    // MARK: 场景 Tick / 同步 私有 Helpers
    /////////////////////////////////

    TickState BuildPendingTickState() const;

public:
    ///////////////////////
    // MARK: 场景查询 API
    ///////////////////////

    entt::entity              GetRootNodeEntity() const;
    bool                      IsValidEntity(entt::entity entity) const;
    bool                      IsValidNodeEntity(entt::entity entity) const;
    bool                      IsRootNode(entt::entity entity) const;
    uint32                    GetNodeChildCount(entt::entity entity) const;
    entt::entity              GetNodeChildEntity(entt::entity entity, uint32 child_index) const;
    std::vector<entt::entity> ListNodeChildren(entt::entity entity) const;
    EntityComponentFlags      GetEntityComponentFlags(entt::entity entity) const;

    template<typename Fn>
    void ForEachNodeChild(entt::entity parent, Fn&& fn) const {
        if (!IsValidNodeEntity(parent)) {
            return;
        }

        const auto&  registry   = r();
        entt::entity child_entt = registry.get<ecs::CNode>(parent).first_child_entt;
        while (child_entt != entt::null) {
            if (!registry.valid(child_entt) || !registry.all_of<ecs::CNode>(child_entt)) {
                return;
            }

            const entt::entity next_sibling_entt = registry.get<ecs::CNode>(child_entt).next_sibling_entt;
            std::forward<Fn>(fn)(child_entt);
            child_entt = next_sibling_entt;
        }
    }

    std::string GetNodeDisplayName(entt::entity entity) const;
    // 按节点名字查找第一个匹配的 node entity，未找到时返回 entt::null
    entt::entity                      FindNodeEntityByName(std::string_view name) const;
    NodeSubtreeStats                  GetNodeSubtreeStats(entt::entity entity) const;
    bool                              TryGetNodeName(entt::entity entity, std::string& out_name) const;
    std::optional<NodeLocalTransform> TryGetNodeLocalTransform(entt::entity entity) const;
    NodeVisibility                    GetNodeVisibility(entt::entity entity) const;
    bool                              IsNodeVisibleInEditor(entt::entity entity) const;
    bool                              IsNodeVisibleInGame(entt::entity entity) const;

    entt::entity GetMainCameraEntity() const;
    entt::entity GetMainDirectionalLightEntity() const;
    entt::entity GetMainPointLightEntity() const;

    ecs::CCamera&                 GetMainCamera();
    const ecs::CLightDirectional& GetMainDirectionalLight() const;
    const ecs::CLightPoint&       GetMainPointLight() const;

    const ecs::CNode& GetNode(entt::entity entity) const;

public:
    //////////////////////////
    // MARK: 场景修改 API
    //////////////////////////

    // 低层 patch 入口，主要给 runtime 内部和受控迁移点使用。
    // editor / tooling 在有具名 API 时，应优先调用具名 Scene API 或 SceneEditing。
    template<typename T, typename Fn>
    T& Patch(entt::entity entity, Fn&& fn) {
        auto& registry = r();
        assert(registry.valid(entity) && "Patch target entity is invalid");
        assert(registry.all_of<T>(entity) && "Patch target entity does not have the component");

        registry.patch<T>(entity, std::forward<Fn>(fn));
        MarkDirty<T>(entity);
        return registry.get<T>(entity);
    }

    template<typename T>
    void MarkDirty(entt::entity entity);

    bool SetNodeName(entt::entity entity, std::string_view name);
    bool SetNodeTranslation(entt::entity entity, const float3& value);
    bool SetNodeRotation(entt::entity entity, const Quaternion& value);
    bool SetNodeScale(entt::entity entity, const float3& value);
    bool SetNodeVisibleInEditor(entt::entity entity, bool visible);
    bool SetNodeVisibleInGame(entt::entity entity, bool visible);

    // 创建普通 entity，不接入 scene node 树，也不触发 scene sync。
    entt::entity CreateEntity(std::string_view name = {});

    // 创建带 CNode 的 entity，并接入 parent 或 root node。
    entt::entity CreateEntityWithNode(const EntityWithNodeCreateInfo& create_info);

    // 创建带 CNode 和 CRenderable 的 entity，并复用已有 mesh 资源。
    entt::entity CreateRenderableWithNode(const RenderableCreateInfo& create_info);

    // 创建运行时 Material，并标记为需要创建 render-side material slot。
    entt::entity CreateMaterial(const MaterialCreateInfo& create_info);

    // 创建运行时 Primitive Data，并 append 到 CtxMegaBuffers。
    entt::entity CreatePrimitive(const PrimitiveCreateInfo& create_info);

    // 创建运行时 Mesh，第一版只做全量 mesh resource rebuild。
    entt::entity CreateMesh(const MeshCreateInfo& create_info);

    // 创建简单 procedural material + primitive + mesh + renderable。
    CreateProceduralRenderableResult CreateProceduralRenderable(const ProceduralMeshCreateInfo& create_info);

    // 修改已有 EntityWithNode 的 local transform，并标记 transform 同步。
    bool SetLocalTransform(entt::entity entity, const Transform& local_transform);

    // 将已有 EntityWithNode 重挂到新的 parent node 下。
    bool AttachToParent(entt::entity child_entt, entt::entity parent_entt);

    // 将已有 EntityWithNode 从当前 parent 下移除，并挂回 root node。
    bool DetachFromParent(entt::entity child_entt);

    // 删除普通 entity 或 leaf EntityWithNode，复杂 render-side entity 暂不支持。
    bool DestroyEntity(entt::entity entity);

    // 删除一个 node 及其所有子节点；当前通过重建 CpuScene 并发送全量快照保证 render-side 数据正确。
    bool DestroyNodeSubtree(entt::entity entity);

    // 删除 renderable 会在后续 Tick 中触发 mesh instance cache rebuild，当前先接受这部分开销
    bool DestroyRenderable(entt::entity renderable_entity);

    // 创建运行时 PointLight，并标记为需要创建 render-side light slot。
    entt::entity CreatePointLight(const PointLightCreateInfo& create_info);

    // 删除 point light 会在后续 Tick 中触发 light cache rebuild，当前先接受这部分开销
    bool DestroyPointLight(entt::entity light_entity);

public:
    ///////////////////////////
    // MARK: CPU Scene / Bindless API
    ///////////////////////////

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    const CpuScene& cpu_scene() const;

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    const CpuScene& GetCpuScene() const;

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    Render::BindlessArrayRef bindless_array();

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    Render::BindlessArrayRef GetBindlessArray();

    void SetBindlessArray(Render::BindlessArrayRef bindless_array);

public:
    ///////////////////////////////////////////
    // MARK: Logical Scene API
    ///////////////////////////////////////////

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    ecs::LogicalScene& logical_scene();

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    const ecs::LogicalScene& logical_scene() const;

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    ecs::LogicalScene& GetLogicalScene();

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    const ecs::LogicalScene& GetLogicalScene() const;

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    entt::registry& r();

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    const entt::registry& r() const;

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    entt::registry& GetRegistry();

    // Editor / 外部代码 不应该调用此接口，推荐通过Scene API操作场景
    const entt::registry& GetRegistry() const;

private:
    //////////////////////
    // MARK: 私有 State
    //////////////////////

    // Renderer 注入的共享 bindless array；保留给仍通过 Scene 获取它的 RT passes。
    Render::BindlessArrayRef m_bindless_array;

    UniquePtr<ecs::LogicalScene> m_logical_scene;
    UniquePtr<CpuScene>          m_cpu_scene;

    SceneLoadInfoAsync    m_scene_load_info;
    TickState             m_last_tick_state;
    std::filesystem::path m_source_file_path;
    bool                  m_has_pending_gpu_scene_update = false;
};

struct SceneGeometryInstanceSnapshot {
    uint64 key            = 0u;
    Box3D  bounds{};
    uint64 transform_hash = 0u;
};

struct SceneGeometrySnapshot {
    Array<Box3D>                         primitive_bounds;
    Array<SceneGeometryInstanceSnapshot> instances;
    uint                                 renderable_instance_count = 0u;
    uint                                 leaf_primitive_count       = 0u;
    uint                                 skipped_invalid_count      = 0u;
};

struct SceneUpdateBatch {
    SceneUpdateBatch() = default;

    SceneUpdateBatch(const SceneUpdateBatch&)            = delete;
    SceneUpdateBatch& operator=(const SceneUpdateBatch&) = delete;
    SceneUpdateBatch(SceneUpdateBatch&&)                 = default;
    SceneUpdateBatch& operator=(SceneUpdateBatch&&)      = default;

    bool scene_ready = false;

    Scene::TickState tick_state{};
    std::optional<Render::GpuSceneUpdate> initial_gpu_update;
    std::optional<Render::GpuSceneUpdate> update_gpu_update;

    Camera                                main_camera{};
    uint                                  light_count = 0u;
    std::optional<ecs::CLightDirectional> main_directional_light;
    std::optional<ecs::CLightPoint>       main_point_light;
    std::optional<SceneGeometrySnapshot>  geometry;
};

template<>
RENDER_API void Scene::MarkDirty<ecs::CLightDirectional>(entt::entity entity);

template<>
RENDER_API void Scene::MarkDirty<ecs::CLightPoint>(entt::entity entity);

template<>
RENDER_API void Scene::MarkDirty<ecs::CMaterial>(entt::entity entity);

template<>
RENDER_API void Scene::MarkDirty<ecs::CNode>(entt::entity entity);

} // namespace Moer
