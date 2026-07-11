/**
 * 请统一Include如下文件，不要Include当前文件
 * CPP:
 *     #include "shaderheaders/shared/raster/ShaderParameters.h"
 * HLSL:
 *     #include "shared/raster/ShaderParameters.h"
 */
#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
//#define CONST constexpr
#include <cstddef>
#include "misc/Traits.h"
namespace Moer::Render {
#else
//#define CONST const
namespace Moer {
#endif

// MARK: Main Content Begin

#ifdef __cplusplus
static constexpr uint RASTER_PROBE_VOLUME_MAX_COUNT = 4;
static constexpr uint RASTER_PROBE_MAX_COUNT_PER_VOLUME = 512;
static constexpr uint RASTER_PROBE_MAX_COUNT =
    RASTER_PROBE_VOLUME_MAX_COUNT * RASTER_PROBE_MAX_COUNT_PER_VOLUME;
static constexpr uint RASTER_PROBE_BRICK_DIM = 4;
static constexpr uint RASTER_PROBE_MAX_FINE_BRICKS_PER_AXIS = 4;
static constexpr uint RASTER_PROBE_MAX_SUBDIVISION_LEVEL = 2;
static constexpr uint RASTER_PROBE_CELL_BRICK_DIM = 2;
static constexpr uint RASTER_PROBE_OCCUPANCY_GRID_DIM = 4;
static constexpr uint RASTER_PROBE_OCCUPANCY_VOXEL_COUNT =
    RASTER_PROBE_OCCUPANCY_GRID_DIM * RASTER_PROBE_OCCUPANCY_GRID_DIM * RASTER_PROBE_OCCUPANCY_GRID_DIM;
static constexpr uint RASTER_PROBE_MAX_CELLS_PER_VOLUME = 8;
static constexpr uint RASTER_PROBE_MAX_CELL_COUNT =
    RASTER_PROBE_VOLUME_MAX_COUNT * RASTER_PROBE_MAX_CELLS_PER_VOLUME;
static constexpr uint RASTER_PROBE_MAX_PAGES_PER_VOLUME = 128;
static constexpr uint RASTER_PROBE_MAX_PAGE_COUNT =
    RASTER_PROBE_VOLUME_MAX_COUNT * RASTER_PROBE_MAX_PAGES_PER_VOLUME;
static constexpr uint RASTER_PROBE_MAX_BRICKS_PER_VOLUME = RASTER_PROBE_MAX_PAGES_PER_VOLUME;
static constexpr uint RASTER_PROBE_MAX_BRICK_COUNT =
    RASTER_PROBE_VOLUME_MAX_COUNT * RASTER_PROBE_MAX_BRICKS_PER_VOLUME;
static constexpr uint RASTER_PROBE_PAGE_INVALID = 0xffffffffu;
static constexpr uint RASTER_PROBE_UPDATE_GROUP_SIZE = 64;
static constexpr uint RASTER_PROBE_VISIBILITY_ATLAS_DIM = 8;
static constexpr uint RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT =
    RASTER_PROBE_VISIBILITY_ATLAS_DIM * RASTER_PROBE_VISIBILITY_ATLAS_DIM;
static constexpr uint RASTER_PROBE_IRRADIANCE_ATLAS_DIM = 8;
static constexpr uint RASTER_PROBE_IRRADIANCE_ATLAS_TEXEL_COUNT =
    RASTER_PROBE_IRRADIANCE_ATLAS_DIM * RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
static constexpr uint RASTER_PROBE_ATLAS_TILE_BORDER = 1;
static constexpr uint RASTER_PROBE_ATLAS_TILE_STRIDE =
    RASTER_PROBE_IRRADIANCE_ATLAS_DIM + RASTER_PROBE_ATLAS_TILE_BORDER * 2;
static constexpr uint RASTER_PROBE_ATLAS_TILE_COLUMNS = 32;
static constexpr uint RASTER_PROBE_ATLAS_TILE_ROWS =
    (RASTER_PROBE_MAX_COUNT + RASTER_PROBE_ATLAS_TILE_COLUMNS - 1) / RASTER_PROBE_ATLAS_TILE_COLUMNS;
static constexpr uint RASTER_PROBE_ATLAS_TEXTURE_WIDTH =
    RASTER_PROBE_ATLAS_TILE_COLUMNS * RASTER_PROBE_ATLAS_TILE_STRIDE;
static constexpr uint RASTER_PROBE_ATLAS_TEXTURE_HEIGHT =
    RASTER_PROBE_ATLAS_TILE_ROWS * RASTER_PROBE_ATLAS_TILE_STRIDE;
static constexpr uint RASTER_PROBE_STATE_INVALID = 0;
static constexpr uint RASTER_PROBE_STATE_ACTIVE = 1;
static constexpr uint RASTER_PROBE_STATE_RELOCATED = 2;
static constexpr uint RASTER_PROBE_STATE_NEAR_SURFACE = 3;
static constexpr uint RASTER_PROBE_GIZMO_DRAW_MODE_PROBES = 1;
static constexpr uint RASTER_PROBE_GIZMO_DRAW_MODE_BOUNDS = 2;
static constexpr uint RASTER_PROBE_GIZMO_DRAW_MODE_ADAPTIVE_CELL_BOUNDS = 3;
#else
static const uint RASTER_PROBE_VOLUME_MAX_COUNT = 4;
static const uint RASTER_PROBE_MAX_COUNT_PER_VOLUME = 512;
static const uint RASTER_PROBE_MAX_COUNT =
    RASTER_PROBE_VOLUME_MAX_COUNT * RASTER_PROBE_MAX_COUNT_PER_VOLUME;
static const uint RASTER_PROBE_BRICK_DIM = 4;
static const uint RASTER_PROBE_MAX_FINE_BRICKS_PER_AXIS = 4;
static const uint RASTER_PROBE_MAX_SUBDIVISION_LEVEL = 2;
static const uint RASTER_PROBE_CELL_BRICK_DIM = 2;
static const uint RASTER_PROBE_OCCUPANCY_GRID_DIM = 4;
static const uint RASTER_PROBE_OCCUPANCY_VOXEL_COUNT =
    RASTER_PROBE_OCCUPANCY_GRID_DIM * RASTER_PROBE_OCCUPANCY_GRID_DIM * RASTER_PROBE_OCCUPANCY_GRID_DIM;
static const uint RASTER_PROBE_MAX_CELLS_PER_VOLUME = 8;
static const uint RASTER_PROBE_MAX_CELL_COUNT =
    RASTER_PROBE_VOLUME_MAX_COUNT * RASTER_PROBE_MAX_CELLS_PER_VOLUME;
static const uint RASTER_PROBE_MAX_PAGES_PER_VOLUME = 128;
static const uint RASTER_PROBE_MAX_PAGE_COUNT =
    RASTER_PROBE_VOLUME_MAX_COUNT * RASTER_PROBE_MAX_PAGES_PER_VOLUME;
static const uint RASTER_PROBE_MAX_BRICKS_PER_VOLUME = RASTER_PROBE_MAX_PAGES_PER_VOLUME;
static const uint RASTER_PROBE_MAX_BRICK_COUNT =
    RASTER_PROBE_VOLUME_MAX_COUNT * RASTER_PROBE_MAX_BRICKS_PER_VOLUME;
static const uint RASTER_PROBE_PAGE_INVALID = 0xffffffffu;
static const uint RASTER_PROBE_UPDATE_GROUP_SIZE = 64;
static const uint RASTER_PROBE_VISIBILITY_ATLAS_DIM = 8;
static const uint RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT =
    RASTER_PROBE_VISIBILITY_ATLAS_DIM * RASTER_PROBE_VISIBILITY_ATLAS_DIM;
static const uint RASTER_PROBE_IRRADIANCE_ATLAS_DIM = 8;
static const uint RASTER_PROBE_IRRADIANCE_ATLAS_TEXEL_COUNT =
    RASTER_PROBE_IRRADIANCE_ATLAS_DIM * RASTER_PROBE_IRRADIANCE_ATLAS_DIM;
static const uint RASTER_PROBE_ATLAS_TILE_BORDER = 1;
static const uint RASTER_PROBE_ATLAS_TILE_STRIDE =
    RASTER_PROBE_IRRADIANCE_ATLAS_DIM + RASTER_PROBE_ATLAS_TILE_BORDER * 2;
static const uint RASTER_PROBE_ATLAS_TILE_COLUMNS = 32;
static const uint RASTER_PROBE_ATLAS_TILE_ROWS =
    (RASTER_PROBE_MAX_COUNT + RASTER_PROBE_ATLAS_TILE_COLUMNS - 1) / RASTER_PROBE_ATLAS_TILE_COLUMNS;
static const uint RASTER_PROBE_ATLAS_TEXTURE_WIDTH =
    RASTER_PROBE_ATLAS_TILE_COLUMNS * RASTER_PROBE_ATLAS_TILE_STRIDE;
static const uint RASTER_PROBE_ATLAS_TEXTURE_HEIGHT =
    RASTER_PROBE_ATLAS_TILE_ROWS * RASTER_PROBE_ATLAS_TILE_STRIDE;
static const uint RASTER_PROBE_STATE_INVALID = 0;
static const uint RASTER_PROBE_STATE_ACTIVE = 1;
static const uint RASTER_PROBE_STATE_RELOCATED = 2;
static const uint RASTER_PROBE_STATE_NEAR_SURFACE = 3;
static const uint RASTER_PROBE_GIZMO_DRAW_MODE_PROBES = 1;
static const uint RASTER_PROBE_GIZMO_DRAW_MODE_BOUNDS = 2;
static const uint RASTER_PROBE_GIZMO_DRAW_MODE_ADAPTIVE_CELL_BOUNDS = 3;
#endif

struct ProbeGridProbeData {
    float4 world_position; // xyz = probe center, w = RASTER_PROBE_STATE_*
    float4 irradiance;     // rgb = diffuse irradiance, w = confidence
    float4 visibility;     // x = mean ray distance, y = mean distance squared, z = open ratio, w = traced confidence
};

struct ProbeGridVisibilityTexel {
    float4 moments; // x = mean distance, y = mean distance squared, z = open ratio, w = confidence
};

struct ProbeGridIrradianceTexel {
    float4 irradiance; // rgb = directional diffuse irradiance, w = confidence
};

struct ProbeVolumeGpuDesc {
    float4 origin_bias;       // xyz = min corner, w = normal bias
    float4 spacing_intensity; // xyz = probe spacing, w = GI intensity
    float4 extent_blend;      // xyz = volume extent, w = boundary blend distance
    uint4  counts;            // xyz = local grid counts, w = local probe count
    uint4  allocation;        // x = logical probe base, y = config index, z = page table base, w = brick count
    float4 visibility;        // x = bias, y = power, z = min weight, w = strength
    uint4  hierarchy;         // x = first cell, y = cell count, z = max subdivision level, w = layout generation
};

struct ProbeCellGpuDesc {
    float4 origin_spacing; // xyz = cell min probe position, w = minimum base probe spacing
    float4 extent;         // xyz = span from first to last probe, w = geometry occupancy ratio
    uint4  coord_volume;   // xyz = logical cell coordinate, w = compact volume index
    uint4  brick_range;    // x = first brick descriptor, y = brick count, z = requested count, w = resident count
    uint4  hierarchy;      // x = min level, y = max level, z = config index, w = layout generation
    uint4  geometry;       // x = intersecting primitives, y = occupied 4^3 voxels, z = desired level, w = geometry generation
};

struct ProbeBrickGpuDesc {
    uint4 coord_volume; // xyz = logical brick coordinate, w = compact volume index
    uint4 probe_range;  // x = physical probe offset, y = local probe count, z = resident state, w = page index
    uint4 local_counts; // xyz = valid probe counts inside this brick, w = frames since last update (clamped)
    uint4 hierarchy;    // x = cell index, y = subdivision level, z = parent page, w = virtual page
    uint4 neighbor_pages_0; // x/y = -X/+X, z/w = -Y/+Y virtual pages
    uint4 neighbor_pages_1; // x/y = -Z/+Z virtual pages, z = layout generation, w = flags
};

struct ProbeUpdateParam {
    uint4  probe_volume_counts;  // xyz = local probe counts, w = compact volume index
    float4 probe_volume_origin;  // xyz = min corner, w = irradiance history weight
    float4 probe_volume_spacing; // xyz = cell spacing, w = visibility history weight
    float4 probe_sky_color;      // rgb = sky tint, w = sky intensity
    float4 probe_ground_color;   // rgb = ground tint, w = directional bounce strength
    float4 main_light_direction; // xyz = world-space light direction, w = resident brick index
    float4 main_light_color;     // rgb = light color, w = light intensity
    float4 probe_trace_config;   // x = max ray distance, y = trace bias, z = ray count, w = ray query enabled
};

struct ProbeGizmoParam {
    float4x4 world2clip;
    uint4    probe_volume_config; // probes: x = mode, y = color mode, z = probe buffer handle, w = probe count; bounds: x = mode, yzw = origin bits
    float4   gizmo_config;        // probes: x = axis half-size, y = color intensity, z = axis thickness; bounds: xyz = extent, w = line thickness
    float4   fixed_color;         // rgb = fixed gizmo color, w = alpha
    float4   camera_position;     // xyz = camera position, w = reserved
};

struct MaterialPassBindlessParam {
    float3 extra_ambient_color;
    float  extra_ambient_intensity;
    uint   enable_extra_ambient;
    uint   material_type;
    uint   light_buf_hdl;
    uint   gbuffer_base_color;
    uint   gbuffer_normal;
    uint   gbuffer_metal_rough_ao;
    uint   gbuffer_depth;
    uint   gbuffer_position;
    uint   shading_mode;
    uint   cubemap_handle;
    uint   shadow_mask_handle;
};

// UBO (ConstantBuffer)，需要遵循std140
struct LightingData {
    float4x4 world2shadow_clip[4]; // 4层CSM
    float4x4 world2view;
    float4x4 clip2world;

