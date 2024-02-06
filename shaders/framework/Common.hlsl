#ifndef FRAMEWORK_COMMON_HLSL
#define FRAMEWORK_COMMON_HLSL
struct CameraData{
    float4x4 view;
  float4x4 proj;
  float4x4 inv_view;
  float4x4 inv_proj;
};

struct InstanceData{
    float4x4 model2world;
    float4x4 inv_model2world;
    uint material_id;
    uint material_type;
};
#endif