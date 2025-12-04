#include "lighting/common/Lighting.hlsl"
#include "materials/Material.hlsl"

struct LightingData {
  float4x4 inv_view_proj;
  uint light_count;
  uint3 padding;
  float3 camera_position;
};

 [[vk::binding(0, 2)]] Texture2D scene_textures[25];
// [[vk::binding(0, 2)]] Texture2D scene_texture;

static const uint CUR_MATERIAL_TYPE = 0;

[[vk::binding(0, 0)]] StructuredBuffer<MaterialData> material_data
    : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<Light> light_data : register(t1, space0);
[[vk::push_constant]] ConstantBuffer<LightingData> lighting_data : register(b0);

[[vk::binding(0, 1)]] Texture2D<uint> mat_attach;
[[vk::binding(1, 1)]] Texture2D<float4> normal_attach;
[[vk::binding(2, 1)]] Texture2D<float2> gbuffer_uv;
[[vk::binding(3, 1)]] Texture2D depth_attach;

[[vk::binding(4, 1)]] SamplerState default_sampler;

// [[vk::input_attachment_index(1), vk::binding(1)]] SubpassInput depthAttach;
float3 worldPosFromDepth(float depth, float2 in_uv) {
  float4 clip = float4(in_uv.x * 2.f - 1.f, 1.f - in_uv.y * 2.f, depth, 1.0);
  float4 world_w = mul(lighting_data.inv_view_proj, clip);
  float3 pos = world_w.xyz / world_w.w;
  return pos;
}
float4 main(float2 in_uv
            : TEXCOORD0, float4 position
            : SV_POSITION)
    : SV_TARGET {
  uint gbuffer_mat = mat_attach.Sample(default_sampler, in_uv);
  //  printf("uv %f %f svposition %f %f %f \n", in_uv.x, in_uv.y, position.x,
  //  position.y, position.z);
  uint mat_type = gbuffer_mat & 0x000000FF;
  float2 uv = gbuffer_uv.Sample(default_sampler, in_uv);
  uint mat_id = (gbuffer_mat & 0xFFFFFF00) >> 8;
  MaterialData mat = material_data[NonUniformResourceIndex(mat_id)];
  float4 base_color;
  if (mat.albedo_map == -1) {
    base_color = mat.base_color_factor;
  } else {
     base_color = scene_textures[NonUniformResourceIndex(mat.albedo_map)].Sample(
         default_sampler, uv);
	//base_color = float4(uv, 0.0f, 1.0f);
  }
  float3 result = float3(0.0f, 0.0f, 0.0f);
  float3 normal = (normal_attach.Sample(default_sampler, in_uv).xyz - 0.5f) * 2;
  float depth = depth_attach.Sample(default_sampler, in_uv).x;
  float3 world_pos = worldPosFromDepth(depth, in_uv);
  for (uint i = 0; i < lighting_data.light_count; i++) {
    Light light = light_data[i];

    // result += base_color.xyz * apply_light(light, world_pos, normal);
    result += apply_light_blinn_phong(light, world_pos, normal, base_color.xyz, float3(0.4f, 0.4f, 0.4f), lighting_data.camera_position);
  }
  return float4(result, 1.0f);
}