    float4 scale_data[4];

    float4 cascade_split_ratios;       // [0..3]，因为std140才写成float4
    float4 cascade_blend_start_ratios; // [0..3]，因为std140才写成float4
    uint4  cascade_shadow_map;         // [0..3]，因为std140才写成uint4，存放CSM的纹理句柄
    // HLSL支持通过[x]访问float4和uint4，所以HLSL不需要再修改

    float3 camera_position;
    uint   light_count;

    float3 main_light_direction;
    uint   shadow_map_mode;

    float3 light_pos;
    float  light_radius;

    uint shadow_sampling_mode;
    uint shadow_csm_num_of_cascades;
    uint shadow_csm_sm_size;
    uint shadow_csm_visualize_cascade;

    uint  point_shadow_map; //handle
    uint  pcss_enabled;
    float light_size_world; //assumed light size for soft shadow calculation
    float near_clip;

    float far_clip;
    uint  is_csm_blend_enabled;
    uint  lut_ggx_emu_handle;
    uint  lut_ggx_eavg_handle;

    uint brdf_enable_multi_scatter; // kulla-conty approximation
    uint brdf_NDF_mode;             // NDF Mode
    uint brdf_G_mode;               // 用 Vis_SmithJointGGX 来代替 G_Smith
    uint brdf_G_is_ibl;             // 是否使用IBL的Fresnel近似

