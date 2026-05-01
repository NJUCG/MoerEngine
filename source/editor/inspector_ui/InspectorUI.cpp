#include "InspectorUI.h"

#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

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

bool IsSelectedNodeValid(Scene* scene, entt::entity entity) {
    return scene != nullptr && entity != entt::null && scene->r().valid(entity) &&
           scene->r().all_of<ecs::CNode>(entity);
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

    if (!IsSelectedNodeValid(scene, selected_node)) {
        selected_node          = entt::null;
        m_rotation_cache_entity = entt::null;
    }

    if (selected_node == entt::null) {
        ImGui::TextDisabled("No node selected.");
        ImGui::End();
        return;
    }

    const auto& node = scene->r().get<ecs::CNode>(selected_node);
    if (m_rotation_cache_entity != selected_node) {
        m_rotation_cache_entity = selected_node;
        m_rotation_euler        = EulerDegreesFromQuaternion(node.rotation);
    }

    std::array<char, 256> name_buffer{};
    std::snprintf(name_buffer.data(), name_buffer.size(), "%s", node.name.c_str());

    ImGui::TextDisabled("Entity %u", static_cast<uint32>(entt::to_integral(selected_node)));

    if (ImGui::InputText("Name", name_buffer.data(), name_buffer.size())) {
        scene->Patch<ecs::CNode>(selected_node, [&](auto& mutable_node) {
            mutable_node.name = name_buffer.data();
        });
    }

    float3 translation = node.translation;
    if (ImGui::DragFloat3("Position", (float*)&translation, 0.05f)) {
        scene->Patch<ecs::CNode>(selected_node, [&](auto& mutable_node) {
            mutable_node.translation = translation;
        });
    }

    if (ImGui::DragFloat3("Rotation", (float*)&m_rotation_euler, 0.2f)) {
        m_rotation_euler = WrapEulerDegrees(m_rotation_euler);
        scene->Patch<ecs::CNode>(selected_node, [&](auto& mutable_node) {
            mutable_node.rotation = QuaternionFromEulerDegrees(m_rotation_euler);
        });
    }

    float3 scale = node.scale;
    if (ImGui::DragFloat3("Scale", (float*)&scale, 0.05f)) {
        scene->Patch<ecs::CNode>(selected_node, [&](auto& mutable_node) {
            mutable_node.scale = scale;
        });
    }

    ImGui::End();
}

} // namespace Moer