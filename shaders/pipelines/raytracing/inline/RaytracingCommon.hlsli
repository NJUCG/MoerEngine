#ifndef MOER_RAYTRACING_COMMON_HLSLI
#define MOER_RAYTRACING_COMMON_HLSLI

#include "shared/scene/SharedSceneStruct.h"

namespace Moer {
enum EGeometryAttrib {
  EGA_Position = 0x1,
  EGA_PrevPosition = 0x2, // TODO: 暂不支持 prev position
  EGA_UV = 0x4,
  EGA_Normal = 0x8,
  EGA_Tangent = 0x10,
  EGA_All = 0x1f
};

// RT 命中记录
struct GeometryRecord {
  Moer::GInstance  instance;
  Moer::GPrimitive primitive;
  Moer::GMaterial  material;

  float3 vtx_positions[3]; // 三角形三个顶点的模型空间位置
  float2 vtx_uvs[3];       // 三角形三个顶点的 UV 坐标

  float3 model_pos;         // 重心坐标插值后的模型空间位置
  float3 model_pos_prev;    // 上一帧的模型空间位置（TODO: 暂时等于 model_pos）
  float2 texcoord;          // 重心坐标插值后的 UV
  float3 normal;            // 世界空间法线
  float3 tangent;           // 世界空间切线
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

GeometryRecord GetGeometryRecordFrom(
    Moer::GBufferPassParams _param,   // push constant（bindless handles：instance_buf, primitive_buf, material_buf, MegaBuffers）
    uint _instance_idx,                // = InstanceID()，GInstance[] 索引
    uint _prim_idx,                    // 三角形在该 Primitive 内的索引（0-based）
    float2 _ray_barycentrics,          // 命中重心坐标 (u, v)
    EGeometryAttrib _attrib,            // 需要读取的顶点属性掩码
    bool _b_backface = false) {

  GeometryRecord geo_record = (GeometryRecord)0;

  // 1. 加载 GInstance → 获取 world_transform 和 primitive_id
  // 新架构：_instance_idx 直接对应 GInstance[] 索引（因为 TLAS 构建时每个 CPrimitive 对应一个 Instance）
  ArrayBuffer instance_buf = ArrayBuffer(_param.instance_buf_hdl);
  geo_record.instance = instance_buf.Load<Moer::GInstance>(_instance_idx);
  uint primitive_id = geo_record.instance.primitive_id;

  // 2. 加载 GPrimitive → 获取 material_idx 和 MegaBuffer 起始索引
  ArrayBuffer primitive_buf = ArrayBuffer(_param.primitive_buf_hdl);
  geo_record.primitive = primitive_buf.Load<Moer::GPrimitive>(primitive_id);

  // 3. 加载 GMaterial → 获取材质参数和纹理 handles
  ArrayBuffer material_buf = ArrayBuffer(_param.material_buf_hdl);
  geo_record.material = material_buf.Load<Moer::GMaterial>(geo_record.primitive.material_idx);

  // 4. 从 MegaBuffer 读取三角形索引
  ArrayBuffer index_buf = ArrayBuffer(_param.index_buf_hdl);
  uint index_start = geo_record.primitive.index_start_idx; // in uint
  uint3 indices;
  indices.x = index_buf.Load<uint>(index_start + _prim_idx * 3);
  indices.y = index_buf.Load<uint>(index_start + _prim_idx * 3 + 1);
  indices.z = index_buf.Load<uint>(index_start + _prim_idx * 3 + 2);

  // 5. 计算重心坐标
  float3 barycentrics;
  barycentrics.x = 1.f - _ray_barycentrics.x - _ray_barycentrics.y;
  barycentrics.yz = _ray_barycentrics;

  // 6. 从 MegaBuffers 读取顶点属性
  float4x4 model2world = geo_record.instance.world_transform;

  if (_attrib & EGA_Position) {
    if (geo_record.primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Position) {
      ArrayBuffer position_buf = ArrayBuffer(_param.position_buf_hdl);
      uint pos_start = geo_record.primitive.position_start_idx;
      geo_record.vtx_positions[0] = position_buf.Load<float3>(pos_start + indices.x);
      geo_record.vtx_positions[1] = position_buf.Load<float3>(pos_start + indices.y);
      geo_record.vtx_positions[2] = position_buf.Load<float3>(pos_start + indices.z);
    }
    geo_record.model_pos = Moer::Interpolate(geo_record.vtx_positions, barycentrics);
  }

  // PrevPosition: 新架构暂不支持上一帧顶点位置，使用当前帧位置
  if (_attrib & EGA_PrevPosition) {
    geo_record.model_pos_prev = geo_record.model_pos;
  }

  if (_attrib & EGA_UV) {
    if (geo_record.primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::Texcoord0) {
      ArrayBuffer texcoord_buf = ArrayBuffer(_param.texcoord0_buf_hdl);
      uint uv_start = geo_record.primitive.texcoord0_start_idx;
      geo_record.vtx_uvs[0] = texcoord_buf.Load<float2>(uv_start + indices.x);
      geo_record.vtx_uvs[1] = texcoord_buf.Load<float2>(uv_start + indices.y);
      geo_record.vtx_uvs[2] = texcoord_buf.Load<float2>(uv_start + indices.z);
    }
    geo_record.texcoord = Moer::Interpolate(geo_record.vtx_uvs, barycentrics);
  }

  if (_attrib & EGA_Normal) {
    if (geo_record.primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::PackedNormal) {
      ArrayBuffer normal_buf = ArrayBuffer(_param.packed_normal_buf_hdl);
      uint normal_start = geo_record.primitive.packed_normal_start_idx;

      float3 normals[3];
      normals[0] = Moer::Unpack_Normal(normal_buf.Load<uint>(normal_start + indices.x));
      normals[1] = Moer::Unpack_Normal(normal_buf.Load<uint>(normal_start + indices.y));
      normals[2] = Moer::Unpack_Normal(normal_buf.Load<uint>(normal_start + indices.z));

      float3 local_normal = Moer::Interpolate(normals, barycentrics);
      geo_record.normal = normalize(mul(model2world, float4(local_normal, 0.f)));

      if (_b_backface) {
        geo_record.normal = -geo_record.normal;
      }
    }
  }

  if (_attrib & EGA_Tangent) {
    if (geo_record.primitive.attribute_mask & Moer::GPrimitiveEAttributeMask::PackedTangent) {
      ArrayBuffer tangent_buf = ArrayBuffer(_param.packed_tangent_buf_hdl);
      uint tangent_start = geo_record.primitive.packed_tangent_start_idx;

      float3 tangents[3];
      tangents[0] = Moer::Unpack_Normal(tangent_buf.Load<uint>(tangent_start + indices.x));
      tangents[1] = Moer::Unpack_Normal(tangent_buf.Load<uint>(tangent_start + indices.y));
      tangents[2] = Moer::Unpack_Normal(tangent_buf.Load<uint>(tangent_start + indices.z));

      geo_record.tangent = Moer::Interpolate(tangents, barycentrics);
      geo_record.tangent = normalize(mul(model2world, float4(geo_record.tangent, 0.f)));
    }
  }

  return geo_record;
}

// 将切线空间法线转换到世界空间
float3 ApplyNormal(float3 _normal, float3 _geom_normal, float3 _tangent) {
  float3 T = normalize(_tangent);
  float3 N = normalize(_geom_normal);
  float3 B = cross(T, N);
  return normalize(_normal.x * T + _normal.y * B + _normal.z * N);
}


MaterialSample SampleGeometryMaterial(GeometryRecord _geo_record,
                                      float2 _grad_x, float2 _grad_y,
                                      float _mip, EMaterialAttribute _attribs) {

  Moer::GMaterial mat = _geo_record.material;

  float4 base_color = 1.f;
  float4 emissive = float4(mat.emissive_factor, 1.f);
  float4 normal = float4(_geo_record.normal, 0.f);
  float4 metallic_roughness = float4(0.f, mat.roughness_factor,
                                     mat.metallic_factor, 0.f);
  float4 transmission = 0.f;

  // Albedo / BaseColor 纹理
  bool has_base_color = _attribs & EMA_BaseColor && mat.albedo_map_hdl > 0;
  if (has_base_color) {
    TextureHandle albedo_tex = TextureHandle(mat.albedo_map_hdl);
    if (_mip >= 0.f) {
      base_color = albedo_tex.SampleLevel(_geo_record.texcoord, _mip);
    } else {
      base_color = albedo_tex.SampleGrad<float4>(_geo_record.texcoord, _grad_x, _grad_y);
    }
  }

  // Emissive 纹理
  if (_attribs & EMA_Emissive && mat.emissive_map_hdl > 0) {
    TextureHandle emissive_tex = TextureHandle(mat.emissive_map_hdl);
    if (_mip >= 0.f) {
      emissive *= emissive_tex.SampleLevel<float4>(_geo_record.texcoord, _mip);
    } else {
      emissive *= emissive_tex.SampleGrad<float4>(_geo_record.texcoord, _grad_x, _grad_y);
    }
    emissive.xyz = pow(emissive.xyz, 2.2);
  }

  // Normal 纹理
  if (_attribs & EMA_Normal && mat.normal_map_hdl > 0) {
    TextureHandle normal_tex = TextureHandle(mat.normal_map_hdl);
    if (_mip >= 0.f) {
      normal = normal_tex.SampleLevel<float4>(_geo_record.texcoord, _mip);
    } else {
      normal = normal_tex.SampleGrad<float4>(_geo_record.texcoord, _grad_x, _grad_y);
    }
    normal.xyz = 2.f * normal.xyz - 1.f;
    normal.xyz = normalize(normal.xyz);
    normal.xyz = ApplyNormal(normal.xyz, _geo_record.normal, _geo_record.tangent);
  }

  // MetallicRoughness 纹理
  if (_attribs & EMA_MetalRough && mat.metallic_roughness_map_hdl > 0) {
    TextureHandle metal_rough_tex = TextureHandle(mat.metallic_roughness_map_hdl);
    if (_mip >= 0.f) {
      metallic_roughness = metal_rough_tex.SampleLevel<float4>(_geo_record.texcoord, _mip);
    } else {
      metallic_roughness = metal_rough_tex.SampleGrad<float4>(_geo_record.texcoord, _grad_x, _grad_y);
      metallic_roughness.xyz = pow(metallic_roughness.xyz, 2.2);
    }
  }

  /////////////////////////////////////////////////
  // Material evaluation
  /////////////////////////////////////////////////

  MaterialSample result = MaterialSample::ConstructDefault();
  result.normal = normalize(normal.xyz);

  // albedo_factor.xyz 对应旧架构的 base_color_factor.xyz
  result.base_color = base_color.xyz * mat.albedo_factor.xyz;

  result.roughness = metallic_roughness.g;
  result.metalness = metallic_roughness.b;

  result.emissive = emissive.xyz;

  result.occlusion = 1.f;
  result.opacity = 1.f; // TODO: support alpha_mode / alpha_cutoff
  result.transmission = 0.f;

  STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(
      result.base_color, result.metalness, result.diffuse_albedo,
      result.specular_f0);
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