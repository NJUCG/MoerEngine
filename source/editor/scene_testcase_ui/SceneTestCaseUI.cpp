#include "SceneTestCaseUI.h"

#include <imgui.h>

namespace Moer {

SceneTestCaseUI::SceneTestCaseUI(SceneTestCaseConfig& config) : m_config(config) {}

void SceneTestCaseUI::ShowWindow(bool* p_open) {
    if (!p_open || !*p_open) {
        return;
    }

    if (!ImGui::Begin("Scene TestCases", p_open)) {
        ImGui::End();
        return;
    }

    if (ImGui::TreeNode("Smoke Cases")) {
        if (ImGui::Button("SceneTestCase Noop")) {
            m_config.requested_test_case = ESceneTestCaseId::FrameworkNoop;
        }
        if (ImGui::Button("SceneTestCase Create Point Light")) {
            m_config.requested_test_case = ESceneTestCaseId::CreatePointLightOnce;
        }
        if (ImGui::Button("SceneTestCase Patch Point Light Transform")) {
            m_config.requested_test_case = ESceneTestCaseId::PatchCreatedPointLightTransform;
        }
        if (ImGui::Button("SceneTestCase Create/Destroy Point Light")) {
            m_config.requested_test_case = ESceneTestCaseId::CreateDestroyPointLight;
        }
        if (ImGui::Button("SceneTestCase EntityWithNode Flow")) {
            m_config.requested_test_case = ESceneTestCaseId::EntityWithNodeStructuralFlow;
        }
        if (ImGui::Button("SceneTestCase EntityWithNode Invalid Ops")) {
            m_config.requested_test_case = ESceneTestCaseId::EntityWithNodeRejectInvalidOps;
        }

        ImGui::Checkbox(
            "SceneTestCase Renderable Stress Create (5x5-1)", &m_config.renderable_stress_create_enabled
        );
        if (ImGui::Button("SceneTestCase Create/Destroy Renderable")) {
            m_config.requested_test_case = ESceneTestCaseId::CreateDestroyRenderable;
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Scene Actions")) {
        ImGui::SliderFloat3("TestCase Add Light Position", (float*)&m_config.add_light_position, -2.0f, 2.0f);
        ImGui::ColorEdit3("TestCase Add Light Color", (float*)&m_config.add_light_color);

        if (ImGui::Button("TestCase Add Light")) {
            m_config.requested_test_case = ESceneTestCaseId::DebugAddPointLight;
        }

        if (ImGui::Button("TestCase Modify Material")) {
            m_config.requested_test_case = ESceneTestCaseId::DebugModifyMaterial;
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Scene Motion")) {
        ImGui::Checkbox("TestCase Move Renderables", &m_config.move_renderables_enabled);
        ImGui::Checkbox("TestCase Move Point Lights", &m_config.move_point_lights_enabled);
        ImGui::TreePop();
    }

    ImGui::End();
}

} // namespace Moer
