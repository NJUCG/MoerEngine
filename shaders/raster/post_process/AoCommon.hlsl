struct AoOutput {
    float4 color_with_ao : SV_Target0;
    float ambient_only : SV_Target1;
    float2 camera_motion_vector : SV_Target2;
};

float2 GetCameraMotionVector() {
    return float2(0.0, 0.0); // TODO
}