    // Skybox
    uint  skybox_exposure_correct_enabled; // 是否启用Skybox曝光校正，找到第一个平行光，乘上它的颜色
    float skybox_exposure_correct_factor;  // 曝光校正因子
    float2 skybox_exposure_padding; // pad next float4 to a cbuffer 16B register

    // Probe GI
    uint4 probe_system_config; // x = enabled, y = debug mode, z = probe buffer, w = volume descriptor buffer
    uint4 probe_system_counts; // x = volume count, y = probe count, z = brick buffer, w = page table buffer
    uint4 probe_system_atlas;  // x = visibility buffer, y = irradiance buffer, z = irradiance texture, w = visibility texture
    uint4 probe_system_hierarchy; // x = cell buffer, y = cell count, z = max level, w = layout generation
    float4 probe_system_debug; // x = debug scale, yzw = reserved
};

#ifdef __cplusplus
static_assert(sizeof(ProbeUpdateParam) <= 128);
static_assert(sizeof(ProbeVolumeGpuDesc) == 112);
static_assert(sizeof(ProbeCellGpuDesc) == 96);
static_assert(sizeof(ProbeBrickGpuDesc) == 96);
static_assert(sizeof(ProbeVolumeGpuDesc) % 16 == 0);
static_assert(sizeof(ProbeCellGpuDesc) % 16 == 0);
static_assert(sizeof(ProbeBrickGpuDesc) % 16 == 0);
static_assert(offsetof(LightingData, probe_system_config) % 16 == 0);
static_assert(sizeof(LightingData) % 16 == 0);
#endif

struct DirectionalShadowMaskPassBindlessParam {
    uint normal_hdl;
    uint depth_hdl;
};

// MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
//#undef CONST
