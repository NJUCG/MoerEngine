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

#define MAX_CSM_CASCADES 8

#ifdef __cplusplus
//#define CONST constexpr
#include "misc/Traits.h"
namespace Moer::Render {
#else
//#define CONST const
namespace Moer {
#endif

// MARK: Main Content Begin

struct MaterialPassBindlessParam {
    float3 extra_ambient_color;
    float  extra_ambient_intensity;
    uint   enable_extra_ambient;
    uint   material_type;
    uint   light_buf_hdl;
    uint   material_buf_hdl;
    uint   vbuffer;
    uint   gbuffer_normal;
    uint   gbuffer_tangent;
    uint   gbuffer_uv;
    uint   gbuffer_depth;
    uint   gbuffer_position;
    uint   global_param_handle;
    uint   shading_mode;
    uint   cubemap_handle;
    uint   shadow_mask_handle;
};
struct LightingData {
    float4x4 world2shadow_clip[MAX_CSM_CASCADES];

    float4x4 clip2world;
    float3   camera_position;
    // uint     padding;// FIXME: need or not?
    uint   light_count;
    float3 main_light_direction;

    uint shadow_map_mode;
    uint shadow_sampling_mode;
    uint shadow_csm_num_of_cascades;
    uint shadow_csm_sm_size;
    uint shadow_csm_visualize_cascade;

    uint cascade_shadow_map[MAX_CSM_CASCADES];

    // Point Light Shadow Map
    uint   point_shadow_map; //handle
    float3 light_pos;
    float  light_radius;

    uint pcss_enabled;

    float  light_size_world; //assumed light size for soft shadow calculation
    float4 scale_data[MAX_CSM_CASCADES];

    float4x4 world2view;
    float    near_clip;
    float    far_clip;
    float    cascade_split_ratios[MAX_CSM_CASCADES];
    float    cascade_blend_start_ratios[MAX_CSM_CASCADES];
    uint     is_csm_blend_enabled;

    // Shading
    uint lut_ggx_emu_handle;
    uint lut_ggx_eavg_handle;

    uint brdf_enable_multi_scatter; // kulla-conty approximation
    uint brdf_NDF_mode;             // NDF Mode
    uint brdf_G_mode;               // 用 Vis_SmithJointGGX 来代替 G_Smith
    uint brdf_G_is_ibl;             // 是否使用IBL的Fresnel近似

    // Skybox
    uint  skybox_exposure_correct_enabled; // 是否启用Skybox曝光校正，找到第一个平行光，乘上它的颜色
    float skybox_exposure_correct_factor;  // 曝光校正因子
};

struct DirectionalShadowMaskPassBindlessParam {
    uint global_param_hdl;
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