/**
 * 定义 Scene testcase 的稳定 ID，供 UI、Runner 和 factory 之间传递测试请求
 */
#pragma once

#include <cstdint>

namespace Moer {

enum class ESceneTestCaseId : std::uint32_t {
    // 不运行任何 Scene testcase
    None,

    // Suite-only：批量运行前保存当前 Scene State Cache。
    SuiteSaveStateCache,

    // 验证 scene.Tick(true) 在无写入时没有副作用
    FrameworkNoop,

    // 创建一个 point light，并验证同一帧 Scene sync 能消费创建请求。
    CreatePointLightOnce,

    // 创建 point light 后跨帧 Patch transform，并验证 derived light position 更新
    PatchCreatedPointLightTransform,

    // 创建 point light 后再删除，并验证删除同步链路
    CreateDestroyPointLight,

    // 验证 EntityWithNode 创建、重挂、脱离和 leaf 删除主链路
    EntityWithNodeStructuralFlow,

    // 验证 EntityWithNode 结构性 API 拒绝关键非法操作
    EntityWithNodeRejectInvalidOps,

    // 验证 renderable 创建、复制偏移和删除同步链路
    CreateDestroyRenderable,

    // 验证 procedural material / primitive / mesh / renderable 创建链路
    CreateProceduralRenderable,

    // 验证具名 Node setter API 能正确修改名称和局部变换
    SetNodeProperties,

    // 验证普通 entity / node query / SetLocalTransform 相关 API
    QueryNodeAndLocalTransform,

    // 验证 DestroyNodeSubtree 会删除整棵 node 子树
    DestroyNodeSubtree,

    // 调试修改场景材质，并走 Scene sync 路径
    DebugModifyMaterial,

    // 验证同步 ImportSceneFromFileSync 能导入 fixture scene 并接入当前 scene
    ImportSceneFromFile,

    // Suite-only：批量运行结束后请求 reload，并从已保存的 State Cache 恢复场景。
    SuiteLoadStateCache,
};

} // namespace Moer
