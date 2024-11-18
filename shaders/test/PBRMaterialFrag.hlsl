#include "framework/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "framework/Common.hlsl"
#include "framework/Lighting.hlsl"
#include "framework/Material.hlsl"

struct LightingData {
  float4x4 inv_view_proj;
  uint light_count;
  uint3 padding;
  float3 camera_position;
};

static const uint CUR_MATERIAL_TYPE = 0;

struct PackedMaterialData {
  float4 packed_0;
  float4 packed_1;
  float4 packed_2;
  float4 packed_3;

  float4 packed_4;
  float4 packed_5;
  float4 packed_6;
  float4 packed_7;
};

struct Constant {
  uint material_type;
  uint light_buffer;
  uint material_buffer;
  uint vbuffer;
  uint gbuffer_normal;
  uint gbuffer_uv;
};

[[vk::push_constant]] ConstantBuffer<Constant> param;

// [[vk::input_attachment_index(1), vk::binding(1)]] SubpassInput depthAttach;
// float3 worldPosFromDepth(float depth, float2 in_uv) {
//     float4 clip    = float4(in_uv.x * 2.f - 1.f, 1.f - in_uv.y * 2.f,
//     depth, 1.0); float4 world_w = mul(lighting_data.inv_view_proj, clip);
//     float3 pos     = world_w.xyz / world_w.w;
//     return pos;
// }
float4 main(float2 in_uv
            : TEXCOORD0, float4 position
            : SV_POSITION)
    : SV_TARGET {
  TextureHandle mat_attach = TextureHandle(param.vbuffer);

  uint gbuffer_mat = TextureHandle(param.vbuffer).Sample2D<uint>(in_uv);
  uint mat_type = gbuffer_mat & 0x000000FF;
  if (mat_type != param.material_type) {
    printf("mat_type:%d, param.material_type:%d\n", mat_type,
           param.material_type);
    discard;
  }
  float2 uv = TextureHandle(param.gbuffer_uv).Sample2D<float2>(in_uv);
  //  return float4(uv, 0.0, 1.0);
  uint mat_id = (gbuffer_mat & 0xFFFFFF00) >> 8;
  MaterialData mat =
      UnpackMaterialData<MaterialData>(param.material_buffer, mat_id);
  float4 base_color;
  if (mat.albedo_map == -1) {
    base_color = mat.base_color_factor;
  } else {
    // printf("mat.albedo_map:%d\n", mat.albedo_map);
    base_color = TextureHandle(mat.albedo_map).Sample2D<float4>(uv);
  }
  return float4(base_color.xyz, 1.0f);
}