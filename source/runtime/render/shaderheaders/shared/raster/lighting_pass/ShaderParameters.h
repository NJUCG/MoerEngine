#pragma once

#ifdef CONST
#undef CONST
#endif

#define MAX_CSM_CASCADES 4

#ifdef __cplusplus
#define CONST constexpr
#include "misc/Traits.h"
#include "shaderheaders/shared/raster/ShaderParametersUtils.h"
namespace Moer::Render {
#else
#include "shared/raster/ShaderParametersUtils.h"
#define CONST const
namespace Moer {
#endif

// MARK: Main Content Begin

struct MaterialPassBindlessParam {
    float3 extra_ambient_color;
    float  extra_ambient_intensity;
    uint   enable_extra_ambient;
    uint   material_type;
    uint   light_buffer;
    uint   material_buffer;
    uint   vbuffer;
    uint   gbuffer_normal;
    uint   gbuffer_tangent;
    uint   gbuffer_uv;
    uint   gbuffer_depth;
    uint   gbuffer_position;
    uint   global_param_handle;
    uint   shading_mode;
    uint   skybox_handles[6]; //posz, negz, posy,negy, posx, negx
    uint   skybox_handle_posz;
    uint   skybox_handle_negz;
    uint   skybox_handle_posx;
    uint   skybox_handle_negx;
    uint   skybox_handle_posy;
    uint   skybox_handle_negy;

    //TODO:加速csm计算
    float4x4 view_matrix;
    float    near_clip;
    float    far_clip;
    float    csm_split_ratios[MAX_CSM_CASCADES];
};
struct LightingData {
    float4x4 world_to_shadow_clip[MAX_CSM_CASCADES];

    float4x4 inv_view_proj;
    float3   camera_position;
    // uint     padding;// FIXME: 需要加吗？
    uint light_count;

    uint shadow_map_mode;
    uint shadow_sampling_mode;
    uint shadow_csm_num_of_cascades;
    uint shadow_csm_sm_size;

    uint shadow_map[MAX_CSM_CASCADES];
};

// MARK: Main Content End

//MARK:Enum Definitions Begin
//deferred
//shadowpass
//ssrpass

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST