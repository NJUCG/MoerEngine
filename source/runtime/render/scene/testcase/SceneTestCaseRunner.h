/**
 * 定义 Scene testcase 的全局单例调度器
 */
#pragma once

#include "misc/STL.h"
#include "scene/testcase/SceneTestCase.h"
#include "scene/testcase/SceneTestCaseId.h"

#include <chrono>

namespace Moer {

// SceneTestCaseRunner 是 Scene testcase 的全局单例调度器
class SceneTestCaseRunner {
public:
    // 获取唯一的 Scene testcase runner 实例
    static SceneTestCaseRunner& Get();

    // 禁止拷贝构造，保证 runner 只有一个实例
    SceneTestCaseRunner(const SceneTestCaseRunner&) = delete;
    // 禁止拷贝赋值，保证 runner 只有一个实例
    SceneTestCaseRunner& operator=(const SceneTestCaseRunner&) = delete;

    // 请求运行一个 Scene testcase
    void RequestCase(ESceneTestCaseId test_case_id);

    // 配置 renderable create/destroy testcase 的创建压力模式
    void SetCreateDestroyRenderableStressEnabled(bool enabled);

    // 查询 renderable create/destroy testcase 是否启用创建压力模式
    bool IsCreateDestroyRenderableStressEnabled() const;

    // 查询当前是否有正在运行的 testcase
    bool HasActiveCase() const;

    // 查询当前是否有等待启动的 testcase
    bool HasPendingCase() const;

    // 在 Scene 采样 dirty tag 之前推进 testcase
    void PreTick(Scene& scene);

    // 在 Scene sync 之后推进 testcase
    void PostTick(Scene& scene, const Scene::TickState& tick_state);

private:
    // 私有构造函数，只允许 Get() 创建单例
    SceneTestCaseRunner() = default;

    // 构建本帧 testcase 上下文
    SceneTestCaseContext BuildContext();

    // 启动当前等待中的 testcase
    void StartPendingCase(Scene& scene);

private:
    ESceneTestCaseId                      m_pending_case_id = ESceneTestCaseId::None;
    UniquePtr<ISceneTestCase>             m_active_case;
    bool                                  m_create_destroy_renderable_stress_enabled = false;
    std::uint64_t                         m_frame_index                              = 0;
    std::chrono::steady_clock::time_point m_start_time = std::chrono::steady_clock::now();
};

} // namespace Moer
