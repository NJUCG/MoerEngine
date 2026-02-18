#ifndef MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H
#define MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H

#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
#define MARK_USER_TRIVIAL_TYPE()
namespace Moer {
#endif
#define LIGHT_TASK_PRIMITIVE_LIGHT_BIT 0x80000000

struct PrepareLightsTask {
    uint primitive_id; // primitive_id（对于自发光三角形）或 prim_light_idx（对于场景光源）
    uint num_triangles;
    uint light_offset;       // offset in light buffer
    int  prev_light_offset;  // offset in prev-frame light buffer, -1 if not exist
    uint index_start_idx;    // 新架构：索引在 MegaBuffer 中的起始位置（元素偏移，不是字节偏移）
    uint first_instance_idx; // 新架构：第一个 Instance 在 m_instance_buf 中的索引（用于获取 world_transform）
};

#ifdef __cplusplus
enum class EPolyLightType : uint
#else
enum EPolyLightType
#endif
{
    ELSphere = 0,
    ELDisk,
    ELCylinder,
    ELRect,
    ELTriangle,
    ELDirectional,
    ELEnv,
    ELPoint,
    ELTriangleIndirect
};
static const float g_poly_morphic_light_max_radiance      = 1e4f;
static const float g_poly_morphic_light_max_flux          = 1.0f;
static const uint  g_poly_morphic_light_type_shift        = 24;
static const uint  g_poly_morphic_light_type_mask         = 0xf;
static const float g_poly_morphic_light_min_log2_radiance = -8.f;
static const float g_poly_morphic_light_max_log2_radiance = 40.f;
static const uint  g_poly_morphic_light_shaping_bit       = 1 << 28;
static const uint  g_poly_morphic_light_env_is_scalar_bit = 1 << 16;
static const uint  g_poly_morphic_light_log_radiance_mask = 0xffff;
static const uint  g_task_prim_light_bit                  = 0x80000000u;

//////////////////////////////////////////////////////////////////////////
//Common definitions
//////////////////////////////////////////////////////////////////////////
struct PolymorphicLightInfo {
    MARK_USER_TRIVIAL_TYPE();

    float3 center;
    uint   color_type_flags;

    uint direction1;
    uint direction2;
    uint scalars;      //fp16x2
    uint log_radiance; //uint16

    //light shaping
    uint profile_idx;
    uint primary_axis;            //oct-encode axis
    uint cos_cone_angle_softness; //fp16x2
    uint reserved;
};

struct RLightInfo {
    MARK_USER_TRIVIAL_TYPE();

    float3 center;
    uint   scalars; //fp16x2

    uint2 radiance;   //fp16x4
    uint  direction1; //oct-encode
    uint  direction2; //oct-encode
};

struct PrepareLightsParams {
    // 新架构：使用 GPrimitive, GInstance, GMaterial 和 MegaBuffers
    uint primitive_buf_hdl;  // GPrimitive 数组的 bindless handle
    uint instance_buf_hdl;   // GInstance 数组的 bindless handle
    uint material_buf_hdl;   // GMaterial 数组的 bindless handle
    uint position_buf_hdl;   // CtxMegaBuffers.position 的 bindless handle
    uint index_buf_hdl;      // CtxMegaBuffers.index 的 bindless handle
    uint texcoord0_buf_hdl;  // CtxMegaBuffers.texcoord0 的 bindless handle（用于自发光纹理）
    uint primitive_to_light; // primitive_id -> light_offset 映射数组的 bindless handle

    uint num_tasks;
    uint cur_light_offset;
    uint prev_light_offset;
};

struct EnvironmentMapParams {};

struct PreprocessEnvironmentMapParams {
    uint2 src_size;
    uint  src_mip_level;
    uint  num_mip_levels;
};

namespace Grid {
struct CommonParams {
    uint  local_light_sampling_fallback_mode;
    float center_x;
    float center_y;
    float center_z;

