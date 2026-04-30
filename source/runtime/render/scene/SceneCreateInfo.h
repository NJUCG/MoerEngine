#pragma once

#include "math/Quaternion.h"
#include "misc/BoundingBox.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "shaderheaders/shared/raster/SharedEnum.h"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <string>
#include <string_view>


namespace Moer {

enum class EProceduralPrimitiveShape : std::uint32_t {
    Cube,
    FacetedSphere,
};

struct PointLightCreateInfo {
    float3       position              = float3(0.f, 0.f, 0.f);
    float3       color                 = float3(1.f, 1.f, 1.f);
    float        intensity             = 1.f;
    std::string  name                  = "Runtime Point Light";
    entt::entity parent_node_entt      = entt::null;
    bool         should_set_main_light = false;
};

struct EntityWithNodeCreateInfo {
    entt::entity     parent_node_entt = entt::null;
    std::string_view name             = {};
    float3           translation      = float3(0.f, 0.f, 0.f);
    Quaternion       rotation         = Quaternion();
    float3           scale            = float3(1.f, 1.f, 1.f);
};

struct RenderableCreateInfo {
    entt::entity     mesh_entt        = entt::null;
    entt::entity     parent_node_entt = entt::null;
    std::string_view name             = {};
    float3           translation      = float3(0.f, 0.f, 0.f);
    Quaternion       rotation         = Quaternion();
    float3           scale            = float3(1.f, 1.f, 1.f);
};

struct MaterialCreateInfo {
    std::string_view name             = {};
    float4           albedo_factor    = float4(1.f, 1.f, 1.f, 1.f);
    float3           emissive_factor  = float3(0.f, 0.f, 0.f);
    float            metallic_factor  = 0.f;
    float            roughness_factor = 1.f;
    EAlphaMode       alpha_mode       = EAlphaMode::Opaque;
    float            alpha_cutoff     = 0.5f;
};

struct PrimitiveCreateInfo {
    std::string_view name = {};

    Array<float3> positions;
    Array<float3> normals;
    Array<float3> tangents;
    Array<float2> texcoord0;
    Array<uint32> indices;

    entt::entity material_entt = entt::null;

    Box3D aabb;
    bool  has_aabb = false;
};

struct MeshCreateInfo {
    std::string_view    name = {};
    Array<entt::entity> primitive_entts;
};

struct ProceduralMeshCreateInfo {
    EProceduralPrimitiveShape shape = EProceduralPrimitiveShape::Cube;

    entt::entity     parent_node_entt = entt::null;
    std::string_view name             = "Runtime Procedural Renderable";
    float3           translation      = float3(0.f, 0.f, 0.f);
    Quaternion       rotation         = Quaternion();
    float3           scale            = float3(1.f, 1.f, 1.f);

    MaterialCreateInfo material;
};

struct CreateProceduralRenderableResult {
    entt::entity material_entt   = entt::null;
    entt::entity primitive_entt  = entt::null;
    entt::entity mesh_entt       = entt::null;
    entt::entity renderable_entt = entt::null;

    explicit operator bool() const {
        return renderable_entt != entt::null;
    }
};

} // namespace Moer