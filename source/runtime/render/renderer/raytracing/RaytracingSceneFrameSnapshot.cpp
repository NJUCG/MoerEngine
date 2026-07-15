#include "RaytracingSceneFrameSnapshot.h"

#include "log/LogSystem.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace Moer::Render::Raytracing {

namespace {

uint LightPriority(entt::entity entity, const Scene& scene) {
    const auto& registry = scene.r();
    if (!registry.valid(entity) || !registry.all_of<ecs::CLight>(entity)) {
        return 0;
    }

    switch (registry.get<ecs::CLight>(entity).type) {
        case ELightType::Directional:
            return 1;
        case ELightType::Environment:
            return 2;
        default:
            return 0;
    }
}

uint FloatToUInt(float value, float scale) {
    return static_cast<uint>(std::floor(value * scale + 0.5f));
}

float Saturate(float value) {
    return std::clamp(value, 0.f, 1.f);
}

uint Float3ToR8G8B8Unorm(const float3& value) {
    return (FloatToUInt(Saturate(value.x), 255.f) & 0xffu) |
           ((FloatToUInt(Saturate(value.y), 255.f) & 0xffu) << 8) |
           ((FloatToUInt(Saturate(value.z), 255.f) & 0xffu) << 16);
}

uint16_t Fp32ToFp16(float value) {
    static const union FloatBits {
        uint  ui;
        float f;
    } multiplier = {0x07800000};

    FloatBits biased_float{};
    biased_float.f  = value * multiplier.f;
    const uint bits = biased_float.ui;

    const uint sign = bits & 0x80000000;
    const uint body = bits & 0x0fffffff;
    return static_cast<uint16_t>((sign >> 16 | body >> 13) & 0xffffu);
}

void PackPolyLightColor(const float3& color, PolymorphicLightInfo& info) {
    const float max_radiance = Max(Max(color.x, color.y), color.z);
    if (max_radiance <= 0.f) {
        return;
    }

    const float log_radiance = std::clamp(
        (std::log2f(max_radiance) - g_poly_morphic_light_min_log2_radiance) /
            (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance),
        0.f,
        1.f
    );
    const uint  packed_radiance = std::min(static_cast<uint>(std::ceil(log_radiance * 65534.f)) + 1, 0xffffu);
    const float unpacked_radiance = std::exp2f(
        (static_cast<float>(packed_radiance - 1) / 65534.f) *
            (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance) +
        g_poly_morphic_light_min_log2_radiance
    );

    info.color_type_flags |= Float3ToR8G8B8Unorm(color / unpacked_radiance);
    info.log_radiance = packed_radiance;
}

float2 UnitVectorToOctahedron(const float3& value) {
    const float divisor = std::abs(value.x) + std::abs(value.y) + std::abs(value.z);
    float2      result  = {value.x / divisor, value.y / divisor};
    if (value.z <= 0.f) {
        const float2 signs = {result.x >= 0.f ? 1.f : -1.f, result.y >= 0.f ? 1.f : -1.f};
        const float  x     = (1.f - std::abs(result.y)) * signs.x;
        const float  y     = (1.f - std::abs(result.x)) * signs.y;
        result.x           = x;
        result.y           = y;
    }
    return result;
}

uint PackNormalizedVector(const float3& value) {
    float2 octahedron = UnitVectorToOctahedron(value);
    octahedron.x      = octahedron.x * 0.5f + 0.5f;
    octahedron.y      = octahedron.y * 0.5f + 0.5f;
    const uint x      = FloatToUInt(Saturate(octahedron.x), 65535.f);
    const uint y      = FloatToUInt(Saturate(octahedron.y), 65535.f);
    return x | (y << 16);
}

bool CanConvert(entt::entity entity, const Scene& scene) {
    const auto& registry = scene.r();
    if (!registry.valid(entity) || !registry.all_of<ecs::CLight>(entity)) {
        return false;
    }

    switch (registry.get<ecs::CLight>(entity).type) {
        case ELightType::Directional:
            return registry.all_of<ecs::CLightDirectional>(entity);
        case ELightType::Point:
            return registry.all_of<ecs::CLightPoint>(entity);
        case ELightType::Spot:
        case ELightType::Environment:
        default:
            return false;
    }
}

bool ConvertLight(entt::entity entity, const Scene& scene, PolymorphicLightInfo& info) {
    if (!CanConvert(entity, scene)) {
        return false;
    }

    const auto& registry = scene.r();
    const auto& light    = registry.get<ecs::CLight>(entity);
    switch (light.type) {
        case ELightType::Directional: {
            const auto& directional           = registry.get<ecs::CLightDirectional>(entity);
            const float half_angular_size_rad = Angle::DegreeToRadian(0.533f);
            const float solid_angle           = 2 * PI * (1 - std::cos(half_angular_size_rad));

            float3      radiance = directional.color * directional.intensity / std::max(solid_angle, 1e-6f);
            const float max_radiance = Max(Max(radiance.x, radiance.y), radiance.z);
            if (max_radiance > g_poly_morphic_light_max_radiance) {
                radiance = radiance / max_radiance * g_poly_morphic_light_max_radiance;
            }

            info.color_type_flags = static_cast<uint>(EPolyLightType::ELDirectional)
                                    << g_poly_morphic_light_type_shift;
            PackPolyLightColor(radiance, info);
            info.direction1 = PackNormalizedVector(Normalizef(directional.d_direction));
            info.scalars    = Fp32ToFp16(half_angular_size_rad) | (Fp32ToFp16(solid_angle) << 16);
            return true;
        }
        case ELightType::Point: {
            const auto& point     = registry.get<ecs::CLightPoint>(entity);
            float3      flux      = point.color * point.intensity;
            info.color_type_flags = static_cast<uint>(EPolyLightType::ELPoint)
                                    << g_poly_morphic_light_type_shift;

            const float max_flux = Max(Max(flux.x, flux.y), flux.z);
            if (max_flux > g_poly_morphic_light_max_flux) {
                flux = flux / max_flux * g_poly_morphic_light_max_flux;
            }

            PackPolyLightColor(flux, info);
            info.center = point.d_position;
            return true;
        }
        case ELightType::Spot:
        case ELightType::Environment:
        default:
            return false;
    }
}

} // namespace