    uint  ris_buffer_offset;
    uint  lights_per_cell;
    float cell_size;
    float jitter;

    uint local_light_sample_mode;
    uint num_build_samples;
    uint padding0;
    uint padding1;
};

struct GridParameters {
    uint cell_x;
    uint cell_y;
    uint cell_z;
    uint padding0;
};

struct Params {
    CommonParams   common_params;
    GridParameters grid_params;
};
}; // namespace Grid

namespace DI {

struct PackedReservoir {
    uint  light_data;
    uint  uv_data;
    uint  visibility;
    uint  distance_age;
    float target_pdf;
    float weight;
};

struct LightBufferRegion {
    uint first_light_idx;
    uint light_cnt;
    uint padding0;
    uint padding1;
};

struct CommonParams {
    uint neighbor_offset_mask;
    uint padding0;
    uint padding1;
    uint padding2;
};

struct EnvLightParams {
    uint light_cnt;
    uint light_idx;
    uint padding0;
    uint padding1;
};

struct LightBufferParams {
    LightBufferRegion local_light_region;
    LightBufferRegion infinite_light_region;
    EnvLightParams    env_light;
};

struct ReservoirBufferParams {
    uint block_row_pitch;
    uint block_array_pitch;
    uint padding0;
    uint padding1;
};

struct RISBufferSegmentParams {
    uint buffer_offset;
    uint tile_size;
    uint tile_cnt;
    uint padding0;
};

//////////////////////////////////////////////////////////////////////////
//ReSTIR DI
//////////////////////////////////////////////////////////////////////////

struct ReSTIRDIBufferIndices {
    uint initial_sample_output_buff_idx;
    uint temperal_resample_input_buff_idx;
    uint temperal_resample_output_buff_idx;
    uint spatial_resample_input_buff_idx;

    uint spatial_resample_output_buff_idx;
    uint shading_input_buff_idx;
    uint padding0;
    uint padding1;
};

struct ReSTIRDIInitialSampleParams {
    uint num_primary_local_lights;
    uint num_primary_infinite_lights;
    uint num_primary_env_lights;
    uint num_primary_brdf_lights;

    float brdf_cutoff;
    uint  enable_initial_visiblity;
    uint  env_map_is; //importance sampling
    uint  local_light_sample_mode;
};

struct ReSTIRDITemporalResampleParams {
    float depth_threshold;
    float normal_threshold;
    uint  max_history_length;
    uint  bias_correction_mode;

    uint  enable_permutation_sample; //reduce temporal correlation
    float permutation_sample_threshold;
    uint  enbale_boiling_filter;
    float boiling_filter_scale;

    uint discard_inviable_samples;
    uint random_number;
    uint padding0;
    uint padding1;
};

struct ReSTIRDISpatialResampleParams {
    float depth_threshold;
    float normal_threshold;
    uint  bias_correction_mode;
    uint  num_spatial_samples;

    uint  num_disocclusion_samples;
    float radius;
    uint  neighbor_offset_mask;
    uint  discount_native_samples;
};

struct ReSTIRDIShadingParams {
    uint  enable_final_visiblity;
    uint  reuse_final_visiblity;
    uint  final_visiblity_max_age;
    float final_visiblity_max_distance;

    uint enable_denoiser_input_packing;
    uint padding0;
    uint padding1;
    uint padding2;
};

struct ReSTIRDIParams {
    ReservoirBufferParams          reservoir_buffer_params;
    ReSTIRDIBufferIndices          buffer_indices;
    ReSTIRDIInitialSampleParams    initial_sample_params;
    ReSTIRDITemporalResampleParams temporal_resample_params;
    ReSTIRDISpatialResampleParams  spatial_resample_params;
    ReSTIRDIShadingParams          shading_params;
};
}; // namespace DI

#ifdef __cplusplus
}
#else
}
#endif

#endif //MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H