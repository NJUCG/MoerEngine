/**
 * Implements a minimal batch runner that requests Scene testcases one by one.
 */
#include "scene/testcase/SceneTestSuiteRunner.h"

#include "log/LogSystem.h"
#include "scene/Scene.h"
#include "scene/testcase/SceneTestCaseRegistry.h"
#include "scene/testcase/SceneTestCaseRunner.h"

namespace Moer {

namespace {

bool IsSaveStateCacheSuiteStep(ESceneTestCaseId test_case_id) {
    return test_case_id == ESceneTestCaseId::SuiteSaveStateCache;
}

bool IsLoadStateCacheSuiteStep(ESceneTestCaseId test_case_id) {
    return test_case_id == ESceneTestCaseId::SuiteLoadStateCache;
}

bool IsSuiteSpecialCase(ESceneTestCaseId test_case_id) {
    return IsSaveStateCacheSuiteStep(test_case_id) || IsLoadStateCacheSuiteStep(test_case_id);
}

SceneTestCaseRunResult
BuildImmediateResult(ESceneTestCaseId test_case_id, bool passed, std::string_view failure_summary = {}) {
    SceneTestCaseRunResult result{};
    result.test_case_id    = test_case_id;
    result.case_name       = std::string(GetSceneTestCaseName(test_case_id));
    result.begin_frame     = 0;
    result.end_frame       = 0;
    result.passed          = passed;
    result.failure_summary = std::string(failure_summary);
    return result;
}

} // namespace

SceneTestSuiteRunner& SceneTestSuiteRunner::Get() {
    static SceneTestSuiteRunner s_runner;
    return s_runner;
}

bool SceneTestSuiteRunner::RequestRunAll(bool renderable_stress_create_enabled, bool stop_on_failure) {
    auto& case_runner = SceneTestCaseRunner::Get();
    if (m_is_running || case_runner.HasActiveCase() || case_runner.HasPendingCase()) {
        LOG_WARNING("SceneTestSuite request ignored because another testcase or suite is already running.");
        return false;
    }

    const auto& all_case_ids = GetAllSceneTestCaseIds();
    if (all_case_ids.empty()) {
        LOG_WARNING("SceneTestSuite request ignored because there are no registered Scene testcases.");
        return false;
    }

    case_runner.ClearCompletedResult();

    m_is_running                       = true;
    m_stop_on_failure                  = stop_on_failure;
    m_renderable_stress_create_enabled = renderable_stress_create_enabled;
    m_has_saved_state_cache            = false;
    m_pending_ids                      = all_case_ids;
    m_results.clear();

    m_status                  = {};
    m_status.is_running       = true;
    m_status.total_case_count = static_cast<uint32>(m_pending_ids.size());
    m_status.current_case_name.clear();

    LOG_INFO("SceneTestSuite started: {} cases queued.", m_status.total_case_count);
    return true;
}

void SceneTestSuiteRunner::TickDispatch(Scene& scene) {
    auto& case_runner = SceneTestCaseRunner::Get();

    auto append_completed_result = [&](const SceneTestCaseRunResult& completed_result) {
        m_results.push_back(completed_result);

        m_status.completed_case_count = static_cast<uint32>(m_results.size());
        if (completed_result.passed) {
            m_status.passed_case_count += 1;
        } else {
            m_status.failed_case_count += 1;
            m_status.last_failed_case_name = completed_result.case_name;
            m_status.last_failed_summary   = completed_result.failure_summary;
        }
    };

    auto stop_suite = [&]() {
        m_is_running = false;
        m_pending_ids.clear();
        m_status.is_running = false;
        m_status.current_case_name.clear();
    };

    SceneTestCaseRunResult completed_result{};
    if (case_runner.ConsumeCompletedResult(completed_result)) {
        append_completed_result(completed_result);

        m_status.current_case_name.clear();

        if (m_is_running && !completed_result.passed && m_stop_on_failure) {
            if (m_has_saved_state_cache) {
                m_pending_ids.clear();
                m_pending_ids.push_back(ESceneTestCaseId::SuiteLoadStateCache);
                LOG_WARNING(
                    "SceneTestSuite stopped on failure: case='{}', summary='{}'. Restore step queued.",
                    completed_result.case_name,
                    completed_result.failure_summary
                );
            } else {
                stop_suite();
                LOG_WARNING(
                    "SceneTestSuite stopped on failure: case='{}', summary='{}'.",
                    completed_result.case_name,
                    completed_result.failure_summary
                );
                return;
            }
        }
    }

    if (!m_is_running) {
        return;
    }

    if (case_runner.HasActiveCase() || case_runner.HasPendingCase()) {
        return;
    }

    if (m_pending_ids.empty()) {
        m_is_running        = false;
        m_status.is_running = false;
        m_status.current_case_name.clear();
        LOG_INFO(
            "SceneTestSuite finished: {}/{} passed, {} failed.",
            m_status.passed_case_count,
            m_status.total_case_count,
            m_status.failed_case_count
        );
        return;
    }

    const ESceneTestCaseId next_case_id = m_pending_ids.front();
    m_pending_ids.erase(m_pending_ids.begin());

    if (IsSaveStateCacheSuiteStep(next_case_id)) {
        const bool save_result = scene.SaveStateCache();
        append_completed_result(BuildImmediateResult(
            next_case_id, save_result, save_result ? "" : "Scene::SaveStateCache returned false."
        ));
        m_has_saved_state_cache = save_result;

        if (!save_result && m_stop_on_failure) {
            stop_suite();
            LOG_WARNING("SceneTestSuite stopped because SuiteSaveStateCache failed.");
        }
        return;
    }

    if (IsLoadStateCacheSuiteStep(next_case_id)) {
        if (!m_has_saved_state_cache) {
            append_completed_result(
                BuildImmediateResult(next_case_id, false, "No saved suite state cache is available.")
            );

            if (m_stop_on_failure) {
                stop_suite();
            }
            return;
        }

        const std::filesystem::path source_file_path = scene.GetSourceFilePath();
        if (source_file_path.empty()) {
            append_completed_result(
                BuildImmediateResult(next_case_id, false, "Scene source file path is empty.")
            );

            if (m_stop_on_failure) {
                stop_suite();
            }
            return;
        }

        scene.LoadSceneFromFile(source_file_path);

        append_completed_result(BuildImmediateResult(
            next_case_id,
            scene.IsReady(),
            scene.IsReady() ? "" : "Scene::LoadSceneFromFile failed to restore the saved state cache."
        ));

        m_has_saved_state_cache = false;
        m_is_running            = false;
        m_status.is_running     = false;
        m_status.current_case_name.clear();

        LOG_INFO("SceneTestSuite restored the saved state cache via Scene::LoadSceneFromFile.");
        LOG_INFO(
            "SceneTestSuite finished: {}/{} passed, {} failed.",
            m_status.passed_case_count,
            m_status.total_case_count,
            m_status.failed_case_count
        );
        return;
    }

    SceneTestCaseRequest request{};
    request.test_case_id                     = next_case_id;
    request.renderable_stress_create_enabled = m_renderable_stress_create_enabled;

    auto test_case = CreateSceneTestCase(request);
    if (!test_case) {
        const auto failed_result = BuildImmediateResult(
            next_case_id,
            false,
            IsSuiteSpecialCase(next_case_id) ?
                "Suite special step should have been intercepted before factory dispatch." :
                "CreateSceneTestCase returned nullptr."
        );
        append_completed_result(failed_result);

        if (m_stop_on_failure) {
            if (m_has_saved_state_cache) {
                m_pending_ids.clear();
                m_pending_ids.push_back(ESceneTestCaseId::SuiteLoadStateCache);
            } else {
                stop_suite();
            }
        }
        return;
    }

    if (!case_runner.RequestCase(next_case_id, std::move(test_case))) {
        m_pending_ids.insert(m_pending_ids.begin(), next_case_id);
        return;
    }

    m_status.current_case_name = GetSceneTestCaseName(next_case_id);
}

bool SceneTestSuiteRunner::IsRunning() const {
    return m_is_running;
}

const SceneTestSuiteStatus& SceneTestSuiteRunner::GetStatus() const {
    return m_status;
}

} // namespace Moer