#pragma once

#include "CpuScene.h"
#include "GpuScene.h"
#include "LogicalScene.h"
#include "RenderAPI.h"
#include "SceneLoadInfoAsync.h"
#include "scene/SceneCreateInfo.h"


#include "scene/LogicalComponents.h"

#include <cassert>
#include <entt/entt.hpp>
#include <filesystem>
#include <string_view>
#include <utility>

namespace Moer {

namespace Render {
class CommandList;
}

/**
 * MoerEngine场景
 * 
 * 非RAII，需要手动管理生命周期
 * 
 * 场景共分为3大部分：LogicalScene, CpuScene, GpuScene
 * - LogicalScene：场景的逻辑数据，使用ECS存储，适合运行时修改
 * - CpuScene：场景的CPU渲染数据，存储一系列准备upload到gpu的数据
 * - GpuScene：场景的GPU渲染数据，存储一系列gpu handle和view
 * 
 * Scene类作为三者的管理者，负责三者之间的数据同步和转换
 * - LogicalScene完全对外界暴露，外界可以自由修改
 * - CpuScene不允许外部修改、读取
 * - GpuScene不允许外部修改；外部可以读取数据
 * 
 * 每帧，通过Tick()来更新CpuScene和GpuScene的数据
 * - 具体逻辑全部封装，内部管理
 * 
 * // TODO: 实现RingBuffer
 * 
 * 文件职责划分：
 * - LogicalScene：负责具体 ECS 操作
 * - Scene：负责对外接口声明与场景同步管理
 * - SceneCreateInfo.h：负责 Scene API 的 CreateInfo 定义
 * - SceneMutation.cpp：负责调用 LogicalScene，并维护同步数据与 Dirty 标记
 */
class RENDER_API Scene {

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

        explicit operator bool() const {
            return did_sync;
        }
    };

    Scene();
    ~Scene() = default;

    Scene(const Scene&)            = delete;
    Scene& operator=(const Scene&) = delete;

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

    /**
    * 每帧调用，有需要时更新CpuScene和GpuScene数据
     */
    const TickState& Tick(bool is_run_test_case = false);

    const TickState& GetLastTickState() const;

    /**
     * 重置所有场景数据
     * 
     * 之后，需要手动调用 LoadSceneFromFileAsync 重新加载场景
     */
    void Reset();

public:
    // 场景加载状态相关接口

    /**
     * 场景是否开始加载
     */
    bool IsStartLoading() const;

    /**
     * 场景是否加载完成
     */
    bool IsReady() const;

public:
    // Graphics API相关接口
    Render::GpuScene::PendingCommandList&& PopPendingCommandList();

public:
    // 修改已有组件并自动标记对应的场景同步 dirty/tag。
    template<typename T, typename Fn>
    T& Patch(entt::entity entity, Fn&& fn) {
        auto& registry = r();
        assert(registry.valid(entity) && "Patch target entity is invalid");
        assert(registry.all_of<T>(entity) && "Patch target entity does not have the component");

        registry.patch<T>(entity, std::forward<Fn>(fn));
        MarkDirty<T>(entity);
        return registry.get<T>(entity);
    }

    // 标记组件修改带来的场景同步 dirty/tag，具体特化放在独立实现文件中。
    template<typename T>
    void MarkDirty(entt::entity entity);

    // 创建普通 entity，不接入 scene node 树，也不触发 scene sync。
    entt::entity CreateEntity(std::string_view name = {});

    // 创建带 CNode 的 entity，并接入 parent 或 root node。
    entt::entity CreateEntityWithNode(const EntityWithNodeCreateInfo& create_info);

    // 创建带 CNode 和 CRenderable 的 entity，并复用已有 mesh 资源。
    entt::entity CreateRenderableWithNode(const RenderableCreateInfo& create_info);

    // 修改已有 EntityWithNode 的 local transform，并标记 transform 同步。
    bool SetLocalTransform(entt::entity entity, const Transform& local_transform);

    // 将已有 EntityWithNode 重挂到新的 parent node 下。
    bool AttachToParent(entt::entity child_entt, entt::entity parent_entt);

    // 将已有 EntityWithNode 从当前 parent 下移除，并挂回 root node。
    bool DetachFromParent(entt::entity child_entt);

    // 删除普通 entity 或 leaf EntityWithNode，复杂 render-side entity 暂不支持。
    bool DestroyEntity(entt::entity entity);

    // 删除 renderable 会在后续 Tick 中触发 mesh instance cache rebuild，当前先接受这部分开销
    bool DestroyRenderable(entt::entity renderable_entity);

    // 创建运行时 PointLight，并标记为需要创建 render-side light slot。
    entt::entity CreatePointLight(const PointLightCreateInfo& create_info);

    // 删除 point light 会在后续 Tick 中触发 light cache rebuild，当前先接受这部分开销
    bool DestroyPointLight(entt::entity light_entity);

private:
    // 构造函数 初始化
    /**
     * Bindless Array Reference
     * 
     * m_bindless_array这里的初始化逻辑比较奇怪，需要提前初始化
     * 另外，滥用Ref导致BindlessArray生命周期管理不清晰。这个是历史遗留问题，难以修改
     */
    Render::BindlessArrayRef m_bindless_array;

    // LoadiSceneFromFileAsync 初始化
    UniquePtr<ecs::LogicalScene> m_logical_scene;
    UniquePtr<CpuScene>          m_cpu_scene;
    UniquePtr<Render::GpuScene>  m_gpu_scene;

    SceneLoadInfoAsync m_scene_load_info;
    TickState          m_last_tick_state;

private:
    /**
     * 内部实现：从文件加载场景（同步执行）
     * 
     * LoadSceneFromFileAsync 和 LoadSceneFromFile 的公共实现
     */
    void LoadSceneInternal(const std::filesystem::path& file_path);

    // 判断当前 Scene 是否存在需要同步到 CpuScene/GpuScene 的 tag。
    bool HasPendingSceneSync() const;

    TickState BuildPendingTickState() const;

public:
    /**
     * MARK: 一系列public getter
     */

    ecs::LogicalScene&       logical_scene();
    const ecs::LogicalScene& logical_scene() const;
    ecs::LogicalScene&       GetLogicalScene();
    const ecs::LogicalScene& GetLogicalScene() const;

    entt::registry&       r();
    const entt::registry& r() const;
    entt::registry&       GetRegistry();
    const entt::registry& GetRegistry() const;

    const Render::GpuScene::Res& gpu_scene_res() const;
    const Render::GpuScene::Res& GetGpuSceneRes() const;

    /**
     * 恢复 Draw Commands 到原始状态（用于关闭 GPU Culling 时）
     */
    void RestoreDrawCommands(Render::CommandList& cmd_list);

    const CpuScene& cpu_scene() const;
    const CpuScene& GetCpuScene() const;

    Render::BindlessArrayRef bindless_array();
    Render::BindlessArrayRef GetBindlessArray();

public:
    /**
     * MARK: 封装一些常用逻辑
     */

    entt::entity GetMainCameraEntity() const;
    entt::entity GetMainDirectionalLightEntity() const;
    entt::entity GetMainPointLightEntity() const;

    ecs::CCamera&                 GetMainCamera();
    const ecs::CLightDirectional& GetMainDirectionalLight() const;
    const ecs::CLightPoint&       GetMainPointLight() const;

    const ecs::CNode& GetNode(entt::entity entity) const;
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