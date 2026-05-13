#pragma once

#include "misc/Traits.h"
#include "scene/testcase/SceneTestCaseConfig.h"

#include <string>

namespace Moer {

class Scene;

/**
 * SceneEditingUI 负责 scene 编辑相关窗口内容。
 *
 * 结构:
 * - Editing：常用正式编辑入口，如 Lighting / Primitive
 * - Test & Debug：Scene testcase 与长期 debug switch
 *
 * 边界约束:
 * - 只通过 Scene 正式 API、SceneEditing helper 和 SceneTestCaseConfig 驱动行为
 * - 不直接访问 registry / LogicalScene；缺能力时优先补 Scene / SceneEditing 接口
 */
class SceneEditingUI {
public:
    explicit SceneEditingUI(SceneTestCaseConfig& test_case_config);

    void ShowWindow(bool* p_open, Scene& scene);

private:
    void ShowEditingSection(Scene& scene);
    void ShowLightingSection(Scene& scene);
    void ShowPrimitiveSection(Scene& scene);
    void ShowTestAndDebugSection();

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