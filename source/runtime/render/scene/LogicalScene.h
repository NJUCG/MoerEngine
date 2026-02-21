#pragma once

#include "LogicalComponents.h"
#include "RenderAPI.h"
#include "entt/entity/fwd.hpp"
#include <entt/fwd.hpp>

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
 * 换句话说，LogicalScene不能存储任何数据，只提供一系列函数(System)
 * 
 * 所有函数默认直接操作当前LogicalScene对象内的entt::registry
 * 
 * 所有System均以 S 开头；所有的辅助函数均以 U (utility) 开头
 */
class RENDER_API LogicalScene {

public:
    LogicalScene();
    ~LogicalScene();
    LogicalScene(const LogicalScene&)            = delete;
    LogicalScene& operator=(const LogicalScene&) = delete;

    entt::registry&       r();
    const entt::registry& r() const;

    void SBuildPrimitiveHash();

    void SBuildMeshHash();

    // SBuildMeshAABB 只负责将Primitive的AABB同步到Mesh
    void SBuildMeshAABB();

    // SUpdateAllNodeTransformAndAABB 负责将Node的变换和AABB同步到整个场景
    void SUpdateAllNodeTransformAndAABB();

    /**
     * 在指定parent下，添加一个child节点
     */
    void UEmplaceNodeToParent(
        const entt::entity parent_entt,
        CNode&             parent_node,
        const entt::entity child_id,
        CNode&             child_node
    );

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

    /**
     * 获取主方向光的方向
     */
    float3 GetDirectionalLightDirection(entt::entity entity) const;

    /**
     * 获取主点光源的位置
     */
    float3 GetPointLightPosition(entt::entity entity) const;
};

} // namespace Moer::ecs