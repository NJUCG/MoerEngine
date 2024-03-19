#ifndef FRAMEWORK_MATERIAL_HLSL
#define FRAMEWORK_MATERIAL_HLSL
struct MaterialData {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float ao;
  int albedo_map;
  int normal_map;
  int metallic_roughness_map;
  int ao_map;
  int emissive_map;
  int padding;
};
#endif