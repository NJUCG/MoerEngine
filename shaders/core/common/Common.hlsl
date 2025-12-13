#ifndef FRAMEWORK_COMMON_HLSL
#define FRAMEWORK_COMMON_HLSL
#include "core/math/Math.hlsli"

#define FP16_MAX 65504.0
#define INF 1e5

#define MAX_MIP 11

// Missing Texture Color (purple)
#define MISSING_TEXTURE_COLOR float3(1.0, 0.0, 1.0)

// Mip mode
#define MIP_VISIBILITY 0 // for visibility: emission, shadow and alpha mask
#define MIP_LESS_SHARP 1 // for normal
#define MIP_SHARP 2      // for albedo and roughness

// Shadow
#define SHADOW_BIAS 0.000001

// BRDF
#define RF0_DIELECTRICS 0.04
#define GTR_GAMMA 1.5

struct CameraData {
  float4x4 view;
  float4x4 view_proj;
  float4x4 prev_view_proj;
  float4 camera_pos;
};

struct InstanceData {
  float4x4 model2world;
  float4x4 inv_model2world;
  float scale;
  float padding;
  uint material_id;
  uint material_type;
};

static const float3 corners[8] = {
    float3(-1, -1, -1), float3(-1, 1, -1), float3(1, 1, -1), float3(1, -1, -1),
    float3(-1, -1, 1),  float3(-1, 1, 1),  float3(1, 1, 1),  float3(1, -1, 1),
};
struct VirtualView {
  float4x4 view;
  float4x4 view_proj;
  float4x4 prev_view_proj;
  float4x4 proj;
  float4 planes[6];
  float3 pos;
  float nearz;
  float3 bound_center;
  float aspect_ratio;
  float3 bound_extent;
  float inv_tan_half_fov;
};

struct InstanceMeshletInfo {
  float3 center;
  uint vertex_offset;
  float3 extent;
  uint vertex_count;
  uint index_offset;
  uint index_count;
  uint meshlet_offset;
  uint meshlet_count;
};
struct InstanceMeshletCullInfo {
  uint meshlet_id;
  uint instance_id;
  uint padding;
  uint padding2;
};

struct DrawCommandData {
  uint index_count;
  uint instance_count;
  uint first_index;
  uint vertex_offset;
  uint first_instance;
  uint padding;
  uint padding2;
  uint padding3;
};

struct MeshletDesc {
  uint vertex_offset;
  uint vertex_count;
  uint index_offset;
  uint index_count;
};

struct RTConfigParam {
  float4x4 view2world;
  float4x4 view2clip;
  float4x4 world2view;
  float4x4 world2view_prev;
  float4x4 world2clip;
  float4x4 world2clip_prev;
  float4 nrd_hit_dist_params;
  float4 sun_direction_gexposure;
  float4 camera_origin_gmip_bias;
  float4 view_direction_gorthomode;
  // float4 HairBaseColorOverride; // w is alpha or blend factor
  // float2 HairBetasOverride;
  float2 window_size;
  float2 inv_window_size;
  float2 output_size;
  float2 inv_output_size;
  float2 render_size;
  float2 inv_render_size;
  float2 rect_size;
  float2 inv_rect_size;
  float2 rect_size_prev;
  float2 jitter;
  float emission_intensity;
  float separator;
  float roughness_override;
  float metalness_override;
  float unit_to_meters_multiplier;
  float indirect_diffuse;
  float indirect_specular;
  float tan_sun_angular_radius;
  float tan_pixel_angular_radius;
  float debug;
  float transparent;
  float prev_frame_confidence;
  float min_probability;
  float unproject;
  float aperture;
  float focal_distance;
  float focal_length;
  uint denoiser_type;
  uint on_screen;
  uint frame_index;
  uint forced_material;
  uint use_normalmap;
  uint b_worldspace_motion;
  uint tracing_mode;
  uint sample_num;
  uint bounce_num;
  uint taa;
  uint resolve;
  uint psr;
  uint validation;
  uint trim_lobe;
  // uint32_t highlight_ahs;
  // uint32_t ahs_dynamic_mip;

  // Ambient
  float ambient_max_accumulated_frames_num;
  float ambient;
};

struct MaterialData {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float ao;
  int albedo_map;
  int normal_map;
  int metallic_roughness_map;
  int ao_map;
  int emissive_map;
  int padding;
};

