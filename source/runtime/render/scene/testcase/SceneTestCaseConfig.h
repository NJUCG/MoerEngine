/**
 * Defines editor-driven Scene testcase and scene debug action state.
 */
#pragma once

#include "misc/Traits.h"
#include "scene/testcase/SceneTestCaseId.h"

namespace Moer {

struct SceneTestCaseConfig {
    ESceneTestCaseId requested_test_case = ESceneTestCaseId::None;

    bool renderable_stress_create_enabled = false;

    bool move_renderables_enabled  = false;
    bool move_point_lights_enabled = false;

    float3 add_light_position = float3(0.f, 2.f, 0.f);
    float3 add_light_color    = float3(1.f, 0.2f, 0.05f);
};

} // namespace Moer
