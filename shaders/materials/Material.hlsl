#ifndef FRAMEWORK_MATERIAL_HLSL
#define FRAMEWORK_MATERIAL_HLSL

// #include "core/common/Bindless.hlsl"

template <typename T>
T UnpackMaterialData(uint material_buffer_handle, uint material_index) {
  ArrayBuffer buf = ArrayBuffer(material_buffer_handle);
  return buf.Load<T>(0, material_index * 512);
}

template <typename T>
T UnpackMaterialData(ByteAddressBuffer _material_buffer, uint material_index) {
  return _material_buffer.Load<T>(material_index * 512);
}

template <typename T>
T GetTextureData(int bindless_handle, float2 uv, T default_value, T missing_value) {
  if (bindless_handle >= 0) {
    return TextureHandle(bindless_handle).Sample2D<T>(uv);

  } else if (bindless_handle == -1) {
    return default_value;

  } else {
    // assert bindless_handle == -2
    // use -2 presents missing texture
    return missing_value;
  }
}

float3 GetNormalFromNormalMap(int normal_map, float2 uv, float3 normal, float3 tangent) {
  if (normal_map >= 0) {
    float3 normal_in_tbn = (TextureHandle(normal_map).Sample2D<float3>(uv) * 2.0) - 1.0;
    float3 bitangent = cross(normal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    return normalize(mul(normal_in_tbn, tbn));
    // return mul(tbn, normal_in_tbn);
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