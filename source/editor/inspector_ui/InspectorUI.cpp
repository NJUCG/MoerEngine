#include "InspectorUI.h"

// Edits the selected scene node through Scene's public query and mutation API.

#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace Moer {

namespace {

constexpr float k_pi = 3.14159265358979323846f;

float DegreesToRadians(float degrees) {
    return degrees * (k_pi / 180.0f);
}

float RadiansToDegrees(float radians) {
    return radians * (180.0f / k_pi);
}

float WrapDegrees(float degrees) {
    float wrapped = std::fmod(degrees + 180.0f, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    return wrapped - 180.0f;
}

float3 WrapEulerDegrees(const float3& euler_degrees) {
    return {
        WrapDegrees(euler_degrees.x),
        WrapDegrees(euler_degrees.y),
        WrapDegrees(euler_degrees.z),
    };
}

Quaternion QuaternionFromEulerDegrees(const float3& euler_degrees) {
    const float3 euler_radians = {
        DegreesToRadians(euler_degrees.x),
        DegreesToRadians(euler_degrees.y),
        DegreesToRadians(euler_degrees.z),
    };

    const float half_x = euler_radians.x * 0.5f;
    const float half_y = euler_radians.y * 0.5f;
    const float half_z = euler_radians.z * 0.5f;

    const float cx = std::cos(half_x);
    const float sx = std::sin(half_x);
    const float cy = std::cos(half_y);
    const float sy = std::sin(half_y);
    const float cz = std::cos(half_z);
    const float sz = std::sin(half_z);

    return Quaternion(
        cx * cy * cz + sx * sy * sz,
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz
    );
}

float3 EulerDegreesFromQuaternion(const Quaternion& quaternion) {
    const float x = quaternion.vec.x;
    const float y = quaternion.vec.y;
    const float z = quaternion.vec.z;
    const float w = quaternion.vec.w;

    const float sin_x_cos_y = 2.0f * (w * x + y * z);
    const float cos_x_cos_y = 1.0f - 2.0f * (x * x + y * y);
    const float euler_x     = std::atan2(sin_x_cos_y, cos_x_cos_y);

    const float sin_y   = std::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f);
    const float euler_y = std::asin(sin_y);

    const float sin_z_cos_y = 2.0f * (w * z + x * y);
    const float cos_z_cos_y = 1.0f - 2.0f * (y * y + z * z);
    const float euler_z     = std::atan2(sin_z_cos_y, cos_z_cos_y);

    return WrapEulerDegrees({
        RadiansToDegrees(euler_x),
        RadiansToDegrees(euler_y),
        RadiansToDegrees(euler_z),
    });
}

} // namespace

