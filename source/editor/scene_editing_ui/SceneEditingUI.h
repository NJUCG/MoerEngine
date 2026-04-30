#pragma once

#include "misc/Traits.h"
#include "scene/testcase/SceneTestCaseConfig.h"

#include <string>

namespace Moer {

class SceneEditingUI {
public:
    explicit SceneEditingUI(SceneTestCaseConfig& test_case_config);

    void ShowWindow(bool* p_open);

private:
    SceneTestCaseConfig& m_test_case_config;

    int    m_procedural_shape_index = 0;
    float3 m_create_translation     = float3(0.f, 0.f, 0.f);
    float3 m_create_scale           = float3(1.f, 1.f, 1.f);
    float3 m_create_albedo          = float3(0.8f, 0.4f, 0.2f);
    float  m_create_roughness       = 0.6f;
    float  m_create_metallic        = 0.f;

    float3 m_add_point_light_position  = float3(0.f, 2.f, 0.f);
    float3 m_add_point_light_color     = float3(1.f, 0.2f, 0.05f);
    float  m_add_point_light_intensity = 10000.f;

    std::string m_last_create_status;
    std::string m_last_light_status;
};

} // namespace Moer