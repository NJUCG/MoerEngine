#ifndef MOER_SHARED_SHADER_PARAMETERS_H
#define MOER_SHARED_SHADER_PARAMETERS_H

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#define CONST constexpr
#include "lighting/ShaderParameters.h"
#include "misc/Traits.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"

namespace Moer::Render {
#else
#define CONST const
#include <shared/lighting/ShaderParameters.h>
namespace Moer {
#endif

static CONST uint s_di_light_compact_bit    = 0x80000000u; //mask in ris buffer for compact light
static CONST uint s_di_light_idx_mask       = 0x7fffffffu;
static CONST uint s_di_reservoir_block_size = 16;

static CONST uint s_di_bias_correction_none      = 0; //1/M normaliztion
static CONST uint s_di_bias_correction_basic     = 1; //mis normalization
static CONST uint s_di_bias_correction_pair_wise = 2; //pair-wise mis normalization
static CONST uint s_di_bias_correction_traced    = 3; //unbiased, using traced visibility

//local light initial sample mode
static CONST uint s_di_local_light_sample_mode_uniform   = 0;
static CONST uint s_di_local_light_sample_mode_power_ris = 1; //power based ris
static CONST uint s_di_local_light_sample_mode_grid      = 2; //presample light grid

static CONST uint s_invalid_light_idx = 0xffffffffu;

static CONST uint s_vis_mode_color             = 0;
static CONST uint s_vis_mode_direct_lighting   = 1;
static CONST uint s_vis_mode_emission          = 2;
static CONST uint s_vis_mode_diffuse_lighting  = 3;
static CONST uint s_vis_mode_specular_lighting = 4;
static CONST uint s_vis_mode_grid              = 5;

static CONST uint s_denoiser_mode_off    = 0;
static CONST uint s_denoiser_mode_reblur = 1;
static CONST uint s_denoiser_mode_relax  = 2;

#define DI_SCREEN_TILE_SIZE    16
#define DI_GRAD_FACTOR         3
#define DI_GRAD_STORAGE_SCALE  256.f
#define DI_GRAD_MAX_VALUE      65504.f
#define DI_PRESAMPLE_GRID_SIZE 256

#ifdef __cplusplus
enum RTVisibleMask : uint8 {
#else
enum RTVisibleMask {
#endif
    RTVM_NONE,
    RTVM_DISABLE      = 0x1,
    RTVM_OPAQUE       = 0x2,
    RTVM_TRANSPARANT  = 0x4,
    RTVM_ALPHA_TESTED = 0x8,
    RTVM_ALL          = 0xff
};

enum EFinalColor {
    EFC_SceneColor,
    EFC_DI,
    EFC_EMISSIVE,
    EFC_DIFFUSE,
    EFC_SPECULAR,
    EFC_NORMAL,
    EFC_VIEW_DEPTH,
    EFC_DEPTH,
    EFC_MOTION,
    EFC_GRID,
    EFC_MATERIAL,
    EFC_INSTANCE,
    EFC_POSITION,
    EFC_CUSTOM,
    EFC_NUM
};
#ifdef __cplusplus
enum EMaterialDomain : uint8 {
#else
enum EMaterialDomain {
#endif
    MD_Opaque,
    MD_AlphaTested,
    MD_AlphaBlended,
    MD_Transmissive,
    MD_TransmissiveAlphaTested,
    MD_TransmissiveAlphaBlended,
    MD_Num
};

#ifdef __cplusplus
enum EMaterialFlags : uint {
#else
enum EMaterialFlags {
#endif

    MF_DoubleSided                 = 1 << 0,
    MF_UseMetallicRoughnessTexture = 1 << 1,
    MF_UseBaseColorTexture         = 1 << 2,
    MF_UseEmmissiveTexture         = 1 << 3,
    MF_UseNormalTexture            = 1 << 4,
    MF_UseOcclusionTexture         = 1 << 5,
    MF_UseTransmissionTexture      = 1 << 6
};
struct ViewParam {
    float4x4 view2world;
    float4x4 world2view;
    float4x4 world2clip;
    float4x4 clip2view;
    float4x4 view2clip;
    float4x4 clip2world;

    float4 frustum;

    float2 near_far;
    float2 rect;
    float2 inv_rect;
    float2 jitter;

    float2 clip2window_scale;
    float2 clip2window_bias;
    float2 window2clip_scale;
    float2 window2clip_bias;

