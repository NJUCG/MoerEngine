/**
 * 实现 Scene testcase 单例调度器，负责请求排队、启动和 Tick 阶段推进
 */
#include "scene/testcase/SceneTestCaseRunner.h"

#include "log/LogSystem.h"

namespace Moer {

// 返回函数局部静态单例，避免全局初始化顺序问题
SceneTestCaseRunner& SceneTestCaseRunner::Get() {
    static SceneTestCaseRunner s_runner;
    return s_runner;
}

// 将外部构造好的 testcase 转成 pending testcase，已有 active/pending 时拒绝新请求
void SceneTestCaseRunner::RequestCase(UniquePtr<ISceneTestCase> test_case) {
    if (!test_case) {
        return;
    }

    const std::string_view requested_name = test_case->Name();

    if (m_active_case || m_pending_case) {
        LOG_WARNING(
            "SceneTestCase request '{}' ignored because '{}' is still running or pending.",
            requested_name,
            m_active_case ? m_active_case->Name() : m_pending_case->Name()
        );
        return;
    }

    m_pending_case = std::move(test_case);
    LOG_INFO("SceneTestCase request queued: {}.", requested_name);
}

// 返回当前是否有正在运行的 testcase
bool SceneTestCaseRunner::HasActiveCase() const {
    return m_active_case != nullptr;
}

// 返回当前是否有等待启动的 testcase
bool SceneTestCaseRunner::HasPendingCase() const {
    return m_pending_case != nullptr;
}

// 在 Scene dirty tag 采样前启动并推进 testcase 的写入阶段
void SceneTestCaseRunner::PreTick(Scene& scene) {
    if (!m_active_case && m_pending_case) {
        StartPendingCase(scene);
    }

    if (!m_active_case) {
        return;
    }

    m_active_case->PreTick(scene, BuildContext());
}

// 在 Scene sync 后推进 testcase 的验证阶段，并在完成后释放 active case
void SceneTestCaseRunner::PostTick(Scene& scene, const Scene::TickState& tick_state) {
    if (!m_active_case) {
        return;
    }

    m_active_case->PostTick(scene, tick_state);
    if (m_active_case->IsFinished()) {
        LOG_INFO("SceneTestCase finished: {}.", m_active_case->Name());
        m_active_case.reset();
    }
}

// 构建 testcase 每帧可用的时间和帧号信息
SceneTestCaseContext SceneTestCaseRunner::BuildContext() {
    SceneTestCaseContext context{};
    context.frame_index = m_frame_index++;
    context.elapsed_time_seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - m_start_time).count();
    return context;
}

// 启动当前等待中的 testcase 并重置其初始状态
void SceneTestCaseRunner::StartPendingCase(Scene& scene) {
    if (!m_pending_case) {
        return;
    }

    m_active_case = std::move(m_pending_case);

    m_start_time  = std::chrono::steady_clock::now();
    m_frame_index = 0;
    m_active_case->Reset(scene);
    LOG_INFO("SceneTestCase started: {}.", m_active_case->Name());
}

} // namespace Moer
