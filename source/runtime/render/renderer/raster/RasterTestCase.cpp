#include "RasterTestCase.h"

#include "log/LogSystem.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"
#include "scene/SceneLightApi.h"

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

} // namespace Moer::Render::Raster