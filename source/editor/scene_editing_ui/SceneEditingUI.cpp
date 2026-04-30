#include "SceneEditingUI.h"

#include "scene/SceneGlobalEntry.h"
#include "scene/editing/SceneEditing.h"

#include <imgui.h>

namespace Moer {

SceneEditingUI::SceneEditingUI(SceneTestCaseConfig& test_case_config) :
    m_test_case_config(test_case_config) {}

void SceneEditingUI::ShowWindow(bool* p_open) {
    if (!p_open || !*p_open) {
        return;
    }

    Scene* scene = SceneGlobalEntry::Get().PeekScene();

    if (!ImGui::Begin("Scene Editing", p_open)) {
        ImGui::End();
        return;
    }

    if (scene == nullptr) {
        ImGui::TextDisabled("scene加载中");
        ImGui::End();
        return;
    }

    if (ImGui::TreeNode("Editing")) {
        if (ImGui::TreeNode("Lighting")) {
            float3 main_directional_light_direction = float3(0.f, 0.f, -1.f);
            if (SceneEditing::TryGetMainDirectionalLightDirection(*scene, main_directional_light_direction)) {
                if (ImGui::SliderFloat3(
                        "MainDirectionalLight Direction",
                        (float*)&main_directional_light_direction,
                        -1.0f,
                        1.0f
                    )) {
                    SceneEditing::SetMainDirectionalLightDirection(*scene, main_directional_light_direction);
                }
            } else {
                ImGui::TextDisabled("MainDirectionalLight not found.");
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Smoke Cases")) {
        if (ImGui::Button("SceneTestCase Noop")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::FrameworkNoop;
        }
        if (ImGui::Button("SceneTestCase Create Point Light")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::CreatePointLightOnce;
        }
        if (ImGui::Button("SceneTestCase Patch Point Light Transform")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::PatchCreatedPointLightTransform;
        }
        if (ImGui::Button("SceneTestCase Create/Destroy Point Light")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::CreateDestroyPointLight;
        }
        if (ImGui::Button("SceneTestCase EntityWithNode Flow")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::EntityWithNodeStructuralFlow;
        }
        if (ImGui::Button("SceneTestCase EntityWithNode Invalid Ops")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::EntityWithNodeRejectInvalidOps;
        }

        ImGui::Checkbox(
            "SceneTestCase Renderable Stress Create (5x5-1)",
            &m_test_case_config.renderable_stress_create_enabled
        );
        if (ImGui::Button("SceneTestCase Create/Destroy Renderable")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::CreateDestroyRenderable;
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Scene Actions")) {
        ImGui::SliderFloat3(
            "TestCase Add Light Position", (float*)&m_test_case_config.add_light_position, -2.0f, 2.0f
        );
        ImGui::ColorEdit3("TestCase Add Light Color", (float*)&m_test_case_config.add_light_color);

        if (ImGui::Button("TestCase Add Light")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::DebugAddPointLight;
        }

        if (ImGui::Button("TestCase Modify Material")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::DebugModifyMaterial;
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Scene Motion")) {
        ImGui::Checkbox("TestCase Move Renderables", &m_test_case_config.move_renderables_enabled);
        ImGui::Checkbox("TestCase Move Point Lights", &m_test_case_config.move_point_lights_enabled);
        ImGui::TreePop();
    }

    ImGui::End();
}

} // namespace Moer