#ifndef FRAMEWORK_MATERIAL_HLSLI
#define FRAMEWORK_MATERIAL_HLSLI

#include "core/common/Bindless.hlsl"

template <typename T>
T GetTextureData(int bindless_handle, float2 uv, T default_value, T missing_value) {
  if (bindless_handle > 0) {
    // 可以通过禁用mipmap来消除mesh之间的描边
    return TextureHandle(bindless_handle).Sample2D<T>(uv);
    // return TextureHandle(bindless_handle).SampleLevel<T>(uv, 0.0);

  } else if (bindless_handle == -1) {
    return default_value;

  } else {
    // assert bindless_handle == -2
    // use -2 presents missing texture
    return missing_value;
  }
}

// Compatibility wrapper: main renamed GetTextureData -> SampleTextureAndApplyFactor
// with different default-value semantics (handle >= 0 valid, multiplies by factor).
template <typename T>
T SampleTextureAndApplyFactor(int bindless_handle, float2 uv, T factor, T missing_value) {
    T sample_value = (T)1.0;
    if (bindless_handle >= 0) {
        sample_value = TextureHandle(bindless_handle).Sample2D<T>(uv);
    } else {
        sample_value = missing_value;
    }
    return sample_value * factor;
}

float3 GetNormalFromNormalMap(int normal_map, float2 uv, float3 normal, float3 tangent) {
  if (normal_map >= 0) {
    float3 normal_in_tbn = normalize((TextureHandle(normal_map).Sample2D<float3>(uv) * 2.0) - 1.0); // Mipmap采样后的法线需要normalize
    float3 bitangent = cross(normal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    return normalize(mul(normal_in_tbn, tbn)); // TBN变换不保证长度不变，所以需要normalize
  } else {
    return normal;
  }
}



#define Material_Standard_PBR 0

namespace BRDF {

namespace IOR {
static const float Air = 1.0f;
static const float Glass = 1.5f;
static const float Water = 1.33f;
static const float Diamond = 2.42f;
static const float Vaccuum = 1.0f;
static const float Ice = 1.31f;
static const float Quartz = 1.46f;
static const float FusedSilica = 1.46f;
static const float SodiumChloride = 1.54f;
static const float Fluorite = 1.43f;
static const float Pyrex = 1.47f;
static const float Acrylic = 1.49f;
static const float Polystyrene = 1.59f;
static const float Polyethylene = 1.51f;
static const float Polypropylene = 1.49f;
} // namespace IOR

} // namespace BRDF
#endif