#include "framework/Common.hlsl"
#define MAX_MESHLET_COUNT uint(1024 * 1024 * 8)

struct TaskInput {
  uint instance_count;
  uint counter_buffer_offset;
};

struct CameraCullData {
  CameraData camera_data;
  float4 planes[6]; // world space planes
};

[[vk::push_constant]] ConstantBuffer<TaskInput> input;

[[vk::binding(0, 0)]] ConstantBuffer<CameraCullData> cull_data : register(b0);

[[vk::binding(0, 1)]] StructuredBuffer<InstanceData> instance_data
    : register(t0, space0);
[[vk::binding(1, 1)]] StructuredBuffer<InstanceMeshletInfo>
    instance_meshlet_info : register(t1, space0);

[[vk::binding(0, 2)]] RWStructuredBuffer<InstanceMeshletCullInfo>
    instance_meshlet_cull_info : register(u0, space0);

// 0 for processed instance, 1 for processed meshlet
[[vk::binding(0, 3)]] RWByteAddressBuffer counters_buffer
    : register(u1, space1);

bool IsInstanceVisible(in InstanceMeshletInfo instance) {
  // frustum cull use aabb

  float3 min_pos = instance.center - instance.extent;
  float3 max_pos = instance.center + instance.extent;

  for (uint i = 0; i < 6; i++) {
    float3 normal = cull_data.planes[i].xyz;
    float3 pos = normal * cull_data.planes[i].w;
    float3 n = float3(normal.x > 0 ? min_pos.x : max_pos.x,
                      normal.y > 0 ? min_pos.y : max_pos.y,
                      normal.z > 0 ? min_pos.z : max_pos.z);
    float3 p = float3(normal.x > 0 ? max_pos.x : min_pos.x,
                      normal.y > 0 ? max_pos.y : min_pos.y,
                      normal.z > 0 ? max_pos.z : min_pos.z);
    float d = dot(n, normal);
    float d1 = dot(p, normal);
    float d2 = dot(pos, normal);
    if (d2 + d < 0) {
      return false;
    }
  }
  return true;
}

[numthreads(64, 1, 1)] void main(uint3 dtid
                                 : SV_DispatchThreadID) {
  // first process instance
  uint instance_start_offset = dtid;
  if (instance_start_offset >= input.instance_count) {
    return;
  }
  InstanceMeshletInfo instance_mesh_info =
      instance_meshlet_info[instance_start_offset];

  bool visible = IsInstanceVisible(instance_mesh_info);

  uint instance_count = WaveActiveCountBits(visible);
  uint lane_offset = WavePrefixCountBits(visible);


  uint culled_meshlet_count = visible ? instance_mesh_info.meshlet_count : 0;

  uint total_culled_meshlet_count = WaveActiveSum(culled_meshlet_count);

  uint cull_meshlet_offset;
  if (WaveIsFirstLane()) {
    counters_buffer.InterlockedAdd(input.counter_buffer_offset,
                                   total_culled_meshlet_count,
                                   cull_meshlet_offset);
  }
  cull_meshlet_offset = WaveReadLaneFirst(cull_meshlet_offset);
  cull_meshlet_offset += WavePrefixSum(culled_meshlet_count);

  if (visible) {
    for (uint i = 0; i < culled_meshlet_count; i++) {
      InstanceMeshletCullInfo cull_info;
      uint meshlet_id = instance_mesh_info.meshlet_offset + i;
      InstanceMeshletInfo instance_mesh_info =
          instance_meshlet_info[instance_start_offset];

      cull_info.instance_id = instance_start_offset;
      cull_info.meshlet_id = meshlet_id;
      instance_meshlet_cull_info[cull_meshlet_offset + i] = cull_info;
    }
  }

  // Dispatch meshlet
}