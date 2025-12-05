

#include <core/common/Bindless.hlsl>
#include <shared/utils/ShaderParameters.h>

[[vk::push_constant]] ConstantBuffer<Moer::ShowTextureParams> param;
[[vk::binding(0, 0)]] Texture2D src_tex : register(t0);

BINDLESS_BINDINGS(1, 2, 3, 4)

static SamplerState g_spl{
  Filter = MIN_MAG_MIP_LINEAR;
  AddressU = CLAMP;
  AddressV = CLAMP;
  AddressW = CLAMP;
  ComparisonFunc = NEVER;
  MipLODBias = 0.0f;
  MinLOD = 1.0f;
  MaxLOD = 3000.f;
};

void GetDimensions(out int2 _dimensions) {
  if (param.bdls_handle >= 0 && param.use_bindless) {
    TextureHandle texture = TextureHandle(param.bdls_handle);
    Texture2D texture2d = texture.GetTexture2D();
    texture2d.GetDimensions(_dimensions.x, _dimensions.y);
  } else {
    src_tex.GetDimensions(_dimensions.x, _dimensions.y);
  }
}

float4 SampleTexture(float2 uv, float mip_level) {
  if (param.bdls_handle >= 0 && param.use_bindless) {
    TextureHandle texture = TextureHandle(param.bdls_handle);
    // Texture2D texture2d = texture.GetTexture2D();
    return texture.SampleLevel(uv, mip_level);
  } else {
    int2 dimensions;
    GetDimensions(dimensions);
    dimensions >>= int(mip_level);
    int2 coord = int2(uv * float2(dimensions));
    if (coord.x < 0 || coord.x >= dimensions.x || coord.y < 0 || coord.y >= dimensions.y) {
      return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return src_tex.Load(int3(int2(coord), int(mip_level)));
    // return src_tex.SampleLevel(g_spl, uv, mip_level);
  }
}

void main(in float4 pos
          : SV_Position, in float2 uv
          : TEXCOORD0, out float4 target
          : SV_Target) {
  int2 dimensions;

  GetDimensions(dimensions);
  float2 dst_dim = float2(param.dst_dim);
  float2 src_dim = float2(dimensions);

  // preserve src texture ratio
  float2 scale = min(dst_dim / src_dim, 1.0f);

  // center the texture, fill the rest with black
  uint4 rect;

  if (scale.x > scale.y) {
    float2 new_dim = src_dim * scale.y;
    float2 offset = (dst_dim - new_dim) / 2.0f;
    rect = uint4(offset, offset + new_dim);
  } else {
    float2 new_dim = src_dim * scale.x;
    float2 offset = (dst_dim - new_dim) / 2.0f;
    rect = uint4(offset, offset + new_dim);
  }

  // sample the texture, write black if out of bounds
  if (pos.x < rect.x || pos.x >= rect.z || pos.y < rect.y || pos.y >= rect.w) {
    target = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  float2 sample_uv = (pos.xy - rect.xy) / (rect.zw - rect.xy);

  float4 color = SampleTexture(sample_uv, float(param.mip_level));
  target = color;
}