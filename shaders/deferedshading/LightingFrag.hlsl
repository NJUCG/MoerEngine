#include "framework/Lighting.hlsl"
#include "framework/Material.hlsl"

struct LightingData {
  float4x4 inv_view_proj;
  uint light_count;
  uint3 padding;
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
  float4 clip = float4(in_uv * 2.0 - 1.0, depth, 1.0);
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

  // if(mat_type != CUR_MATERIAL_TYPE)
  // {
  //     discard;
  // }

  //  return float4(in_uv, 0.0f, 1.0f);
  float2 uv = gbuffer_uv.Sample(default_sampler, in_uv);
  uint mat_id = (gbuffer_mat & 0xFFFFFF00) >> 8;
  MaterialData mat = material_data[mat_id];
  float4 base_color;
  if (mat.albedo_map == -1) {
    base_color = 0.f;
  } else {
    base_color =
        scene_textures[mat.albedo_map].SampleLevel(default_sampler, uv, 0.0f);
    // base_color = scene_texture.Sample(default_sampler, in_uv);
  }
  float3 result = float3(0.0f, 0.0f, 0.0f);
  float3 normal = (normal_attach.Sample(default_sampler, in_uv).xyz - 0.5f) * 2;
  float depth = depth_attach.Sample(default_sampler, in_uv).x;
  float3 world_pos = worldPosFromDepth(depth, in_uv);
  for (uint i = 0; i < lighting_data.light_count; i++) {
    Light light = light_data[i];
    result += base_color.xyz * apply_light(light, world_pos, normal);
  }
  // return float4(in_uv, 0.0f, 1.0f);
  // return float4(base_color);
  return float4(result, 1.0f);
}