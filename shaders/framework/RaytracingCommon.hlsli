#ifndef MOER_RAYTRACING_COMMON_HLSLI
#define MOER_RAYTRACING_COMMON_HLSLI
namespace Moer {
enum EGeometryAttrib {
  EGA_Position = 0x1,
  EGA_PrevPosition = 0x2,
  EGA_UV = 0x4,
  EGA_Normal = 0x8,
  EGA_Tangent = 0x10,
  EGA_All = 0x1f
};

struct GeometryRecord {
  Moer::InstanceData instance;
  Moer::GeometryData geometry;
  MaterialData material;

  float3 vtx_positions[3];
  float2 vtx_uvs[3];

  float3 model_pos;
  float3 model_pos_prev;
  float2 texcoord;
  float3 normal;
  float3 tangent;
};

struct MaterialSample {
  float3 normal;
  float3 diffuse_albedo;
  float3 specular_f0;
  float3 emissive;
  float opacity;

  float occlusion;
  float roughness;

  float3 base_color;
  float metalness;
  float transmission;

  static MaterialSample ConstructDefault() {
    MaterialSample result = (MaterialSample)0;
    result.normal = 0.f;
    result.diffuse_albedo = 0.f;
    result.specular_f0 = 0.f;
    result.emissive = 0.f;
    result.opacity = 1.f;

    result.occlusion = 0.f;
    result.roughness = 0.f;

    result.base_color = 0.f;
    result.metalness = 0.f;
    result.transmission = 0.f;

    return result;
  }
};

enum EMaterialAttribute {
  EMA_BaseColor = 0x1,
  EMA_Normal = 0x2,
  EMA_MetalRough = 0x4,
  EMA_Emissive = 0x8,
  EMA_Transmission = 0x10,

  EMA_All = 0x1f
};

GeometryRecord GetGeometryRecordFrom(uint _instance_idx, uint _geometry_idx,
                                     uint _prim_idx, float2 _ray_barycentrics,
                                     EGeometryAttrib _attrib,
                                     ByteAddressBuffer _instance_data,
                                     ByteAddressBuffer _geometry_data,
                                     ByteAddressBuffer _material_data) {

  GeometryRecord geo_record = (GeometryRecord)0;

  geo_record.instance = Moer::LoadInstanceData(
      _instance_data, _instance_idx * sizeof(Moer::InstanceData));
  geo_record.geometry = Moer::LoadGeometryData(
      _geometry_data, (_geometry_idx + geo_record.instance.first_geom_idx) *
                                          sizeof(Moer::GeometryData));
  geo_record.material = UnpackMaterialData<MaterialData>(
      _material_data, geo_record.geometry.GetMaterialIdx());

  ArrayBuffer vtx_buffer =
      ArrayBuffer(geo_record.geometry.vertex_buffer_handle);
  ArrayBuffer idx_buffer = ArrayBuffer(geo_record.geometry.index_buffer_handle);

  float3 barycentrics;
  barycentrics.x = 1.f - _ray_barycentrics.x - _ray_barycentrics.y;
  barycentrics.yz = _ray_barycentrics;

  uint3 indices =
      idx_buffer.Load<uint3>(_prim_idx, geo_record.geometry.index_offset);

  if (_attrib & EGA_Position) {
    geo_record.vtx_positions[0] =
        vtx_buffer.Load<float3>(indices.x, geo_record.geometry.vertex_offset);
    geo_record.vtx_positions[1] =
        vtx_buffer.Load<float3>(indices.y, geo_record.geometry.vertex_offset);
    geo_record.vtx_positions[2] =
        vtx_buffer.Load<float3>(indices.z, geo_record.geometry.vertex_offset);

    geo_record.model_pos =
        Moer::Interpolate(geo_record.vtx_positions, barycentrics);
  }

  if (_attrib & EGA_PrevPosition &&
      geo_record.geometry.prev_vertex_offset != ~0u) {

    float3 prev_positions[3];
    prev_positions[0] = vtx_buffer.Load<float3>(
        indices.x, geo_record.geometry.prev_vertex_offset);
    prev_positions[1] = vtx_buffer.Load<float3>(
        indices.y, geo_record.geometry.prev_vertex_offset);
    prev_positions[2] = vtx_buffer.Load<float3>(
        indices.z, geo_record.geometry.prev_vertex_offset);

    geo_record.model_pos_prev = Moer::Interpolate(prev_positions, barycentrics);
  } else {
    geo_record.model_pos_prev = geo_record.model_pos;
  }

  if (_attrib & EGA_UV) {
    geo_record.vtx_uvs[0] = vtx_buffer.Load<float2>(
        indices.x, geo_record.geometry.texcoord0_offset);
    geo_record.vtx_uvs[1] = vtx_buffer.Load<float2>(
        indices.y, geo_record.geometry.texcoord0_offset);
    geo_record.vtx_uvs[2] = vtx_buffer.Load<float2>(
        indices.z, geo_record.geometry.texcoord0_offset);

    geo_record.texcoord = Moer::Interpolate(geo_record.vtx_uvs, barycentrics);
  }

  if (_attrib & EGA_Normal) {

    float3 normals[3];

    normals[0] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.x, geo_record.geometry.normal_offset));
    normals[1] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.y, geo_record.geometry.normal_offset));
    normals[2] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.z, geo_record.geometry.normal_offset));

    float3 local_normal = Moer::Interpolate(normals, barycentrics);
    
    geo_record.normal = normalize(mul((float3x3)geo_record.instance.model2world, local_normal));
  }

  if (_attrib & EGA_Tangent) {

    float3 tangents[3];

    tangents[0] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.x, geo_record.geometry.tangent_offset));
    tangents[1] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.y, geo_record.geometry.tangent_offset));
    tangents[2] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.z, geo_record.geometry.tangent_offset));

    geo_record.tangent = Moer::Interpolate(tangents, barycentrics);
    geo_record.tangent = normalize(mul((float3x3)geo_record.instance.model2world, geo_record.tangent));
  }

  return geo_record;
}

