/**
 * This file is all written by AI.
 */
#include "RasterTestCase.h"

#include "log/LogSystem.h"
#include "math/Function.h"
#include "scene/LogicalComponents.h"
#include "scene/SceneCreateInfo.h"
#include "scene/Scene.h"

#include <cmath>
#include <entt/entity/entity.hpp>
#include <unordered_set>

namespace Moer::Render::Raster {

namespace {

struct DebugMaterialPreset {
    float4           albedo_factor;
    float3           emissive_factor;
    float            roughness_factor;
    float            metallic_factor;
    std::string_view name;
};

struct TransformMotionState {
    float3 base_translation;
    float3 direction;
    float  amplitude = 0.f;
    float  phase     = 0.f;
};

using TransformMotionStateMap = UnorderedMap<entt::entity, TransformMotionState>;

DebugMaterialPreset GetDebugMaterialPreset(uint preset_index) {
    switch (preset_index % 4) {
        case 0:
            return {float4(1.f, 0.05f, 0.05f, 1.f), float3(0.2f, 0.f, 0.f), 0.15f, 0.0f, "Red Highlight"};
        case 1:
            return {float4(0.05f, 1.f, 0.05f, 1.f), float3(0.f, 0.2f, 0.f), 0.15f, 0.0f, "Green Highlight"};
        case 2:
            return {float4(0.05f, 0.2f, 1.f, 1.f), float3(0.f, 0.f, 0.2f), 0.15f, 0.0f, "Blue Highlight"};
        default:
            return {float4(1.f, 1.f, 1.f, 1.f), float3(0.f, 0.f, 0.f), 1.0f, 0.0f, "Reset"};
    }
}

Array<entt::entity> FindPrimitiveReferencedMaterialEntities(Scene& scene) {
    auto& registry = scene.r();
    auto  view     = registry.view<const ecs::CPrimitive>();

    Array<entt::entity>              material_entities;
    std::unordered_set<entt::entity> visited_materials;

    for (entt::entity primitive_entity : view) {
        const auto& primitive        = registry.get<ecs::CPrimitive>(primitive_entity);
        const auto  material_entity  = primitive.material_entt;
        const bool  has_valid_target = material_entity != entt::null && registry.valid(material_entity) &&
                                       registry.all_of<ecs::CMaterial>(material_entity);
        if (has_valid_target && visited_materials.emplace(material_entity).second) {
            material_entities.emplace_back(material_entity);
        }
    }

    return material_entities;
}

uint& DebugMaterialPresetCursor() {
    static uint s_debug_material_preset_cursor = 0;
    return s_debug_material_preset_cursor;
}

uint HashEntity(entt::entity entity, uint salt) {
    uint value = static_cast<uint>(entt::to_integral(entity)) ^ salt;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float HashToUnitFloat(uint value) {
    return static_cast<float>(value & 0x00ffffffu) / static_cast<float>(0x00ffffffu);
}

float HashToRange(uint value, float min_value, float max_value) {
    return min_value + (max_value - min_value) * HashToUnitFloat(value);
}

float3 BuildMotionDirection(entt::entity entity, uint salt) {
    const float x = HashToRange(HashEntity(entity, salt + 1u), -1.f, 1.f);
    const float y = HashToRange(HashEntity(entity, salt + 2u), -1.f, 1.f);
    const float z = HashToRange(HashEntity(entity, salt + 3u), -1.f, 1.f);

    const float length = std::sqrt(x * x + y * y + z * z);
    if (length <= 1e-5f) {
        return float3(1.f, 0.f, 0.f);
    }
    return float3(x / length, y / length, z / length);
}

TransformMotionState CreateMotionState(
    entt::entity      entity,
    const ecs::CNode& node,
    uint              salt,
    float             min_amplitude,
    float             max_amplitude
) {
    return TransformMotionState{
        .base_translation = node.translation,
        .direction        = BuildMotionDirection(entity, salt),
        .amplitude        = HashToRange(HashEntity(entity, salt + 4u), min_amplitude, max_amplitude),
        .phase            = HashToRange(HashEntity(entity, salt + 5u), 0.f, 6.28318530718f),
    };
}

void RestoreTransformMotionStates(Scene& scene, TransformMotionStateMap& states) {
    auto& registry = scene.r();
    for (auto& [entity, state] : states) {
        if (!registry.valid(entity) || !registry.all_of<ecs::CNode>(entity)) {
            continue;
        }

        scene.Patch<ecs::CNode>(entity, [&](ecs::CNode& node) {
            node.translation = state.base_translation;
        });
    }
    states.clear();
}

template<typename ViewBuilder>
void ProcessTransformMotionGroup(
    Scene&                   scene,
    bool                     enabled,
    float                    elapsed_time_seconds,
    TransformMotionStateMap& states,
    ViewBuilder&&            build_view,
    uint                     salt,
    float                    min_amplitude,
    float                    max_amplitude
) {
    if (!enabled) {
        if (!states.empty()) {
            RestoreTransformMotionStates(scene, states);
        }
        return;
    }

    auto&           registry = scene.r();
    auto            view     = build_view(registry);
    constexpr float speed    = 1.25f;

    view.each([&](const auto entity, const auto&, const ecs::CNode& node) {
        auto [state_it, inserted] = states.try_emplace(entity);
        if (inserted) {
            state_it->second = CreateMotionState(entity, node, salt, min_amplitude, max_amplitude);
        }

        const TransformMotionState& state = state_it->second;
        const float offset = std::sin(elapsed_time_seconds * speed + state.phase) * state.amplitude;
        scene.Patch<ecs::CNode>(entity, [&](ecs::CNode& patched_node) {
            patched_node.translation = state.base_translation + state.direction * offset;
        });
    });
}

TransformMotionStateMap& RenderableMotionStates() {
    static TransformMotionStateMap s_states;
    return s_states;
}

TransformMotionStateMap& PointLightMotionStates() {
    static TransformMotionStateMap s_states;
    return s_states;
}

} // namespace

void RasterTestCase::ProcessDebugSceneUpdateRequest(RasterConfig& raster_config, Scene& scene) {
    if (!raster_config.debug_request_scene_update) {
        return;
    }

    raster_config.debug_request_scene_update = false;

    PointLightCreateInfo create_info{};
    create_info.position  = raster_config.debug_test_case_add_light_position;
    create_info.color     = raster_config.debug_test_case_add_light_color;
    create_info.intensity = 10000.f;
    create_info.name      = "TestCase Point Light";

    const entt::entity light_entity = scene.CreatePointLight(create_info);
    if (light_entity == entt::null) {
        return;
    }

    LOG_INFO(
        "TestCase Add Light created point light at ({}, {}, {}) with color ({}, {}, {}) and intensity {}.",
        create_info.position.x,
        create_info.position.y,
        create_info.position.z,
        create_info.color.x,
        create_info.color.y,
        create_info.color.z,
        create_info.intensity
    );
}

bool RasterTestCase::ProcessDebugMaterialRequest(RasterConfig& raster_config, Scene& scene) {
    if (!raster_config.debug_request_material_update) {
        return false;
    }

    raster_config.debug_request_material_update = false;

    const Array<entt::entity> material_entities = FindPrimitiveReferencedMaterialEntities(scene);
    if (material_entities.empty()) {
        LOG_WARNING("Patch Debug Material failed: no material referenced by any primitive was found.");
        return false;
    }

    const DebugMaterialPreset preset = GetDebugMaterialPreset(DebugMaterialPresetCursor());
    DebugMaterialPresetCursor()++;

    for (entt::entity material_entity : material_entities) {
        scene.Patch<ecs::CMaterial>(material_entity, [&](ecs::CMaterial& material) {
            material.albedo_map_entt  = entt::null;
            material.albedo_factor    = preset.albedo_factor;
            material.emissive_factor  = preset.emissive_factor;
            material.roughness_factor = preset.roughness_factor;
            material.metallic_factor  = preset.metallic_factor;
        });
    }

    LOG_INFO(
        "Patch Debug Material applied preset '{}' to {} materials.", preset.name, material_entities.size()
    );
    return true;
}

void RasterTestCase::ProcessRenderableTransformMotion(
    RasterConfig& raster_config,
    Scene&        scene,
    float         elapsed_time_seconds
) {
    ProcessTransformMotionGroup(
        scene,
        raster_config.debug_test_case_renderable_transform_motion_enabled,
        elapsed_time_seconds,
        RenderableMotionStates(),
        [](entt::registry& registry) {
            return registry.view<const ecs::CRenderable, const ecs::CNode>();
        },
        0x4d52524fu,
        0.08f,
        0.22f
    );
}

void RasterTestCase::ProcessPointLightTransformMotion(
    RasterConfig& raster_config,
    Scene&        scene,
    float         elapsed_time_seconds
) {
    ProcessTransformMotionGroup(
        scene,
        raster_config.debug_test_case_point_light_transform_motion_enabled,
        elapsed_time_seconds,
        PointLightMotionStates(),
        [](entt::registry& registry) {
            return registry.view<const ecs::CLightPoint, const ecs::CNode>();
        },
        0x504c4954u,
        0.35f,
        1.15f
    );
}

} // namespace Moer::Render::Raster
