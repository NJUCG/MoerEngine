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
static constexpr uint RASTER_PROBE_MAX_COUNT = 512;
static constexpr uint RASTER_PROBE_UPDATE_GROUP_SIZE = 64;
static constexpr uint RASTER_PROBE_VISIBILITY_ATLAS_DIM = 8;
static constexpr uint RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT =
    RASTER_PROBE_VISIBILITY_ATLAS_DIM * RASTER_PROBE_VISIBILITY_ATLAS_DIM;
#else
static const uint RASTER_PROBE_MAX_COUNT = 512;
static const uint RASTER_PROBE_UPDATE_GROUP_SIZE = 64;
static const uint RASTER_PROBE_VISIBILITY_ATLAS_DIM = 8;
static const uint RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT =
    RASTER_PROBE_VISIBILITY_ATLAS_DIM * RASTER_PROBE_VISIBILITY_ATLAS_DIM;
#endif

struct ProbeGridProbeData {
    float4 world_position; // xyz = probe center, w = validity
    float4 irradiance;     // rgb = diffuse irradiance, w = confidence
    float4 visibility;     // x = mean ray distance, y = mean distance squared, z = open ratio, w = traced confidence
};

struct ProbeGridVisibilityTexel {
    float4 moments; // x = mean distance, y = mean distance squared, z = open ratio, w = confidence
};

struct ProbeUpdateParam {
    uint4  probe_volume_counts;  // xyz = probe counts, w = total probe count
    float4 probe_volume_origin;  // xyz = min corner, w = unused
    float4 probe_volume_spacing; // xyz = cell spacing, w = GI intensity
    float4 probe_sky_color;      // rgb = sky tint, w = sky intensity
    float4 probe_ground_color;   // rgb = ground tint, w = directional bounce strength
    float4 main_light_direction; // xyz = world-space light direction, w = temporal phase
    float4 main_light_color;     // rgb = light color, w = light intensity
    float4 probe_trace_config;   // x = max ray distance, y = trace bias, z = ray count, w = ray query enabled
};

struct ProbeGizmoParam {
    float4x4 world2clip;
    uint4    probe_volume_config; // x = enabled, y = color mode, z = probe buffer handle, w = probe count
    float4   gizmo_config;        // x = axis half-size, y = color intensity, z = axis thickness, w = reserved
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
    float4 probe_volume_origin;  // xyz = min corner, w = normal bias
    float4 probe_volume_spacing; // xyz = cell spacing, w = intensity
    float4 probe_volume_extent;  // xyz = volume extent, w = debug scale
    uint4  probe_volume_counts;  // xyz = probe counts, w = total probe count
    uint4  probe_volume_config;  // x = enabled, y = debug mode, z = probe buffer handle, w = visibility atlas handle
    float4 probe_volume_visibility; // x = bias, y = power, z = min weight, w = strength
};

#ifdef __cplusplus
static_assert(sizeof(ProbeUpdateParam) <= 128);
static_assert(offsetof(LightingData, probe_volume_origin) % 16 == 0);
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
