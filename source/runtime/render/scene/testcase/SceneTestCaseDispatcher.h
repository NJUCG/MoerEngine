/**
 * Processes editor Scene testcase requests and long-running scene debug actions.
 */
#pragma once

#include "scene/testcase/SceneTestCaseConfig.h"

namespace Moer {

class Scene;

void ProcessSceneTestCaseRequests(
    SceneTestCaseConfig& scene_test_case_config,
    Scene&               scene,
    float                elapsed_time_seconds
);

} // namespace Moer
