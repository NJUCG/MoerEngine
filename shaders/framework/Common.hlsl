#ifndef FRAMEWORK_COMMON_HLSL
#define FRAMEWORK_COMMON_HLSL
#include "framework/Math.hlsli"

#define FP16_MAX                            65504.0
#define INF                                 1e5

#define MAX_MIP 11

// Mip mode
#define MIP_VISIBILITY                      0 // for visibility: emission, shadow and alpha mask
#define MIP_LESS_SHARP                      1 // for normal
#define MIP_SHARP                           2 // for albedo and roughness

// BRDF
#define RF0_DIELECTRICS                         0.04
#define GTR_GAMMA                               1.5

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

struct RTConfigParam{
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
    uint32_t denoiser_type;
    uint32_t on_screen;
    uint32_t frame_index;
    uint32_t forced_material;
    uint32_t use_normalmap;
    uint32_t b_worldspace_motion;
    uint32_t tracing_mode;
    uint32_t sample_num;
    uint32_t bounce_num;
    uint32_t taa;
    uint32_t resolve;
    uint32_t psr;
    uint32_t validation;
    uint32_t trim_lobe;
    // uint32_t highlight_ahs;
    // uint32_t ahs_dynamic_mip;

    // Ambient
    float ambient_max_accumulated_frames_num;
    float ambient;
};

#define RTCONFIG_BINDING(t, s) [[vk::binding(t, s)]] ConstantBuffer<RTConfigParam> rt_config : register(b##t, space##s)


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
  float GetSpecularLobeHalfAngle( float linear_roughness, float percent_of_volumn = 0.75 )
  {
    float m = linear_roughness * linear_roughness;

    // Comparison of two methods:
    // https://www.desmos.com/calculator/4vvg1qrec7
    // #if 1
        // https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf (page 72)
        // TODO: % of NDF volume - is it the trimming factor from VNDF sampling?
        return atan( m * percent_of_volumn / ( 1.0 - percent_of_volumn ) );
    // #else
    //     return Math::DegToRad( 180.0 ) * m / ( 1.0 + m );
    // #endif
  }
}

#endif