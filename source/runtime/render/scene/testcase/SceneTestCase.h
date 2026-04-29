/**
 * 定义 Scene testcase 的执行上下文和基类接口
 */
#pragma once

#include "scene/Scene.h"
#include "scene/testcase/SceneTestCaseId.h"

#include <cstdint>
#include <string_view>

namespace Moer {

struct SceneTestCaseContext {
    std::uint64_t frame_index          = 0;
    float         elapsed_time_seconds = 0.f;
};

class ISceneTestCase {
public:
    // 析构 testcase 基类，保证派生类能正确释放
    virtual ~ISceneTestCase() = default;

    // 返回 testcase 的日志和 UI 名称
    virtual std::string_view Name() const = 0;

    // 重置 testcase 的内部状态
    virtual void Reset(Scene& scene) = 0;

    // 在 Scene 采样 dirty tag 之前执行测试写入
    virtual void PreTick(Scene& scene, const SceneTestCaseContext& context) = 0;

    // 在 Scene sync 之后检查 TickState 和派生数据
    virtual void PostTick(Scene& scene, const Scene::TickState& tick_state) = 0;

    // 返回 testcase 是否已经完成
    virtual bool IsFinished() const = 0;
};

} // namespace Moer