#define RTCONFIG_BINDING(t, s)                                                 \
  [[vk::binding(t, s)]] ConstantBuffer<RTConfigParam> rt_config                \
      : register(b##t, space##s)

static uint cull_thread_size = 64;
static uint cull_thread_bits = 6;

uint IncrementDispatchCounter(in RWByteAddressBuffer target, uint offset,
                              uint cnt, uint prev_cnt) {
  uint result = 0;
  uint sum_cnt = prev_cnt + cnt;
  uint dispatch_cnt_inc =
      ((sum_cnt + cull_thread_size - 1) >> cull_thread_bits) -
      ((prev_cnt + cull_thread_size - 1) >> cull_thread_bits);

  bool b_inc = prev_cnt == 0 || dispatch_cnt_inc > 0;
  bool first_inc = prev_cnt == 0 && dispatch_cnt_inc > 0;
  dispatch_cnt_inc = first_inc ? dispatch_cnt_inc + 1 : dispatch_cnt_inc;
  if (b_inc) {
    target.InterlockedAdd(offset, dispatch_cnt_inc, result);
    if (first_inc) {
      uint temp = 0;
      target.InterlockedAdd(offset + 4, 1, temp);
      target.InterlockedAdd(offset + 8, 1, temp);
    }
  }
  return result;
}

struct MeshletBound {
  float3 center;
  float radius;
  /* normal cone axis and cutoff, stored in 8-bit SNORM format; decode using
   * x/127.0 */
  uint packed_cut_off;
  /* dot(center - camera_position, cone_axis) >= cone_cutoff * length(center -
   * camera_position) + radius
   */
  void DecodeCutOff(out float3 cone_axis, out float cut_off) {
    cone_axis =
        float3(float(packed_cut_off & 0x000000FF) * 0.0078740157,
               float((packed_cut_off & 0x0000FF00) >> 8) * 0.0078740157,
               float((packed_cut_off & 0x00FF0000) >> 16) * 0.0078740157);
    cut_off = float((packed_cut_off & 0xFF000000) >> 24) * 0.0078740157;
  }
  uint padding0;
  uint padding1;
  uint padding2;
};

namespace ImportanceSampling {
float GetSpecularLobeHalfAngle(float linear_roughness,
                               float percent_of_volumn = 0.75) {
  float m = linear_roughness * linear_roughness;

  // Comparison of two methods:
  // https://www.desmos.com/calculator/4vvg1qrec7
  // #if 1
  // https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf
  // (page 72)
  // TODO: % of NDF volume - is it the trimming factor from VNDF sampling?
  return atan(m * percent_of_volumn / (1.0 - percent_of_volumn));
  // #else
  //     return Math::DegToRad( 180.0 ) * m / ( 1.0 + m );
  // #endif
}
} // namespace ImportanceSampling

namespace Raster {
float3 PackNormal(float3 n) { return n * 0.5 + 0.5; }
float3 UnpackNormal(float3 n) { return n * 2.0 - 1.0; }
}

namespace Moer {

// uint MBfe(uint src,uint off,uint bits){return
// uint(Bfe(int(src),int(off),int(bits)));} uint MBfiM(uint src,uint ins,uint
// mask){return (ins&mask)|(src&(~mask));}
// // Proxy for V_BFI_B32 where the 'mask' is set as 'bits', 'mask=(1<<bits)-1',
// and 'bits' needs to be an immediate. uint MBfi(uint src,uint ins,uint
// bits){return Bfi(src,ins,0,int(bits));}

// // Simple remap 64x1 to 8x8 with rotated 2x2 pixel quads in quad linear.
// //  543210
// //  ======
// //  ..xxx.
// //  yy...y
// uint2 Remap8x8(uint a){return uint2(MBfe(a,1u,3u),MBfi(MBfe(a,3u,3u),a,1u));}
// // More complex remap 64x1 to 8x8 which is necessary for 2D wave reductions.
// //  543210
// //  ======
// //  .xx..x
// //  y..yy.
// // Details,
// //  LANE TO 8x8 MAPPING
// //  ===================
// //  00 01 08 09 10 11 18 19
// //  02 03 0a 0b 12 13 1a 1b
// //  04 05 0c 0d 14 15 1c 1d
// //  06 07 0e 0f 16 17 1e 1f
// //  20 21 28 29 30 31 38 39
// //  22 23 2a 2b 32 33 3a 3b
// //  24 25 2c 2d 34 35 3c 3d
// //  26 27 2e 2f 36 37 3e 3f
// uint2 RemapRed8x8(uint a){return
// uint2(MBfi(MBfe(a,2u,3u),a,1u),MBfi(MBfe(a,3u,3u),MBfe(a,1u,2u),2u));}

} // namespace Moer
#endif