#pragma once

#include "PixelFormat.h"
#include "misc/STL.h"
#include "rhi/RHICommandDrawData.h"
#include "shaderheaders/shared/scene/SharedSceneStruct.h"

#include <climits>
#include <cstdint>
#include <limits>
#include <string>

namespace Moer::Render {

using GpuSceneResourceKey = uint64;

inline constexpr GpuSceneResourceKey k_invalid_gpu_scene_resource_key =
    std::numeric_limits<GpuSceneResourceKey>::max();

struct GpuSceneTextureData {
    GpuSceneResourceKey key = k_invalid_gpu_scene_resource_key;
    std::string         name;
    Array<uint8>        data;
    EPixelFormat        format            = PF_UNDEFINED;
    uint32              width             = 0;
    uint32              height            = 0;
    uint32              mip_level_count   = 1;
    uint32              array_layer_count = 1;
};

struct GpuSceneMaterialTextureRefs {
    GpuSceneResourceKey normal             = k_invalid_gpu_scene_resource_key;
    GpuSceneResourceKey ao                 = k_invalid_gpu_scene_resource_key;
    GpuSceneResourceKey albedo             = k_invalid_gpu_scene_resource_key;
    GpuSceneResourceKey emissive           = k_invalid_gpu_scene_resource_key;
    GpuSceneResourceKey metallic_roughness = k_invalid_gpu_scene_resource_key;
};

struct GpuSceneRtGeometryData {
    uint vertex_offset = 0;
    uint vertex_count  = 0;
    uint index_offset  = 0;
    uint index_count   = 0;
    uint primitive_id  = UINT_MAX;
};

struct GpuSceneRtMeshData {
    GpuSceneResourceKey           key = k_invalid_gpu_scene_resource_key;
    Array<GpuSceneRtGeometryData> geometries;
};

struct GpuSceneRtInstanceData {
    GpuSceneResourceKey mesh_key = k_invalid_gpu_scene_resource_key;
    float4x4            world_transform{};
    uint                primitive_count    = 0;
    uint                first_primitive_id = UINT_MAX;
};

enum class EGpuSceneRaytracingUpdate : uint8 {
    None,
    UpdateInstances,
    RebuildTlas,
    RebuildBlas,
};

struct GpuSceneUpdate {
    bool full_rebuild     = false;
    bool update_lights    = false;
    bool update_materials = false;
    bool update_meshes    = false;

    EGpuSceneRaytracingUpdate raytracing_update = EGpuSceneRaytracingUpdate::None;

    Array<GpuSceneTextureData>          textures;
    Array<GLight>                       lights;
    Array<GMaterial>                    materials;
    Array<GpuSceneMaterialTextureRefs>  material_texture_refs;
    Array<DrawIndexedCmdData>           draw_commands;
    Array<GPrimitive>                   primitives;
    Array<GInstance>                    instances;
    Array<GClusterGroup>                cluster_groups;
    Array<float3>                       positions;
    Array<uint32>                       packed_normals;
    Array<uint32>                       packed_tangents;
    Array<float2>                       texcoords0;
    Array<uint32>                       indices;
    Array<GpuSceneRtMeshData>           rt_meshes;
    Array<GpuSceneRtInstanceData>       rt_instances;

    bool HasWork() const {
        return full_rebuild || update_lights || update_materials || update_meshes ||
               raytracing_update != EGpuSceneRaytracingUpdate::None;
    }
};

} // namespace Moer::Render
