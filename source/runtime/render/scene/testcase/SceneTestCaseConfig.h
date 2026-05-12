/**
 * Defines editor-driven Scene testcase and scene debug action state.
 */
#pragma once

#include "scene/testcase/SceneTestCaseId.h"

namespace Moer {

struct SceneTestCaseConfig {
    ESceneTestCaseId requested_test_case = ESceneTestCaseId::None;

    bool request_run_all_scene_tests = false;

    bool renderable_stress_create_enabled = false;

    bool move_point_lights_enabled = false;
};

} // namespace Moer
