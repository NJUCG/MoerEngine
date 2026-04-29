/**
 * 定义 Scene testcase 的稳定 ID，供 UI、Runner 和 factory 之间传递测试请求
 */
#pragma once

#include <cstdint>

namespace Moer {

enum class ESceneTestCaseId : std::uint32_t {
    // 不运行任何 Scene testcase
    None,

    // 验证 scene.Tick(true) 在无写入时没有副作用
    FrameworkNoop,

    // 创建一个 point light，并验证同一帧 Scene sync 能消费创建请求。
    CreatePointLightOnce,

    // 创建 point light 后跨帧 Patch transform，并验证 derived light position 更新
    PatchCreatedPointLightTransform,

    // 预留：验证未来 point light 删除 API 的 Scene sync 链路
    CreateDestroyPointLight,

    // 预留：验证未来 entity 与 transform component 结构性增删 API
    CreateEntityAddRemoveTransform,

    // 预留：验证未来 renderable 创建和删除 API
    CreateDestroyRenderable,
};

} // namespace Moer
