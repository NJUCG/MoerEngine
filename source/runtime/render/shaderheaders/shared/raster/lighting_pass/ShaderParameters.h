#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
//#define CONST constexpr
#include "misc/Traits.h"
#include "shaderheaders/shared/raster/ShaderParametersUtils.h"
namespace Moer::Render {
#else
#include "shared/raster/ShaderParametersUtils.h"
//#define CONST const
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
    uint   skybox_handle_posz;
    uint   skybox_handle_negz;
    uint   skybox_handle_posx;
    uint   skybox_handle_negx;
    uint   skybox_handle_posy;
    uint   skybox_handle_negy;
};

// TODO: 下面的重复变量是否可以使用数组的方式合并？
struct LightingData {
    float4x4 world_to_shadow_clip_0;
    float4x4 world_to_shadow_clip_1;
    float4x4 world_to_shadow_clip_2;
    float4x4 world_to_shadow_clip_3;

    float4x4 inv_view_proj;
    float3   camera_position;
    // uint     padding;// FIXME: 需要加吗？
    uint light_count;

    uint shadow_map_mode;
    uint shadow_sampling_mode;
    uint shadow_csm_num_of_cascades;
    uint shadow_csm_sm_size;

    uint shadow_map_0;
    uint shadow_map_1;
    uint shadow_map_2;
    uint shadow_map_3;
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
//#undef CONST