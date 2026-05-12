/**
 * Defines a minimal run-all runner that queues Scene testcases on top of SceneTestCaseRunner.
 */
#pragma once

#include "RenderAPI.h"
#include "misc/STL.h"
#include "scene/testcase/SceneTestCase.h"
#include "scene/testcase/SceneTestCaseId.h"

#include <string>

namespace Moer {

class Scene;

struct SceneTestSuiteStatus {
    bool        is_running           = false;
    uint32      total_case_count     = 0;
    uint32      completed_case_count = 0;
    uint32      passed_case_count    = 0;
    uint32      failed_case_count    = 0;
    std::string current_case_name;
    std::string last_failed_case_name;
    std::string last_failed_summary;
};

class RENDER_API SceneTestSuiteRunner {
public:
    static SceneTestSuiteRunner& Get();

    SceneTestSuiteRunner(const SceneTestSuiteRunner&)            = delete;
    SceneTestSuiteRunner& operator=(const SceneTestSuiteRunner&) = delete;

    bool RequestRunAll(bool renderable_stress_create_enabled, bool stop_on_failure = true);
    void TickDispatch(Scene& scene);

    bool                        IsRunning() const;
    const SceneTestSuiteStatus& GetStatus() const;

private:
    SceneTestSuiteRunner() = default;

private:
    bool                          m_is_running                       = false;
    bool                          m_stop_on_failure                  = true;
    bool                          m_renderable_stress_create_enabled = false;
    bool                          m_has_saved_state_cache            = false;
    Array<ESceneTestCaseId>       m_pending_ids;
    Array<SceneTestCaseRunResult> m_results;
    SceneTestSuiteStatus          m_status;
};

} // namespace Moer