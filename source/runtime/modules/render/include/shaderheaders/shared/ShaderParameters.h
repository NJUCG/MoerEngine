#ifndef MOER_SHARED_SHADER_PARAMETERS_H
#define MOER_SHARED_SHADER_PARAMETERS_H

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#define CONST constexpr
#include "shaderheaders/shared/lighting/ShaderParameters.h"
#include "misc/Traits.h"
#include "lighting/ShaderParameters.h"

namespace Moer::Render {
#else
#define CONST const
#include <shared/lighting/ShaderParameters.h>
namespace Moer {
#endif

    static CONST uint s_di_light_compact_bit    = 0x80000000u;//mask in ris buffer for compact light
    static CONST uint s_di_light_idx_mask       = 0x7fffffffu;
    static CONST uint s_di_reservoir_block_size = 16;

    static CONST uint s_di_bias_correction_none      = 0;//1/M normaliztion
    static CONST uint s_di_bias_correction_basic     = 1;//mis normalization
    static CONST uint s_di_bias_correction_pair_wise = 2;//pair-wise mis normalization
    static CONST uint s_di_bias_correction_traced    = 3;//unbiased, using traced visibility

    //local light initial sample mode
    static CONST uint s_di_local_light_sample_mode_uniform   = 0;
    static CONST uint s_di_local_light_sample_mode_power_ris = 1;//power based ris
    static CONST uint s_di_local_light_sample_mode_grid      = 2;//presample light grid

    static CONST uint s_invalid_light_idx = 0xffffffffu;

    static CONST uint s_vis_mode_color             = 0;
    static CONST uint s_vis_mode_direct_lighting   = 1;
    static CONST uint s_vis_mode_emission          = 2;
    static CONST uint s_vis_mode_diffuse_lighting  = 3;
    static CONST uint s_vis_mode_specular_lighting = 4;
    static CONST uint s_vis_mode_grid              = 5;

#define DI_SCREEN_TILE_SIZE 16

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
        EFC_NUM
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

        float4 dir_or_pos;//dir for ortho, pos for perspective//w 0 for dir
    };
    struct GBufferConstants {
        ViewParam main_view;
        ViewParam prev_view;
    };

    struct DIParams {
        uint neigbor_offset_mask;//spatial reuse
        uint padding;
        uint padding1;
        uint padding2;
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
        uint geometry_instance_handle;
        uint geometry_data_handle;
        uint instance_data_handle;
        uint material_data_handle;
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

        uint gbuffer_prev_luminance;
        uint motion;
        uint denoiser_normal_roughness;

        //gpu scene
        uint geom_data;

        uint instance_data;
        uint material_data;

        //lighting
        uint poly_light_data;
        uint light_index;

        uint neighbor_offset;
        uint geo_instance_to_light;
        uint local_light_pdf;
        uint env_pdf;

        uint env_map;
        uint padding0;
        uint padding1;
        uint padding2;
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

        uint frame_idx;
        uint enable_accumulation;
        uint discount_native_samples;
        uint visualize_cells;

        uint2 env_pdf_size;
        uint2 local_light_pdf_size;

        uint enable_prev_tlas;
    };

    struct VisualizeParams {
        ViewParam                 main_view;
        RaytracingBindlessHandles bindless_handles;
        Grid::Params              grid_params;

        uint2  output_size;
        float2 resolution_scale;

        uint  visualize_mode;
        uint  b_split;
        float split_ratio;
        uint  padding0;
    };

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST

#endif//MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H