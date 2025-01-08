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
        uint instance_geo_idx;//highest bit is LIGHT_TASK_PRIMITIVE_LIGHT_BIT, mid 19 bits is instance idx, low 12 bits is geo idx
        uint num_triangles;
        uint light_offset;     //offset in light buffer
        int  prev_light_offset;//offset in prev-frame light buffer, -1 if not exist
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
        ELPoint
    };
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
        uint scalars;     //fp16x2
        uint log_radiance;//uint16

        //light shaping
        uint profile_idx;
        uint primary_axis;           //oct-encode axis
        uint cos_cone_angle_softness;//fp16x2
        uint reserved;
    };

    struct RLightInfo {
        MARK_USER_TRIVIAL_TYPE();

        float3 center;
        uint   scalars;//fp16x2

        uint2 radiance;  //fp16x4
        uint  direction1;//oct-encode
        uint  direction2;//oct-encode
    };

    struct PrepareLightsParams {
        uint instance_data_handle;
        uint geometry_data_handle;
        uint material_data_handle;
        uint num_tasks;
        uint cur_light_offset;
        uint prev_light_offset;
    };

    struct EnvironmentMapParams {
    };

    struct PreprocessEnvironmentMapParams {
        uint2 src_size;
        uint  src_mip_level;
        uint  num_mip_levels;
    };

    namespace GridLights {
        struct CommonParams {
            uint  local_light_sampling_mode;
            float center_x;
            float center_y;
            float center_z;

            uint  ris_buffer_offset;
            uint  lights_per_ceil;
            float ceil_size;
            float jitter;

            uint local_light_presample_mode;
            uint num_build_samples;
            uint padding0;
            uint padding1;
        };

        struct GridParameters {
            uint ceil_x;
            uint ceil_y;
            uint ceil_z;
            uint padding0;
        };

        struct Params {
            CommonParams   common_params;
            GridParameters grid_params;
        };
    };// namespace GridLights

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
            uint light_valid;
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
            uint tile_count;
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
            uint  env_map_is;//importance sampling
            uint  local_light_sample_mode;
        };

        struct ReSTIRDITemporalResampleParams {
            float depth_threshold;
            float normal_threshold;
            uint  max_history_length;
            uint  bias_correction_mode;

            uint  enable_permutation_sample;//reduce temporal correlation
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
    };// namespace DI

#ifdef __cplusplus
}
#else
}
#endif

#endif//MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H