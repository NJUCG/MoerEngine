/**
 * 定义 Scene testcase 的全局单例调度器
 */
#pragma once

#include "misc/STL.h"
#include "scene/testcase/SceneTestCase.h"

#include <chrono>

namespace Moer {

/**
 * SceneTestCaseRunner 是 scene testcase 的全局调度器。
 *
 * 结构:
 * - m_pending_case: 下一次要启动的 case
 * - m_active_case: 当前正在跑的 case
 * - frame/time: 给测试构造上下文
 *
 * 改这里:
 * - 改 testcase 调度节奏: SceneTestCaseRunner.h / .cpp
 * - 改 editor 请求入口: SceneTestCaseDispatcher.* + 相关 UI 入口
 * - 改 testcase 列表: SceneTestCaseRegistry.* + 各具体 testcase 文件
 *
 * 用法:
 * - 外部用 RequestCase() 提交 case
 * - Scene::Tick 前后分别调用 PreTick() / PostTick()
 */
class SceneTestCaseRunner {
public:
    // 获取唯一的 Scene testcase runner 实例
    static SceneTestCaseRunner& Get();

    // 禁止拷贝构造，保证 runner 只有一个实例
    SceneTestCaseRunner(const SceneTestCaseRunner&) = delete;
    // 禁止拷贝赋值，保证 runner 只有一个实例
    SceneTestCaseRunner& operator=(const SceneTestCaseRunner&) = delete;

    // 请求运行一个已经构造好的 Scene testcase
    bool RequestCase(ESceneTestCaseId test_case_id, UniquePtr<ISceneTestCase> test_case);

    // 查询当前是否有正在运行的 testcase
    bool HasActiveCase() const;

    // 查询当前是否有等待启动的 testcase
    bool HasPendingCase() const;

    // 查询当前是否有已完成但尚未消费的执行结果
    bool HasCompletedResult() const;

    // 取走最近一次执行完成的结果
    bool ConsumeCompletedResult(SceneTestCaseRunResult& out_result);

    // 清理未消费的旧执行结果
    void ClearCompletedResult();

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
    UniquePtr<ISceneTestCase>             m_pending_case;
    ESceneTestCaseId                      m_pending_case_id = ESceneTestCaseId::None;
    UniquePtr<ISceneTestCase>             m_active_case;
    ESceneTestCaseId                      m_active_case_id = ESceneTestCaseId::None;
    UniquePtr<SceneTestCaseRunResult>     m_completed_result;
    std::uint64_t                         m_active_case_begin_frame = 0;
    std::uint64_t                         m_frame_index             = 0;
    std::chrono::steady_clock::time_point m_start_time              = std::chrono::steady_clock::now();
};

} // namespace Moer