MaterialSample SampleGeometryMaterial(GeometryRecord _geo_record,
                                      float2 _grad_x, float2 _grad_y,
                                      float _mip, EMaterialAttribute _attribs) {

  float4 base_color = 0.f;
  float4 emissive = 0.f;
  float4 normal = 0.f;
  float4 materlic_roughness = 0.f;
  float4 transmission = 0.f;

  bool has_base_color =
      _attribs & EMA_BaseColor && _geo_record.material.albedo_map != 0;
  if (has_base_color) {
    TextureHandle albedo_tex = TextureHandle(_geo_record.material.albedo_map);
    if (_mip >= 0.f) {
      base_color = albedo_tex.SampleLevel<float4>(_geo_record.texcoord, _mip);
    } else {
      base_color =
          albedo_tex.SampleGrad<float4>(_geo_record.texcoord, _grad_x, _grad_y);
    }
  }

  if (_attribs & EMA_Emissive && _geo_record.material.emissive_map != 0) {
    TextureHandle emissive_tex =
        TextureHandle(_geo_record.material.emissive_map);
    if (_mip >= 0.f) {
      emissive = emissive_tex.SampleLevel<float4>(_geo_record.texcoord, _mip);
    } else {
      emissive = emissive_tex.SampleGrad<float4>(_geo_record.texcoord, _grad_x,
                                                 _grad_y);
    }
  }

  if (_attribs & EMA_Normal && _geo_record.material.normal_map != 0) {
    TextureHandle normal_tex = TextureHandle(_geo_record.material.normal_map);
    if (_mip >= 0.f) {
      normal = normal_tex.SampleLevel<float4>(_geo_record.texcoord, _mip);
    } else {
      normal =
          normal_tex.SampleGrad<float4>(_geo_record.texcoord, _grad_x, _grad_y);
    }
  }

  if (_attribs & EMA_MetalRough &&
      _geo_record.material.metallic_roughness_map != 0) {
    TextureHandle metal_rough_tex =
        TextureHandle(_geo_record.material.metallic_roughness_map);
    if (_mip >= 0.f) {
      materlic_roughness =
          metal_rough_tex.SampleLevel<float4>(_geo_record.texcoord, _mip);
    } else {
      materlic_roughness = metal_rough_tex.SampleGrad<float4>(
          _geo_record.texcoord, _grad_x, _grad_y);
    }
  }

  // if(_attribs & EMA_Transmission && _geo_record.material.transmission_map !=
  // 0){
  //     TextureHandle transmission_tex = TextureHandle(_hit

  /////////////////////////////////////////////////
  // Material evaluation, use material flags in the future
  /////////////////////////////////////////////////

  MaterialSample result = MaterialSample::ConstructDefault();
  // currently use geometry normal
  result.normal = _geo_record.normal;

  {
    // todo: add specular glossiness workflow
  }

  // use metallic roughness workflow

  result.base_color =
      has_base_color
          ? base_color.xyz * _geo_record.material.base_color_factor.xyz
          : _geo_record.material.base_color_factor.xyz;
  result.roughness = _geo_record.material.roughness_factor;
  result.metalness = _geo_record.material.metallic_factor;

  result.emissive = _geo_record.material.emissive_factor;


  result.occlusion = 1.f;
  result.opacity = 1.f; // todo: support opacity map and factor
  result.transmission = 0.f;

  float emission_level = STL::Color::Luminance(result.emissive);
  emission_level = saturate(emission_level * 50.0);

  result.metalness = lerp(result.metalness, 0.0, emission_level);
  result.roughness = lerp(result.roughness, 1.0, emission_level);

  STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(
      result.base_color, result.metalness, result.diffuse_albedo,
      result.specular_f0);
  //   if(_geo_record.geometry.GetMaterialIdx() != 1)
  //   printf("material_id %d base_color %f %f %f matalic %f roughness %f
  //   emissive %f %f %f\n", _geo_record.geometry.GetMaterialIdx(),
  //   result.base_color.x, result.base_color.y, result.base_color.z,
  //   result.metalness, result.roughness,
  //          result.emissive.x, result.emissive.y, result.emissive.z);

  return result;
}

// assume ray has already intersected target triangle
float3 RayIntersectBarycentrics(float3 _ray_origin, float3 _ray_dir, float3 _v0,
                                float3 _v1, float3 _v2) {

  float3 e1 = _v1 - _v0;
  float3 e2 = _v2 - _v0;

  float3 h = cross(_ray_dir, e2);
  float a = dot(e1, h);
  float f = 1.f / a;

  float3 s = _ray_origin - _v0;
  float u = f * dot(s, h);

  float3 q = cross(s, e1);
  float v = f * dot(_ray_dir, q);

  return float3(1.f - u - v, u, v);
}

// struct Surface {
//   float3 pos_w;
//   float3 view_dir;
//   float view_depth;
//   float3 normal;
//   float3 albedo;
//   float3 f0;
//   float roughness;
//   float diffuse_prob;
// };
} // namespace Moer
#endif