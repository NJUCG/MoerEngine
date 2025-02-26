#include <framework/Bindless.hlsl>
#include <framework/Common.hlsl>
#include <shared/Geometry.h>
#include <shared/lighting/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>

BINDLESS_BINDINGS(3, 2, 4, 5)
#include <framework/Material.hlsl>
#include <framework/PolymorphicLight.hlsli>
#include <shared/utils/Packing.h>

[[vk::push_constant]] ConstantBuffer<Moer::PrepareLightsParams> param;

[[vk::binding(0, 0)]] RWStructuredBuffer<Moer::PolymorphicLightInfo> light_data
    : register(u0);
[[vk::binding(1, 0)]] RWBuffer<uint> light_index_mapping : register(u1);

[[vk::binding(2, 0)]] RWTexture2D<float> local_light_pdf;

[[vk::binding(3, 0)]] StructuredBuffer<Moer::PolymorphicLightInfo> prim_lights;
[[vk::binding(4, 0)]] StructuredBuffer<Moer::PrepareLightsTask> tasks;

bool FindTask(uint dtid, out Moer::PrepareLightsTask task) {
  // binary search in task buffer
  int left = 0;
  int right = int(param.num_tasks) - 1;

  while (left <= right) {
    int mid = (left + right) / 2;
    task = tasks[mid];
    int tri = int(dtid) - int(task.light_offset);
    if (tri < 0) {
      right = mid - 1;
    } else if (tri >= int(task.num_triangles)) {
      left = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

[numthreads(256, 1, 1)] void main(uint dtid
                                  : SV_DispatchThreadID, uint gtid
                                  : SV_GroupThreadID) {
  Moer::PrepareLightsTask task = (Moer::PrepareLightsTask)0;
  if (!FindTask(dtid, task)) {
    return;
  }

  uint tri_idx = dtid - task.light_offset;
  bool is_prim_light =
      (task.instance_geo_idx & Moer::g_task_prim_light_bit) != 0;

  Moer::PolymorphicLightInfo light_info = (Moer::PolymorphicLightInfo)0;

  if (is_prim_light) {
    uint prim_light_idx = task.instance_geo_idx & ~Moer::g_task_prim_light_bit;
    light_info = prim_lights[prim_light_idx];
    uint type = Moer::GetLightType(light_info);
  } else {
    ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data_handle);
    ArrayBuffer geom_data_array = ArrayBuffer(param.geometry_data_handle);

    Moer::InstanceData inst = Moer::LoadInstanceData(
        instance_data_array.GetByteAddressBuffer(),
        (task.instance_geo_idx >> 12) * sizeof(Moer::InstanceData));

    Moer::GeometryData geom = Moer::LoadGeometryData(
        geom_data_array.GetByteAddressBuffer(),
        (inst.first_geom_idx + (task.instance_geo_idx & 0xfff)) *
            sizeof(Moer::GeometryData));

    ArrayBuffer vtx_buffer = ArrayBuffer(geom.vertex_buffer_handle);
    ArrayBuffer idx_buffer = ArrayBuffer(geom.index_buffer_handle);

    MaterialData mat = UnpackMaterialData<MaterialData>(
        param.material_data_handle, geom.mat_idx_and_type >> 8);

    uint3 idx = idx_buffer.Load<uint3>(tri_idx, geom.index_offset);

    float3 positions[3];

    positions[0] = vtx_buffer.Load<float3>(idx.x, geom.vertex_offset);
    positions[1] = vtx_buffer.Load<float3>(idx.y, geom.vertex_offset);
    positions[2] = vtx_buffer.Load<float3>(idx.z, geom.vertex_offset);

    positions[0] = mul(inst.model2world, float4(positions[0], 1.0f)).xyz;
    positions[1] = mul(inst.model2world, float4(positions[1], 1.0f)).xyz;
    positions[2] = mul(inst.model2world, float4(positions[2], 1.0f)).xyz;

    float3 emissive = mat.emissive_factor;

    // TODO: handle emissive texture

    emissive.rgb = max(emissive.rgb, 0.0f);
    Moer::TriangleLight tri_light = (Moer::TriangleLight)0;
    tri_light.v0 = positions[0];
    tri_light.edge1 = positions[1] - positions[0];
    tri_light.edge2 = positions[2] - positions[0];
    tri_light.radiance = emissive;

    light_info = tri_light.ToLightInfo();
  }

  uint light_buf_idx = task.light_offset + tri_idx;
  light_data[param.cur_light_offset + light_buf_idx] = light_info;

  if (task.prev_light_offset >= 0) {
    uint prev_light_buf_idx = task.prev_light_offset + tri_idx;
    light_index_mapping[prev_light_buf_idx + param.prev_light_offset] =
        light_buf_idx + param.cur_light_offset + 1;

    light_index_mapping[light_buf_idx + param.cur_light_offset] =
        prev_light_buf_idx + param.prev_light_offset + 1;
  }

  float emissive_flux = Moer::PolymorphicLight::GetPower(light_info);
  uint2 pdf_position = Math::LinearIndexToZCurve(light_buf_idx);
  local_light_pdf[pdf_position] = emissive_flux;
}