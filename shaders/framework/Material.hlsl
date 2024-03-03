#ifndef FRAMEWORK_MATERIAL_HLSL
#define FRAMEWORK_MATERIAL_HLSL
struct MaterialData {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float ao;
  uint albedo_map;
  uint normal_map;
  uint metallic_roughness_map;
  uint ao_map;
  uint emissive_map;
  uint padding;
};
#endif