RaytracingSceneFrameSnapshot CaptureRaytracingSceneFrameSnapshot(const Scene& scene) {
    RaytracingSceneFrameSnapshot snapshot{};
    const auto&                  registry  = scene.r();
    const auto&                  cpu_scene = scene.GetCpuScene();

    snapshot.primitive_count = cpu_scene.GetPrimitiveCount();
    snapshot.light_count     = cpu_scene.GetLightCount();

    registry.view<const ecs::CRenderable>().each([&](const auto, const ecs::CRenderable& renderable) {
        if (!registry.valid(renderable.mesh_entt) || !registry.all_of<ecs::CMesh>(renderable.mesh_entt)) {
            return;
        }

        const auto& mesh = registry.get<ecs::CMesh>(renderable.mesh_entt);
        for (const entt::entity primitive_entity : mesh.primitive_entts) {
            if (!registry.valid(primitive_entity) || !registry.all_of<ecs::CPrimitive>(primitive_entity)) {
                continue;
            }

            const auto& primitive = registry.get<ecs::CPrimitive>(primitive_entity);
            if (!registry.valid(primitive.material_entt) ||
                !registry.all_of<ecs::CMaterial>(primitive.material_entt)) {
                continue;
            }

            if (registry.get<ecs::CMaterial>(primitive.material_entt).emissive_factor != float3(0.f)) {
                ++snapshot.emissive_instance_count;
                snapshot.emissive_triangle_count += primitive.index_count / 3;
            }
        }
    });

    registry.view<const ecs::CPrimitive>().each([&](const auto             primitive_entity,
                                                    const ecs::CPrimitive& primitive) {
        if (!registry.valid(primitive.material_entt) ||
            !registry.all_of<ecs::CMaterial>(primitive.material_entt) ||
            registry.get<ecs::CMaterial>(primitive.material_entt).emissive_factor == float3(0.f)) {
            return;
        }

        const uint primitive_id = cpu_scene.GetPrimitiveId(primitive_entity);
        if (primitive_id == UINT_MAX) {
            LOG_ERROR("Primitive entity {} not found in CpuScene", entt::to_integral(primitive_entity));
            return;
        }

        snapshot.emissive_primitives.emplace_back(EmissivePrimitiveFrameInput{
            .stable_key         = static_cast<uint64>(primitive_id),
            .primitive_id       = primitive_id,
            .num_triangles      = primitive.index_count / 3,
            .index_start_idx    = primitive.index.is_valid ? primitive.index.start_idx : 0,
            .first_instance_idx = cpu_scene.GetFirstInstanceIndex(primitive_id)
        });
    });

    auto                light_view = registry.view<const ecs::CLight>();
    Array<entt::entity> light_entities(light_view.begin(), light_view.end());
    std::ranges::sort(light_entities, [&](entt::entity lhs, entt::entity rhs) {
        return LightPriority(lhs, scene) < LightPriority(rhs, scene);
    });

    for (const entt::entity entity : light_entities) {
        PolymorphicLightInfo light_info{};
        if (!ConvertLight(entity, scene, light_info)) {
            continue;
        }

        const auto type = registry.get<ecs::CLight>(entity).type;
        snapshot.analytic_lights.emplace_back(AnalyticLightFrameInput{
            .stable_key = static_cast<uint64>(entt::to_integral(entity)),
            .light_class =
                type == ELightType::Directional ? EAnalyticLightClass::Infinite : EAnalyticLightClass::Finite,
            .light_info = light_info
        });
    }

    return snapshot;
}

} // namespace Moer::Render::Raytracing
