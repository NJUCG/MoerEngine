#include "core/math/Math.hlsli"
#include "pipelines/RasterCommon.hlsli"
#include "pipelines/postprocess/lighting_effects/AoGeometry.hlsli"

struct AoOutput {
    float  ambient_only : SV_Target0;
    float2 camera_motion_vector : SV_Target1;
};

// 结果是 NDC Space，目标为当前像素，上一帧指向当前帧
float2 GetCameraMotionVector(float2 uv) {
    // camera_mv.world2clip
    // camera_mv.world2clip_prev
    // param.depth_tex

    // MARK: Lighting Data
    ArrayBuffer                  camera_mv_data = ArrayBuffer(moer_constants.param.camera_mv_data_handle);
    Moer::CameraMotionVectorData camera_mv      = camera_mv_data.Load<Moer::CameraMotionVectorData>(0);

    AoSurface surface = LoadAoSurface(uv, moer_constants.param.depth_tex, moer_constants.param.normal_tex);
    if (!IsAoSurfaceValid(surface)) {
        return float2(0.0, 0.0);
    }

    float3 world_pos_vec3 = WorldPosFromDepth(surface.depth, surface.uv, moer_constants.param.clip2world);
    float4 world_pos      = float4(world_pos_vec3, 1.0);

    float4 clip_pos_prev = mul(camera_mv.world2clip_prev, world_pos);
    if (clip_pos_prev.w <= Epsilon) {
        return float2(4.0, 4.0); // Outside the history image, including points behind the previous camera.
    }

    // calculate in NDC space
    // Motion starts at the AO output pixel, not at its snapped GBuffer texel center.
    float2 output_ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float2 mv = output_ndc - (clip_pos_prev.xy / clip_pos_prev.w);

    // // for debug, map [-1, 1] to 0/0.5/1
    // if (mv.x > Epsilon) {
    //     mv.x = 1.0;
    // } else if (mv.x < -Epsilon) {
    //     mv.x = 0.0;
    // } else {
    //     mv.x = 0.5;
    // }
    // if (mv.y > Epsilon) {
    //     mv.y = 1.0;
    // } else if (mv.y < -Epsilon) {
    //     mv.y = 0.0;
    // } else {
    //     mv.y = 0.5;
    // }

    return mv;
}
