#include "SceneEditingUI.h"

#include "SceneFileDialog.h"

#include "scene/Scene.h"
#include "scene/editing/SceneEditing.h"
#include "scene/testcase/SceneTestSuiteRunner.h"

#include <imgui.h>

#include <string>

namespace Moer {

static EProceduralPrimitiveShape GetProceduralShapeFromIndex(int shape_index) {
    switch (shape_index) {
        case 1:
            return EProceduralPrimitiveShape::FacetedSphere;
        case 0:
        default:
            return EProceduralPrimitiveShape::Cube;
    }
}

static std::string_view GetProceduralShapeName(EProceduralPrimitiveShape shape) {
    switch (shape) {
        case EProceduralPrimitiveShape::Cube:
            return "Runtime UI Cube";
        case EProceduralPrimitiveShape::FacetedSphere:
            return "Runtime UI FacetedSphere";
    }
    return "Runtime UI Procedural Renderable";
}

SceneEditingUI::SceneEditingUI(SceneTestCaseConfig& test_case_config, bool& out_need_reload) :
    m_test_case_config(test_case_config),
    m_need_reload(out_need_reload) {}

void SceneEditingUI::ShowWindow(bool* p_open, Scene& scene) {
    if (!p_open || !*p_open) {
        return;
    }

    if (!ImGui::Begin("Scene Editing", p_open)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Save Cache")) {
        if (scene.SaveStateCache()) {
            m_last_scene_action_status = "State cache saved";
        } else {
            m_last_scene_action_status = "Save State failed. Check log.";
        }
    }

    ImGui::BeginDisabled(!scene.IsReady() || scene.GetSourceFilePath().empty());
    if (ImGui::Button("Load Cache")) {
        m_need_reload              = true;
        m_last_scene_action_status = "Load Cache requested";
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!scene.IsReady() || scene.GetSourceFilePath().empty());
    if (ImGui::Button("Reset Cache")) {
        if (scene.ResetToSourceScene()) {
            m_need_reload              = true;
            m_last_scene_action_status = "Reset Cache requested";
        } else {
            m_last_scene_action_status = "Reset failed. Check log.";
        }
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!scene.IsReady());
    if (ImGui::Button("Import Into Current Scene")) {
        std::string selected_path;
        if (OpenSceneFileDialog(selected_path) == ESceneFileDialogResult::Selected) {
            const Scene::ImportSceneFromFileResult result = scene.ImportSceneFromFileSync(selected_path);
            if (result) {
                m_last_scene_action_status =
                    "Imported scene entities: " + std::to_string(result.imported_entity_count);
            } else {
                m_last_scene_action_status = "Import failed: " + result.error_message;
            }
        }
    }
    ImGui::EndDisabled();
    if (!m_last_scene_action_status.empty()) {
        ImGui::TextDisabled("%s", m_last_scene_action_status.c_str());
    }
    ImGui::Separator();

    if (ImGui::TreeNode("Editing")) {
        if (ImGui::TreeNode("Lighting")) {
            float3 main_directional_light_direction = float3(0.f, 0.f, -1.f);
            if (SceneEditing::TryGetMainDirectionalLightDirection(scene, main_directional_light_direction)) {
                if (ImGui::SliderFloat3(
                        "MainDirectionalLight Direction",
                        (float*)&main_directional_light_direction,
                        -1.0f,
                        1.0f
                    )) {
                    SceneEditing::SetMainDirectionalLightDirection(scene, main_directional_light_direction);
                }
            } else {
                ImGui::TextDisabled("MainDirectionalLight not found.");
            }

            ImGui::Separator();
            ImGui::SliderFloat3("PointLight Position", (float*)&m_add_point_light_position, -20.f, 20.f);
            ImGui::ColorEdit3("PointLight Color", (float*)&m_add_point_light_color);
            ImGui::SliderFloat(
                "PointLight Intensity",
                &m_add_point_light_intensity,
                1.f,
                100000.f,
                "%.0f",
                ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat
            );
            if (ImGui::Button("Add Point Light")) {
                if (SceneEditing::AddPointLight(
                        scene,
                        m_add_point_light_position,
                        m_add_point_light_color,
                        m_add_point_light_intensity
                    )) {
                    m_last_light_status = "Point light created";
                } else {
                    m_last_light_status = "Create point light failed";
                }
            }
            if (!m_last_light_status.empty()) {
                ImGui::TextDisabled("%s", m_last_light_status.c_str());
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Primitive")) {
            ImGui::Combo("Shape", &m_procedural_shape_index, "Cube\0Faceted Sphere\0");
            ImGui::SliderFloat3("Position", (float*)&m_create_translation, -20.f, 20.f);
            ImGui::SliderFloat3("Scale", (float*)&m_create_scale, 0.1f, 10.f);
            ImGui::ColorEdit3("Albedo", (float*)&m_create_albedo);
            ImGui::SliderFloat("Roughness", &m_create_roughness, 0.f, 1.f);
            ImGui::SliderFloat("Metallic", &m_create_metallic, 0.f, 1.f);

            if (ImGui::Button("Create Procedural Renderable")) {
                const EProceduralPrimitiveShape shape = GetProceduralShapeFromIndex(m_procedural_shape_index);

                ProceduralMeshCreateInfo create_info{};
                create_info.shape       = shape;
                create_info.name        = GetProceduralShapeName(shape);
                create_info.translation = m_create_translation;
                create_info.scale       = m_create_scale;

                create_info.material.name = "Runtime UI Procedural Material";
                create_info.material.albedo_factor =
                    float4(m_create_albedo.x, m_create_albedo.y, m_create_albedo.z, 1.f);
                create_info.material.roughness_factor = m_create_roughness;
                create_info.material.metallic_factor  = m_create_metallic;

                const CreateProceduralRenderableResult result = scene.CreateProceduralRenderable(create_info);
                if (result) {
                    m_last_create_status = "Created renderable entity " +
                                           std::to_string(entt::to_integral(result.renderable_entt));
                } else {
                    m_last_create_status = "Create failed";
                }
            }

            if (!m_last_create_status.empty()) {
                ImGui::TextDisabled("%s", m_last_create_status.c_str());
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Test Cases")) {
        const SceneTestSuiteStatus& suite_status = SceneTestSuiteRunner::Get().GetStatus();

        ImGui::BeginDisabled(suite_status.is_running);
        if (ImGui::Button("Run All Scene Tests")) {
            m_test_case_config.request_run_all_scene_tests = true;
        }
        ImGui::EndDisabled();

        if (suite_status.total_case_count > 0) {
            ImGui::TextDisabled(
                "Suite Progress: %u / %u | Passed: %u | Failed: %u",
                suite_status.completed_case_count,
                suite_status.total_case_count,
                suite_status.passed_case_count,
                suite_status.failed_case_count
            );
            if (!suite_status.current_case_name.empty()) {
                ImGui::TextDisabled("Current Case: %s", suite_status.current_case_name.c_str());
            }
            if (!suite_status.last_failed_case_name.empty()) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                    "Last Failed: %s",
                    suite_status.last_failed_case_name.c_str()
                );
                if (!suite_status.last_failed_summary.empty()) {
                    ImGui::TextWrapped("Reason: %s", suite_status.last_failed_summary.c_str());
                }
            }
            ImGui::Separator();
        }

        ImGui::BeginDisabled(suite_status.is_running);
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
        if (ImGui::Button("SceneTestCase Create Procedural Renderable")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::CreateProceduralRenderable;
        }
        if (ImGui::Button("TestCase Modify Material")) {
            m_test_case_config.requested_test_case = ESceneTestCaseId::DebugModifyMaterial;
        }
        ImGui::EndDisabled();

        ImGui::Checkbox("TestCase Move Point Lights", &m_test_case_config.move_point_lights_enabled);
        ImGui::TreePop();
    }

    ImGui::End();
}

} // namespace Moer