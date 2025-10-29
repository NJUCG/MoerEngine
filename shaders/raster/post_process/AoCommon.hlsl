struct AoOutput {
    float4 color_with_ao : SV_Target0;
    float ambient_only : SV_Target1;
    float2 camera_motion_vector : SV_Target2;
};

static const float Epsilon = 0.0001; // TODO: same with PBRMaterialFrag.hlsl

float2 GetCameraMotionVector(float2 uv) {
    // camera_mv.world2clip
    // camera_mv.world2clip_prev
    // param.depth_tex
    // param.position_tex

    
    // MARK: Lighting Data
    ArrayBuffer camera_mv_data = ArrayBuffer(param.camera_mv_data_handle);
    Moer::CameraMotionVectorData camera_mv = camera_mv_data.Load<Moer::CameraMotionVectorData>(0);
    
    float depth = TextureHandle(param.depth_tex).Sample2D<float>(uv);
    if (depth < Epsilon) {
        return float2(0.0, 0.0);
    }
    
    float3 world_pos_vec3 = TextureHandle(param.position_tex).Sample2D<float3>(uv);
    float4 world_pos = float4(world_pos_vec3, 1.0);

    float4 clip_pos = mul(camera_mv.world2clip, world_pos);
    float4 clip_pos_prev = mul(camera_mv.world2clip_prev, world_pos);

    // calculate in NDC space
    float2 mv = (clip_pos.xy / clip_pos.w) - (clip_pos_prev.xy / clip_pos_prev.w);

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