    float4 dir_or_pos; //dir for ortho, pos for perspective//w 0 for dir
};
struct GBufferConstants {
    ViewParam main_view;
    ViewParam prev_view;
};

struct DIReservoirParams {
    uint block_row_pitch;
    uint block_col_pitch;
    uint padding0;
    uint padding1;
};

struct LightRegion {
    uint first_light_idx;
    uint light_cnt;
    uint padding0;
    uint padding1;
};

struct GBufferPassParams {
    // 场景结构数据
    uint instance_buf_hdl;  // GInstance[]
    uint primitive_buf_hdl; // GPrimitive[]
    uint material_buf_hdl;  // GMaterial[]

    // MegaBuffers
    uint position_buf_hdl;       // CtxMegaBuffers.position
    uint packed_normal_buf_hdl;  // CtxMegaBuffers.packed_normal
    uint packed_tangent_buf_hdl; // CtxMegaBuffers.packed_tangent
    uint texcoord0_buf_hdl;      // CtxMegaBuffers.texcoord0
    uint index_buf_hdl;          // CtxMegaBuffers.index

    // RT 专用（mesh-level BLAS 方案）
    uint rt_instance_buf_hdl;          // GRtInstance[]（per-renderable）
    uint rt_primitive_table_buf_hdl;   // uint[]（GeometryIndex → primitive_id 映射表）
};

struct RaytracingBindlessHandles {
    uint gbuffer_depth;
    uint gbuffer_normal;
    uint gbuffer_diffuse_albedo;
    uint gbuffer_specular_roughness;
    //previous frame
    uint gbuffer_prev_depth;
    uint gbuffer_prev_normal;
    uint gbuffer_prev_diffuse_albedo;
    uint gbuffer_prev_specular_roughness;

    uint restir_prev_luminance;
    uint motion;
    uint denoiser_normal_roughness;

    // // deprecated gpu scene
    // uint geom_data;
    // uint instance_data;
    // uint material_data;

    // gpu scene (FIXME: 删除不要的hdl)
    uint light_buf_hdl;
    uint material_buf_hdl;

    uint primitive_buf_hdl;
    uint instance_buf_hdl;

    uint position_buf_hdl;
    uint packed_normal_buf_hdl;
    uint packed_tangent_buf_hdl;
    uint texcoord0_buf_hdl;

    uint index_buf_hdl;

    // RT 专用（mesh-level BLAS 方案）
    uint rt_instance_buf_hdl;
    uint rt_primitive_table_buf_hdl;

    //lighting
    uint poly_light_data;
    uint light_index;

    uint restir_luminance;
    uint primitive_to_light;
    uint local_light_pdf;
    uint env_pdf;
};

struct SceneGlobalParams {
    uint  enable_env_map;
    uint  env_map_handle;
    float env_map_scale;
    float env_map_rotation;
};

struct ResampleConstants {
    ViewParam                 main_view;
    ViewParam                 prev_view;
    DI::CommonParams          di_params;
    RaytracingBindlessHandles bindless_handles;

    SceneGlobalParams          scene_params;
    DI::LightBufferParams      light_buffer_params;
    DI::RISBufferSegmentParams local_light_ris_buffer_params;
    DI::RISBufferSegmentParams env_light_ris_buffer_params;

    DI::ReSTIRDIParams restir_di_params;
    Grid::Params       grid_params;

    float4 reblur_diff_hit_dist_params;
    float4 reblur_spec_hit_dist_params;

    uint frame_idx;
    uint enable_accumulation;
    uint discount_native_samples;
    uint visualize_cells;

    uint2 env_pdf_size;
    uint2 local_light_pdf_size;

    uint enable_prev_tlas;
    uint denoiser_mode;
};

struct CompositingConstants {
    ViewParam main_view;
    ViewParam prev_view;

    uint enable_textures;
    uint denoiser_mode;
    uint enable_env_map;
    uint env_map_handle;

    float env_scale;
    float env_rotation;
};

struct VisualizeParams {
    ViewParam    main_view;
    Grid::Params grid_params;

    uint2  output_size;
    float2 resolution_scale;

    uint  visualize_mode;
    uint  b_split;
    float split_ratio;
    uint  padding0;
};

struct ConfidenceParams {
    uint2  rect_size;
    float2 inv_grad_size;

    float darkness_bias;
    float sensitivity;
    int   input_buf_idx;
    float blend_factor;
};

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST

#endif //MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H