void InspectorUI::ShowWindow(bool* p_open, Scene* scene, entt::entity& selected_node) {
    if (!p_open || !*p_open) {
        return;
    }

    if (!ImGui::Begin("Inspector", p_open)) {
        ImGui::End();
        return;
    }

    if (scene == nullptr || !scene->IsReady()) {
        ImGui::TextDisabled("scene加载中");
        ImGui::End();
        return;
    }

    if (!scene->IsValidNodeEntity(selected_node)) {
        selected_node           = entt::null;
        m_rotation_cache_entity = entt::null;
    }

    if (selected_node == entt::null) {
        ImGui::TextDisabled("No node selected.");
        ImGui::End();
        return;
    }

    std::string node_name;
    const auto  local_transform = scene->TryGetNodeLocalTransform(selected_node);
    if (!scene->TryGetNodeName(selected_node, node_name) || !local_transform.has_value()) {
        selected_node           = entt::null;
        m_rotation_cache_entity = entt::null;
        ImGui::TextDisabled("No node selected.");
        ImGui::End();
        return;
    }

    const std::string node_display_name = scene->GetNodeDisplayName(selected_node);

    if (m_rotation_cache_entity != selected_node) {
        m_rotation_cache_entity = selected_node;
        m_rotation_euler        = EulerDegreesFromQuaternion(local_transform->rotation);
    }

    std::array<char, 256> name_buffer{};
    std::snprintf(name_buffer.data(), name_buffer.size(), "%s", node_name.c_str());

    ImGui::TextDisabled("Entity %u", static_cast<uint32>(entt::to_integral(selected_node)));

    Scene::NodeVisibility visibility      = scene->GetNodeVisibility(selected_node);
    bool                  visible_in_game = visibility.visible_in_game;
    if (ImGui::Checkbox("Visible in Game", &visible_in_game)) {
        scene->SetNodeVisibleInGame(selected_node, visible_in_game);
        visibility = scene->GetNodeVisibility(selected_node);
    }
    bool visible_in_editor = visibility.visible_in_editor;
    if (ImGui::Checkbox("Visible in Editor", &visible_in_editor)) {
        scene->SetNodeVisibleInEditor(selected_node, visible_in_editor);
        visibility = scene->GetNodeVisibility(selected_node);
    }
    if (!visibility.effectively_visible_in_game) {
        ImGui::TextDisabled("Game visibility is blocked by this node or a parent.");
    }

    if (ImGui::InputText("Name", name_buffer.data(), name_buffer.size())) {
        scene->SetNodeName(selected_node, name_buffer.data());
    }

    float3 translation = local_transform->translation;
    if (ImGui::DragFloat3("Position", (float*)&translation, 0.05f)) {
        scene->SetNodeTranslation(selected_node, translation);
    }

    if (ImGui::DragFloat3("Rotation", (float*)&m_rotation_euler, 0.2f)) {
        m_rotation_euler = WrapEulerDegrees(m_rotation_euler);
        scene->SetNodeRotation(selected_node, QuaternionFromEulerDegrees(m_rotation_euler));
    }

    float3 scale = local_transform->scale;
    if (ImGui::DragFloat3("Scale", (float*)&scale, 0.05f)) {
        scene->SetNodeScale(selected_node, scale);
    }

    ImGui::Separator();

    const Scene::NodeSubtreeStats subtree_stats = scene->GetNodeSubtreeStats(selected_node);

    ImGui::TextDisabled(
        "Subtree: %u nodes, %u renderables, %u cameras, %u lights",
        subtree_stats.node_count,
        subtree_stats.renderable_count,
        subtree_stats.camera_count,
        subtree_stats.light_count
    );
    if (subtree_stats.contains_main_camera) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "This subtree contains the main camera.");
    }
    if (subtree_stats.contains_main_light_tag) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "This subtree contains the main light tag.");
    }

    const bool can_delete_selected_node = !scene->IsRootNode(selected_node);
    if (!can_delete_selected_node) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Delete Node + Children...")) {
        ImGui::OpenPopup("Confirm Delete Node Subtree");
    }

    if (!can_delete_selected_node) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("Root node cannot be deleted.");
    }

    if (ImGui::BeginPopupModal("Confirm Delete Node Subtree", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete '%s'?", node_display_name.c_str());
        ImGui::TextWrapped("This will remove the selected node and all of its descendants from the scene.");
        ImGui::Separator();
        ImGui::Text(
            "Affected: %u nodes, %u renderables, %u cameras, %u lights",
            subtree_stats.node_count,
            subtree_stats.renderable_count,
            subtree_stats.camera_count,
            subtree_stats.light_count
        );
        if (subtree_stats.contains_main_camera) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                "The current main camera is inside this subtree. A default camera will be restored if needed."
            );
        }
        if (subtree_stats.contains_main_light_tag) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                "The current main light is inside this subtree. A default light will be restored if needed."
            );
        }
        ImGui::Separator();

        if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) {
            if (scene->DestroyNodeSubtree(selected_node)) {
                selected_node           = entt::null;
                m_rotation_cache_entity = entt::null;
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace Moer
