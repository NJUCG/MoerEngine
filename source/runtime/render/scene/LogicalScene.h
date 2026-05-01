#pragma once

#include "LogicalComponents.h"
#include "RenderAPI.h"
#include "entt/entity/fwd.hpp"
#include "scene/SceneCreateInfo.h"
#include <entt/entt.hpp>


namespace Moer::ecs {

/**
 * Logical Scene
 * 
 * RAII，构造时初始化，析构时释放（不提供手动Initialize/Destroy/Reset接口）
 * 
 * MARK: MoerEngine场景结构
 * 
 * LogicalScene仅包含场景的逻辑信息，适用于运行时修改场景数据
 * 
 * 场景管理数据流：LogicalScene -> CpuScene -> GpuScene
 * 
 * 为了降低代码复杂度，LogicalScene直接抛出EnTT接口，不进行封装
 * 
 * MARK: ECS System
 * 
 * 这里存了大部分预定义的ECS System
 * 
 * 总所周知，ECS的核心理念之一，就是不在Component中存任何的逻辑代码（除了getter/setter）
 * - 这么做的核心目的是 解耦 数据与逻辑
 * - 如果我们随意地在Component中添加逻辑代码，那么当代码复杂了，Component之间的耦合就会变得非常严重
 * - 比如有可能出现 CompA -> CompB -> CompC -> CompB 这种循环引用
 * - 而ECS的作用，不仅仅是Cache友好，更是解耦数据和逻辑，避免耦合
 * 上述内容源自2017 GDC 暴雪团队的分享：https://www.bilibili.com/video/BV1p4411k7N8
 *
 * MARK: LogicalScene & ECS
 * 
 * LogicalScene即MoerEngine场景数据的ECS System实现。
 * 换句话说，LogicalScene不能存储除Registry以外的任何数据，只提供一系列函数(System)
 * 
 * 所有函数默认直接操作当前LogicalScene对象内的entt::registry
 * 
 * Update为外部统一更新入口；所有System均以 S 开头；所有的辅助函数均以 U (utility) 开头
 */
class RENDER_API LogicalScene {

public:
    LogicalScene();
    ~LogicalScene();
    LogicalScene(const LogicalScene&)            = delete;
    LogicalScene& operator=(const LogicalScene&) = delete;

    entt::registry&       r();
    const entt::registry& r() const;

    // LogicalScene 外部统一更新入口
    void Update();

    void SBuildPrimitiveHash();

    void SBuildMeshHash();

    // SBuildMeshAABB 只负责将Primitive的AABB同步到Mesh
    void SBuildMeshAABB();

    // SUpdateAllNodeTransformAndAABB 负责将Node的变换和AABB同步到整个场景
    void SUpdateAllNodeTransformAndAABB();

    // SUpdateAllLightData 负责将Light依赖的Transform派生数据同步到Light组件
    void SUpdateAllLightData();

    /**
     * 在指定parent下，添加一个child节点
     */
    void UEmplaceNodeToParent(
        const entt::entity parent_entt,
        CNode&             parent_node,
        const entt::entity child_id,
        CNode&             child_node
    );

    // 从父节点的 child 链表中摘除指定节点
    void UDetachNodeFromParent(entt::entity child_entt, CNode& child_node);

    entt::entity UGetRootNodeEntity();

    bool UIsEntityWithNode(entt::entity entity) const;

    entt::entity UCreateEntity(std::string_view name = {});

    entt::entity UCreateEntityWithNode(const EntityWithNodeCreateInfo& create_info);

    entt::entity UCreateRenderableWithNode(const RenderableCreateInfo& create_info);

    entt::entity UCreateMaterial(const MaterialCreateInfo& create_info);

    entt::entity UCreatePrimitive(const PrimitiveCreateInfo& create_info);

    entt::entity UCreateMesh(const MeshCreateInfo& create_info);

    bool USetLocalTransform(entt::entity entity, const Transform& local_transform);

    bool UAttachToParent(
        entt::entity  child_entt,
        entt::entity  parent_entt,
        entt::entity* old_parent_entt = nullptr,
        bool*         did_change      = nullptr
    );

    bool UDetachFromParent(
        entt::entity  child_entt,
        entt::entity* old_parent_entt = nullptr,
        bool*         did_change      = nullptr
    );

    bool UDestroyEntity(entt::entity entity, entt::entity* old_parent_entt = nullptr);

    bool UDestroyRenderable(entt::entity entity, entt::entity* old_parent_entt = nullptr);

    entt::entity UCreatePointLight(const PointLightCreateInfo& create_info);

    bool UCanDestroyPointLight(entt::entity light_entity);

    /**
     * 创建默认摄像机entity，并将其挂在在指定node下
     * 
     * 若parent_node_id为entt::null，则创建在根节点CTagRootNode下
     */
    void
    UCreateDefaultCamera(entt::entity parent_node_id = entt::null, bool shuold_create_main_camera = true);

    /**
     * 创建一或多个light entity，并将其挂在在指定node下
     * 
     * 若parent_node_id为entt::null，则创建在根节点CTagRootNode下
     */
    void UCreateDefaultLights(entt::entity parent_node_id = entt::null, bool should_create_main_light = true);

private:
    entt::registry m_registry;
};

} // namespace Moer::ecs