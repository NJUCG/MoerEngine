#ifndef FRAMEWORK_MATERIAL_HLSL
#define FRAMEWORK_MATERIAL_HLSL

// #include "framework/Bindless.hlsl"
struct MaterialData {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float ao;
  uint albedo_map;
  int normal_map;
  int metallic_roughness_map;
  int ao_map;
  int emissive_map;
  int padding;
};
template <typename T>
T UnpackMaterialData(uint material_buffer_handle, uint material_index) {
  ArrayBuffer buf = ArrayBuffer(material_buffer_handle);
  return buf.Load<T>(0, material_index * 512);
}

#define Material_Standard_PBR 0

#endif