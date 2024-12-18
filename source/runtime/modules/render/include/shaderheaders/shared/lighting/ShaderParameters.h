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
    enum class PolyLightType : uint
#else
    enum PolyLightType
#endif
    {
        ELSphere = 0,
        ELDisk,
        ELRect,
        ELTriangle,
        ELDirectional,
        ELEnv,
        ELPoint
    };
    static const uint  g_poly_morphic_light_shift             = 24;
    static const float g_poly_morphic_light_min_log2_radiance = -8.f;
    static const float g_poly_morphic_light_max_log2_radiance = 40.f;
    static const uint  g_poly_morphic_light_shaping_bit       = 1 << 28;
    static const uint  g_poly_morphic_light_env_is_scalar_bit = 1 << 16;
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
        uint num_tasks;
        uint current_frame_light_offset;
        uint previous_frame_light_offset;
    };

    struct EnvironmentMapParams {
    };

    struct PreprocessEnvironmentMapParams {
        uint2 src_size;
        uint  src_mip_level;
        uint  num_mip_levels;
    };

#ifdef __cplusplus
}
#else
}
#endif

#endif//MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H