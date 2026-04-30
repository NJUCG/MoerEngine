/**
 * 实现 Scene testcase 单例调度器，负责请求排队、启动和 Tick 阶段推进
 */
#include "scene/testcase/SceneTestCaseRunner.h"

#include "log/LogSystem.h"
#include "scene/testcase/SceneTestCaseRegistry.h"

namespace Moer {

// 返回函数局部静态单例，避免全局初始化顺序问题
SceneTestCaseRunner& SceneTestCaseRunner::Get() {
    static SceneTestCaseRunner s_runner;
    return s_runner;
}

// 将外部请求转成 pending testcase，已有 active/pending 时拒绝新请求
void SceneTestCaseRunner::RequestCase(ESceneTestCaseId test_case_id) {
    if (test_case_id == ESceneTestCaseId::None) {
        return;
    }

    if (m_active_case || m_pending_case_id != ESceneTestCaseId::None) {
        LOG_WARNING(
            "SceneTestCase request '{}' ignored because '{}' is still running or pending.",
            GetSceneTestCaseName(test_case_id),
            m_active_case ? m_active_case->Name() : GetSceneTestCaseName(m_pending_case_id)
        );
        return;
    }

    m_pending_case_id = test_case_id;
    LOG_INFO("SceneTestCase request queued: {}.", GetSceneTestCaseName(test_case_id));
}

// 设置 renderable create/destroy testcase 的创建压力模式
void SceneTestCaseRunner::SetCreateDestroyRenderableStressEnabled(bool enabled) {
    m_create_destroy_renderable_stress_enabled = enabled;
}

// 查询 renderable create/destroy testcase 的创建压力模式
bool SceneTestCaseRunner::IsCreateDestroyRenderableStressEnabled() const {
    return m_create_destroy_renderable_stress_enabled;
}

// 返回当前是否有正在运行的 testcase
bool SceneTestCaseRunner::HasActiveCase() const {
    return m_active_case != nullptr;
}

// 返回当前是否有等待启动的 testcase
bool SceneTestCaseRunner::HasPendingCase() const {
    return m_pending_case_id != ESceneTestCaseId::None;
}

// 在 Scene dirty tag 采样前启动并推进 testcase 的写入阶段
void SceneTestCaseRunner::PreTick(Scene& scene) {
    if (!m_active_case && m_pending_case_id != ESceneTestCaseId::None) {
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

// 从 pending ID 创建 testcase 实例并重置其初始状态
void SceneTestCaseRunner::StartPendingCase(Scene& scene) {
    const ESceneTestCaseId test_case_id = m_pending_case_id;
    m_pending_case_id                   = ESceneTestCaseId::None;

    m_active_case = CreateSceneTestCase(test_case_id);
    if (!m_active_case) {
        LOG_ERROR("SceneTestCase '{}' is not implemented.", GetSceneTestCaseName(test_case_id));
        return;
    }

    m_start_time  = std::chrono::steady_clock::now();
    m_frame_index = 0;
    m_active_case->Reset(scene);
    LOG_INFO("SceneTestCase started: {}.", m_active_case->Name());
}

} // namespace Moer
