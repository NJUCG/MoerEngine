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
bool SceneTestCaseRunner::RequestCase(ESceneTestCaseId test_case_id, UniquePtr<ISceneTestCase> test_case) {
    if (!test_case || test_case_id == ESceneTestCaseId::None) {
        return false;
    }

    const std::string_view requested_name = test_case->Name();

    if (m_active_case || m_pending_case) {
        LOG_WARNING(
            "SceneTestCase request '{}' ignored because '{}' is still running or pending.",
            requested_name,
            m_active_case ? m_active_case->Name() : m_pending_case->Name()
        );
        return false;
    }

    m_completed_result.reset();
    m_pending_case_id = test_case_id;
    m_pending_case    = std::move(test_case);
    LOG_INFO("SceneTestCase request queued: {}.", requested_name);
    return true;
}

// 返回当前是否有正在运行的 testcase
bool SceneTestCaseRunner::HasActiveCase() const {
    return m_active_case != nullptr;
}

// 返回当前是否有等待启动的 testcase
bool SceneTestCaseRunner::HasPendingCase() const {
    return m_pending_case != nullptr;
}

bool SceneTestCaseRunner::HasCompletedResult() const {
    return m_completed_result != nullptr;
}

bool SceneTestCaseRunner::ConsumeCompletedResult(SceneTestCaseRunResult& out_result) {
    if (!m_completed_result) {
        return false;
    }

    out_result = std::move(*m_completed_result);
    m_completed_result.reset();
    return true;
}

void SceneTestCaseRunner::ClearCompletedResult() {
    m_completed_result.reset();
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
        auto result             = MakeUnique<SceneTestCaseRunResult>();
        result->test_case_id    = m_active_case_id;
        result->case_name       = std::string(m_active_case->Name());
        result->begin_frame     = m_active_case_begin_frame;
        result->end_frame       = m_frame_index == 0 ? 0 : (m_frame_index - 1);
        result->passed          = !m_active_case->HasFailed();
        result->failure_summary = std::string(m_active_case->FailureSummary());
        m_completed_result      = std::move(result);

        LOG_INFO("SceneTestCase finished: {}.", m_active_case->Name());
        m_active_case.reset();
        m_active_case_id = ESceneTestCaseId::None;
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

    m_active_case     = std::move(m_pending_case);
    m_active_case_id  = m_pending_case_id;
    m_pending_case_id = ESceneTestCaseId::None;

    m_start_time              = std::chrono::steady_clock::now();
    m_frame_index             = 0;
    m_active_case_begin_frame = 0;
    m_active_case->Reset(scene);
    LOG_INFO("SceneTestCase started: {}.", m_active_case->Name());
}

} // namespace Moer
